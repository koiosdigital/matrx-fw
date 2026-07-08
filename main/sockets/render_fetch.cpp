#include "render_fetch.h"
#include "sockets.h"
#include "apps.h"
#include "scheduler.h"

#include <cstring>
#include <cstdio>

#include <esp_log.h>
#include <esp_http_client.h>
#include <esp_heap_caps.h>
#include <kd_http.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>

static const char* TAG = "render_fetch";

namespace {

    constexpr size_t QUEUE_DEPTH = 16;
    constexpr size_t MAX_RENDER_SIZE = 512 * 1024;
    constexpr size_t INITIAL_BUF_SIZE = 32 * 1024;
    constexpr uint32_t WORKER_STACK = 10240;
    constexpr UBaseType_t WORKER_PRIO = 10;
    constexpr BaseType_t WORKER_CORE = 0;

    struct FetchRequest {
        uint8_t uuid[16];
    };

    QueueHandle_t g_queue = nullptr;

    SemaphoreHandle_t g_pending_mutex = nullptr;
    uint8_t g_pending[QUEUE_DEPTH][16];
    size_t g_pending_count = 0;

    char g_resp_etag[APP_ETAG_MAX];

    bool pending_add(const uint8_t* uuid) {
        if (xSemaphoreTake(g_pending_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return false;
        bool added = false;
        bool found = false;
        for (size_t i = 0; i < g_pending_count; i++) {
            if (memcmp(g_pending[i], uuid, 16) == 0) { found = true; break; }
        }
        if (!found && g_pending_count < QUEUE_DEPTH) {
            memcpy(g_pending[g_pending_count++], uuid, 16);
            added = true;
        }
        xSemaphoreGive(g_pending_mutex);
        return added;
    }

    void pending_remove(const uint8_t* uuid) {
        if (xSemaphoreTake(g_pending_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) return;
        for (size_t i = 0; i < g_pending_count; i++) {
            if (memcmp(g_pending[i], uuid, 16) == 0) {
                memmove(g_pending[i], g_pending[i + 1], (g_pending_count - i - 1) * 16);
                g_pending_count--;
                break;
            }
        }
        xSemaphoreGive(g_pending_mutex);
    }

    void uuid_to_str(const uint8_t* u, char* out, size_t out_size) {
        snprintf(out, out_size,
            "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
            u[0], u[1], u[2], u[3], u[4], u[5], u[6], u[7],
            u[8], u[9], u[10], u[11], u[12], u[13], u[14], u[15]);
    }

    esp_err_t http_event_handler(esp_http_client_event_t* evt) {
        if (evt->event_id == HTTP_EVENT_ON_HEADER && evt->header_key && evt->header_value) {
            if (strcasecmp(evt->header_key, "ETag") == 0) {
                strlcpy(g_resp_etag, evt->header_value, sizeof(g_resp_etag));
            }
        }
        return ESP_OK;
    }

    esp_err_t attempt(const char* url, const char* auth, const char* if_none_match,
        int* status_out, uint8_t** body_out, size_t* body_len_out) {
        *status_out = 0;
        *body_out = nullptr;
        *body_len_out = 0;

        esp_http_client_handle_t client = kd_http_acquire(url, http_event_handler, nullptr, 30000);
        if (!client) return ESP_ERR_TIMEOUT;
        kd_http_set_header("Authorization", auth);
        if (if_none_match && if_none_match[0]) {
            kd_http_set_header("If-None-Match", if_none_match);
        }
        g_resp_etag[0] = '\0';

        esp_err_t err = esp_http_client_open(client, 0);
        if (err != ESP_OK) {
            kd_http_invalidate();
            kd_http_release();
            return ESP_FAIL;
        }

        int64_t content_length = esp_http_client_fetch_headers(client);
        if (content_length < 0) {
            kd_http_invalidate();
            kd_http_release();
            return ESP_FAIL;
        }

        int status = esp_http_client_get_status_code(client);

        if (status == 200) {
            if (content_length > (int64_t)MAX_RENDER_SIZE) {
                ESP_LOGE(TAG, "Render too large: %lld bytes", content_length);
                esp_http_client_close(client);
                kd_http_release();
                *status_out = status;
                return ESP_ERR_INVALID_SIZE;
            }
            size_t cap = (content_length > 0) ? (size_t)content_length + 1 : INITIAL_BUF_SIZE;
            auto* buf = static_cast<uint8_t*>(heap_caps_malloc(cap, MALLOC_CAP_SPIRAM));
            if (!buf) {
                esp_http_client_close(client);
                kd_http_release();
                return ESP_ERR_NO_MEM;
            }
            size_t len = 0;
            while (true) {
                if (len == cap) {
                    if (cap >= MAX_RENDER_SIZE) {
                        ESP_LOGE(TAG, "Render exceeds %zu bytes, aborting", MAX_RENDER_SIZE);
                        heap_caps_free(buf);
                        esp_http_client_close(client);
                        kd_http_release();
                        *status_out = status;
                        return ESP_ERR_INVALID_SIZE;
                    }
                    size_t new_cap = (cap * 2 > MAX_RENDER_SIZE) ? MAX_RENDER_SIZE : cap * 2;
                    auto* grown = static_cast<uint8_t*>(
                        heap_caps_realloc(buf, new_cap, MALLOC_CAP_SPIRAM));
                    if (!grown) {
                        heap_caps_free(buf);
                        esp_http_client_close(client);
                        kd_http_release();
                        return ESP_ERR_NO_MEM;
                    }
                    buf = grown;
                    cap = new_cap;
                }
                int r = esp_http_client_read(client, reinterpret_cast<char*>(buf) + len, cap - len);
                if (r < 0) {
                    heap_caps_free(buf);
                    kd_http_invalidate();
                    kd_http_release();
                    return ESP_FAIL;
                }
                if (r == 0) break;
                len += r;
            }
            *body_out = buf;
            *body_len_out = len;
        }
        else {
            char drain[256];
            while (esp_http_client_read(client, drain, sizeof(drain)) > 0) {}
        }

        kd_http_release();
        *status_out = status;
        return ESP_OK;
    }

    void report(const uint8_t* uuid, bool success, bool displayable) {
        scheduler_on_render_response(uuid, success, displayable);
    }

    void do_fetch(const uint8_t* uuid) {
        App_t* app = app_find(uuid);
        if (!app) return;
        bool displayable = app->displayable;

        char uuid_str[37];
        uuid_to_str(uuid, uuid_str, sizeof(uuid_str));

        char* token = sockets_get_device_token_copy();
        if (!token) {
            ESP_LOGE(TAG, "Fetch %.8s deferred: no device token yet", uuid_str);
            report(uuid, false, displayable);
            return;
        }

        char url[160];
        snprintf(url, sizeof(url), RENDER_API_BASE_URL "/d/v1/installations/%s/render.webp", uuid_str);

        size_t auth_len = strlen("Bearer ") + strlen(token) + 1;
        auto* auth = static_cast<char*>(heap_caps_malloc(auth_len, MALLOC_CAP_SPIRAM));
        if (!auth) {
            heap_caps_free(token);
            report(uuid, false, displayable);
            return;
        }
        snprintf(auth, auth_len, "Bearer %s", token);
        heap_caps_free(token);

        char if_none_match[APP_ETAG_MAX];
        app_copy_etag_if_has_data(app, if_none_match, sizeof(if_none_match));

        int status = 0;
        uint8_t* body = nullptr;
        size_t body_len = 0;
        esp_err_t err = attempt(url, auth, if_none_match, &status, &body, &body_len);
        if (err == ESP_FAIL) {
            err = attempt(url, auth, if_none_match, &status, &body, &body_len);
        }
        heap_caps_free(auth);

        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Fetch %.8s: failed (%s)", uuid_str, esp_err_to_name(err));
            report(uuid, false, displayable);
            return;
        }

        app = app_find(uuid);
        if (!app) {
            ESP_LOGW(TAG, "Fetch %.8s: dropped, app removed from schedule", uuid_str);
            heap_caps_free(body);
            return;
        }

        switch (status) {
        case 200:
            if (body_len == 0) {
                app_clear_data(app);
                app_set_displayable(app, true);
                report(uuid, true, true);
            }
            else {
                app_set_data(app, body, body_len);
                app_set_etag(app, g_resp_etag[0] ? g_resp_etag : nullptr);
                app_set_displayable(app, true);
                report(uuid, true, true);
            }
            break;

        case 304:
            report(uuid, true, app->displayable);
            break;

        case 204:
            app_clear_data(app);
            app_set_displayable(app, true);
            report(uuid, true, true);
            break;

        case 404:
            ESP_LOGW(TAG, "Fetch %.8s: 404, not renderable", uuid_str);
            app_clear_data(app);
            app_set_displayable(app, false);
            report(uuid, true, false);
            break;

        case 401:
        case 403:
            ESP_LOGW(TAG, "Fetch %.8s: %d unauthorized, requesting token refresh", uuid_str, status);
            sockets_request_token_refresh();
            report(uuid, false, app->displayable);
            break;

        default:
            ESP_LOGW(TAG, "Fetch %.8s: HTTP %d", uuid_str, status);
            report(uuid, false, app->displayable);
            break;
        }

        heap_caps_free(body);
    }

    void worker(void*) {
        FetchRequest req;
        for (;;) {
            if (xQueueReceive(g_queue, &req, portMAX_DELAY) == pdTRUE) {
                do_fetch(req.uuid);
                pending_remove(req.uuid);
            }
        }
    }

}  // namespace

void render_fetch_init() {
    if (g_queue) return;

    g_pending_mutex = xSemaphoreCreateMutex();
    g_queue = xQueueCreate(QUEUE_DEPTH, sizeof(FetchRequest));
    if (!g_pending_mutex || !g_queue) {
        ESP_LOGE(TAG, "Failed to create queue/mutex");
        return;
    }

    if (xTaskCreatePinnedToCore(worker, "render_fetch", WORKER_STACK, nullptr,
        WORKER_PRIO, nullptr, WORKER_CORE) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create worker task");
    }
}

void render_fetch_request(const uint8_t* uuid16) {
    if (!uuid16 || !g_queue) return;

    if (!pending_add(uuid16)) return;

    FetchRequest req;
    memcpy(req.uuid, uuid16, 16);
    if (xQueueSend(g_queue, &req, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Fetch queue full");
        pending_remove(uuid16);
    }
}

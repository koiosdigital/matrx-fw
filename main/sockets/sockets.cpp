#include "sockets.h"
#include "handlers.h"
#include "messages.h"

#include <ctime>

#include <esp_log.h>
#include <esp_websocket_client.h>
#include <esp_event.h>
#include <esp_wifi.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <esp_crt_bundle.h>
#include <esp_system.h>
#include <esp_random.h>

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include <cJSON.h>
#include <kd_common.h>
#include <kd/v1/matrx.pb-c.h>

#include "apps.h"
#include "scheduler.h"

static const char* TAG = "sockets";

namespace {

    constexpr size_t QUEUE_SIZE = 8;
    constexpr size_t MAX_MSG_SIZE = 16 * 1024;
    constexpr int MAX_SOCK_FAILURES_BEFORE_WIFI_RESET = 8;
    constexpr int MAX_WIFI_RESETS_BEFORE_RESTART = 3;

    void* spiram_alloc(void*, size_t size) {
        return heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
    }

    void spiram_free(void*, void* ptr) {
        heap_caps_free(ptr);
    }

    ProtobufCAllocator spiram_allocator = {
        .alloc = spiram_alloc,
        .free = spiram_free,
        .allocator_data = nullptr,
    };

    enum class State : uint8_t {
        WaitingForNetwork,
        WaitingForCrypto,
        Ready,
        Connected,
    };

    SemaphoreHandle_t client_mutex = nullptr;

    struct {
        QueueHandle_t outbox = nullptr;
        QueueHandle_t inbox = nullptr;
        esp_websocket_client_handle_t client = nullptr;
        State state = State::WaitingForNetwork;
        bool session_ready = false;

        uint8_t* rx_buf = nullptr;
        size_t rx_len = 0;
        size_t rx_expected = 0;
        bool rx_is_text = false;

        void rx_reset() {
            if (rx_buf) { heap_caps_free(rx_buf); rx_buf = nullptr; }
            rx_len = rx_expected = 0;
        }
    } sock;

    bool is_connected() {
        if (!client_mutex) return false;
        if (xSemaphoreTake(client_mutex, pdMS_TO_TICKS(50)) != pdTRUE) return false;
        bool connected = sock.client != nullptr && esp_websocket_client_is_connected(sock.client);
        xSemaphoreGive(client_mutex);
        return connected;
    }

    bool locked_send(const void* data, size_t len, bool binary) {
        if (!client_mutex) return false;
        if (xSemaphoreTake(client_mutex, pdMS_TO_TICKS(6000)) != pdTRUE) return false;
        bool ok = false;
        if (sock.client && esp_websocket_client_is_connected(sock.client)) {
            const char* p = static_cast<const char*>(data);
            int sent = binary
                ? esp_websocket_client_send_bin(sock.client, p, len, pdMS_TO_TICKS(5000))
                : esp_websocket_client_send_text(sock.client, p, len, pdMS_TO_TICKS(5000));
            ok = sent >= 0;
        }
        xSemaphoreGive(client_mutex);
        return ok;
    }

    void destroy_ws_client() {
        esp_websocket_client_handle_t victim = nullptr;
        if (client_mutex && xSemaphoreTake(client_mutex, pdMS_TO_TICKS(6000)) == pdTRUE) {
            victim = sock.client;
            sock.client = nullptr;
            xSemaphoreGive(client_mutex);
        }
        if (victim) {
            esp_websocket_client_destroy(victim);
        }
    }

    int sock_failure_count = 0;
    int wifi_disconnect_count = 0;
    bool disconnect_handled = false;

    QueueHandle_t ctl_queue = nullptr;
    TaskHandle_t ctl_task_handle = nullptr;
    enum class CtlCmd : uint8_t { Reconnect };

    void try_advance_state();
    void process_queues();
    void schedule_reconnect();
    esp_err_t start_client();

    esp_timer_handle_t state_check_timer = nullptr;

    esp_ds_data_ctx_t* static_ds_data_ctx = nullptr;
    char* static_cert = nullptr;
    size_t static_cert_len = 0;

    esp_timer_handle_t reconnect_timer = nullptr;
    constexpr int64_t RECONNECT_BASE_DELAY_US = 3000 * 1000;
    constexpr int64_t RECONNECT_MAX_DELAY_US = 60LL * 1000 * 1000;
    int backoff_level = 0;

    int64_t next_reconnect_delay() {
        int shift = (backoff_level > 5) ? 5 : backoff_level;
        int64_t delay = RECONNECT_BASE_DELAY_US << shift;
        if (delay > RECONNECT_MAX_DELAY_US) delay = RECONNECT_MAX_DELAY_US;
        delay = delay * (80 + (esp_random() % 41)) / 100;
        return delay;
    }

    esp_timer_handle_t schedule_retry_timer = nullptr;
    int schedule_retry_count = 0;
    constexpr int64_t SCHEDULE_RETRY_BASE_US = 10 * 1000 * 1000;
    constexpr int64_t SCHEDULE_RETRY_STEP_US = 10 * 1000 * 1000;
    constexpr int64_t SCHEDULE_RETRY_MAX_US = 30 * 1000 * 1000;

    int64_t next_schedule_retry_delay() {
        int64_t delay = SCHEDULE_RETRY_BASE_US + (schedule_retry_count * SCHEDULE_RETRY_STEP_US);
        return (delay > SCHEDULE_RETRY_MAX_US) ? SCHEDULE_RETRY_MAX_US : delay;
    }

    esp_timer_handle_t welcome_timeout_timer = nullptr;
    constexpr int64_t WELCOME_TIMEOUT_US = 15 * 1000 * 1000;

    SemaphoreHandle_t token_mutex = nullptr;
    char* device_token = nullptr;
    int64_t token_expires_at = 0;

    esp_timer_handle_t token_refresh_timer = nullptr;
    int64_t last_refresh_req_us = 0;
    constexpr int64_t TOKEN_REFRESH_MIN_INTERVAL_US = 30 * 1000 * 1000;
    constexpr int64_t TOKEN_REFRESH_FALLBACK_US = 13LL * 60 * 1000 * 1000;

    bool send_text(const char* text) {
        return locked_send(text, strlen(text), false);
    }

    void store_token(const char* token, int64_t expires_at) {
        if (!token || !token[0]) return;

        if (xSemaphoreTake(token_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) return;
        heap_caps_free(device_token);
        size_t len = strlen(token) + 1;
        device_token = static_cast<char*>(heap_caps_malloc(len, MALLOC_CAP_SPIRAM));
        if (device_token) memcpy(device_token, token, len);
        token_expires_at = expires_at;
        xSemaphoreGive(token_mutex);

        int64_t delay_us = TOKEN_REFRESH_FALLBACK_US;
        time_t now = time(nullptr);
        if (expires_at > 1600000000 && now > 1600000000 && expires_at > now) {
            int64_t margin_s = (expires_at - now) - 120;
            if (margin_s < 30) margin_s = 30;
            if (margin_s > 14 * 60) margin_s = 14 * 60;
            delay_us = margin_s * 1000 * 1000;
        }
        if (token_refresh_timer) {
            esp_timer_stop(token_refresh_timer);
            esp_timer_start_once(token_refresh_timer, delay_us);
        }
    }

    void on_session_ready() {
        if (sock.session_ready) return;
        sock.session_ready = true;
        if (welcome_timeout_timer) esp_timer_stop(welcome_timeout_timer);

        sock_failure_count = 0;
        wifi_disconnect_count = 0;
        backoff_level = 0;

        show_fs_sprite("ready");
        scheduler_on_connect();
        msg_send_device_info();
        msg_send_claim_if_needed();

        schedule_retry_count = 0;
        esp_timer_stop(schedule_retry_timer);
        esp_timer_start_once(schedule_retry_timer, next_schedule_retry_delay());
    }

    void handle_control_frame(const char* data, size_t len) {
        cJSON* root = cJSON_ParseWithLength(data, len);
        if (!root) {
            ESP_LOGW(TAG, "Unparseable control frame (%zu bytes)", len);
            return;
        }

        const char* type = cJSON_GetStringValue(cJSON_GetObjectItem(root, "type"));
        if (!type) {
            ESP_LOGW(TAG, "Control frame without type");
        }
        else if (strcmp(type, "welcome") == 0 || strcmp(type, "token") == 0) {
            const char* token = cJSON_GetStringValue(cJSON_GetObjectItem(root, "token"));
            cJSON* exp = cJSON_GetObjectItem(root, "expires_at");
            store_token(token, cJSON_IsNumber(exp) ? (int64_t)cJSON_GetNumberValue(exp) : 0);

            if (strcmp(type, "welcome") == 0) {
                on_session_ready();
            }
        }
        else if (strcmp(type, "twin.desired") == 0) {
            cJSON* version = cJSON_GetObjectItem(root, "version");
            if (cJSON_IsNumber(version)) {
                char ack[64];
                snprintf(ack, sizeof(ack), "{\"type\":\"twin.desired.ack\",\"version\":%lld}",
                    (long long)cJSON_GetNumberValue(version));
                send_text(ack);
            }
        }
        else if (strcmp(type, "twin.report.ack") == 0) {
        }
        else if (strcmp(type, "twin.error") == 0) {
            const char* msg = cJSON_GetStringValue(cJSON_GetObjectItem(root, "message"));
            ESP_LOGW(TAG, "twin.error: %s", msg ? msg : "?");
        }

        cJSON_Delete(root);
    }

    void token_refresh_timer_callback(void*) {
        sockets_request_token_refresh();
    }

    void welcome_timeout_callback(void*) {
        if (sock.state == State::Connected && !sock.session_ready) {
            ESP_LOGW(TAG, "No welcome frame within %llds, reconnecting",
                WELCOME_TIMEOUT_US / 1000000);
            disconnect_handled = true;
            sock.state = State::Ready;
            backoff_level++;
            show_fs_sprite("connecting");
            schedule_reconnect();
        }
    }

    void process_queues() {
        if (!is_connected()) return;

        QueuedMessage out;
        while (xQueueReceive(sock.outbox, &out, 0) == pdTRUE) {
            if (out.data) {
                locked_send(out.data, out.len, true);
                heap_caps_free(out.data);
            }
        }

        QueuedMessage in;
        while (xQueueReceive(sock.inbox, &in, 0) == pdTRUE) {
            if (in.data) {
                auto* msg = kd__v1__matrx_message__unpack(&spiram_allocator, in.len, in.data);
                if (msg) {
                    handle_message(msg);
                    kd__v1__matrx_message__free_unpacked(msg, &spiram_allocator);
                }
                heap_caps_free(in.data);
            }
        }
    }

    void schedule_retry_callback(void*) {
        if (is_connected() && !scheduler_has_schedule()) {
            schedule_retry_count++;
            int64_t next_delay = next_schedule_retry_delay();
            ESP_LOGW(TAG, "No schedule received, retrying (attempt %d, next in %llds)",
                schedule_retry_count, next_delay / 1000000);
            msg_send_schedule_request();
            esp_timer_start_once(schedule_retry_timer, next_delay);
        }
    }

    void reconnect_timer_callback(void*) {
        CtlCmd cmd = CtlCmd::Reconnect;
        if (ctl_queue) xQueueOverwrite(ctl_queue, &cmd);
    }

    void ctl_task(void*) {
        CtlCmd cmd;
        for (;;) {
            if (xQueueReceive(ctl_queue, &cmd, portMAX_DELAY) != pdTRUE) continue;
            destroy_ws_client();
            if (sock.state == State::Ready) {
                if (start_client() != ESP_OK) {
                    backoff_level++;
                    schedule_reconnect();
                }
            }
        }
    }

    void schedule_reconnect() {
        if (reconnect_timer) {
            int64_t delay_us = next_reconnect_delay();
            esp_timer_stop(reconnect_timer);
            esp_timer_start_once(reconnect_timer, delay_us);
        }
    }

    void ws_event_handler(void*, esp_event_base_t, int32_t event_id, void* event_data) {
        auto* data = static_cast<esp_websocket_event_data_t*>(event_data);

        switch (event_id) {
        case WEBSOCKET_EVENT_CONNECTED:
            sock.state = State::Connected;
            sock.session_ready = false;
            esp_timer_stop(welcome_timeout_timer);
            esp_timer_start_once(welcome_timeout_timer, WELCOME_TIMEOUT_US);
            break;

        case WEBSOCKET_EVENT_DISCONNECTED:
        case WEBSOCKET_EVENT_CLOSED:
        case WEBSOCKET_EVENT_ERROR: {
            ESP_LOGW(TAG, "Disconnected/error (event=%ld)", event_id);
            sock.rx_reset();
            esp_timer_stop(schedule_retry_timer);
            esp_timer_stop(welcome_timeout_timer);

            if (sock.state == State::WaitingForNetwork) break;
            if (disconnect_handled) break;
            disconnect_handled = true;

            const bool was_connected = (sock.state == State::Connected) && sock.session_ready;
            sock.state = State::Ready;
            sock.session_ready = false;

            if (was_connected) {
                scheduler_on_disconnect();
                show_fs_sprite("connecting");
            }

            sock_failure_count++;
            backoff_level++;
            ESP_LOGW(TAG, "Socket failure %d/%d (wifi resets: %d/%d)",
                sock_failure_count, MAX_SOCK_FAILURES_BEFORE_WIFI_RESET,
                wifi_disconnect_count, MAX_WIFI_RESETS_BEFORE_RESTART);

            if (sock_failure_count >= MAX_SOCK_FAILURES_BEFORE_WIFI_RESET) {
                wifi_disconnect_count++;
                sock_failure_count = 0;

                if (wifi_disconnect_count >= MAX_WIFI_RESETS_BEFORE_RESTART) {
                    ESP_LOGE(TAG, "Too many WiFi resets (%d), restarting",
                        wifi_disconnect_count);
                    esp_restart();
                }

                ESP_LOGW(TAG, "Too many socket failures, disconnecting WiFi (%d/%d)",
                    wifi_disconnect_count, MAX_WIFI_RESETS_BEFORE_RESTART);
                kd_common_wifi_disconnect();
            }
            schedule_reconnect();
            break;
        }

        case WEBSOCKET_EVENT_DATA:
            if (data->op_code >= 0x08) break;
            if (data->payload_len == 0 || data->data_ptr == nullptr) break;

            if (data->payload_offset == 0) {
                sock.rx_reset();
                if (data->payload_len > MAX_MSG_SIZE) {
                    ESP_LOGE(TAG, "Message too large: %d", data->payload_len);
                    break;
                }
                sock.rx_buf = static_cast<uint8_t*>(
                    heap_caps_calloc(data->payload_len, 1, MALLOC_CAP_SPIRAM));
                if (!sock.rx_buf) {
                    ESP_LOGE(TAG, "Alloc failed: %d bytes", data->payload_len);
                    break;
                }
                sock.rx_expected = data->payload_len;
                sock.rx_is_text = (data->op_code == 0x01);
            }

            if (sock.rx_buf && sock.rx_len + data->data_len <= sock.rx_expected) {
                memcpy(sock.rx_buf + sock.rx_len, data->data_ptr, data->data_len);
                sock.rx_len += data->data_len;
            }

            if (sock.rx_buf && sock.rx_len >= sock.rx_expected) {
                if (sock.rx_is_text) {
                    handle_control_frame(reinterpret_cast<const char*>(sock.rx_buf), sock.rx_len);
                    heap_caps_free(sock.rx_buf);
                }
                else {
                    QueuedMessage msg = { sock.rx_buf, sock.rx_len };
                    if (xQueueSend(sock.inbox, &msg, 0) == pdTRUE) {
                        process_queues();
                    }
                    else {
                        heap_caps_free(sock.rx_buf);
                    }
                }
                sock.rx_buf = nullptr;
                sock.rx_len = sock.rx_expected = 0;
            }
            break;

        default:
            break;
        }
    }

    esp_err_t start_client() {
        if (sock.client) {
            ESP_LOGW(TAG, "Client already initialized, destroying first");
            destroy_ws_client();
        }
        disconnect_handled = false;

        if (static_cert == nullptr) {
            static_ds_data_ctx = kd_common_crypto_get_ctx();
            if (static_ds_data_ctx == nullptr) {
                ESP_LOGE(TAG, "Failed to get DS context");
                return ESP_FAIL;
            }

            static_cert_len = 0;
            esp_err_t ret = kd_common_get_device_cert(nullptr, &static_cert_len);
            if (ret != ESP_OK || static_cert_len == 0) {
                ESP_LOGE(TAG, "Failed to get device certificate length: %s", esp_err_to_name(ret));
                return ret;
            }

            static_cert = static_cast<char*>(heap_caps_calloc(static_cert_len + 1, sizeof(char), MALLOC_CAP_SPIRAM));
            if (static_cert == nullptr) {
                ESP_LOGE(TAG, "Failed to allocate certificate buffer");
                return ESP_ERR_NO_MEM;
            }

            ret = kd_common_get_device_cert(static_cert, &static_cert_len);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "Failed to get device certificate: %s", esp_err_to_name(ret));
                heap_caps_free(static_cert);
                static_cert = nullptr;
                return ret;
            }
        }

        esp_websocket_client_config_t cfg = {};
        cfg.uri = SOCKETS_URL;
        cfg.port = 443;
        cfg.client_cert = static_cert;
        cfg.client_cert_len = static_cert_len + 1;
        cfg.client_ds_data = static_ds_data_ctx;
        cfg.crt_bundle_attach = esp_crt_bundle_attach;
        cfg.network_timeout_ms = 15000;
        cfg.buffer_size = 4096;
        cfg.ping_interval_sec = 25;
        cfg.pingpong_timeout_sec = 60;

        cfg.keep_alive_enable = true;
        cfg.keep_alive_idle = 10;
        cfg.keep_alive_interval = 5;
        cfg.keep_alive_count = 5;

        cfg.disable_auto_reconnect = true;
        cfg.enable_close_reconnect = false;

        esp_websocket_client_handle_t client = esp_websocket_client_init(&cfg);
        if (!client) return ESP_FAIL;

        if (!client_mutex || xSemaphoreTake(client_mutex, pdMS_TO_TICKS(6000)) != pdTRUE) {
            esp_websocket_client_destroy(client);
            return ESP_FAIL;
        }
        sock.client = client;
        xSemaphoreGive(client_mutex);

        esp_websocket_register_events(client, WEBSOCKET_EVENT_ANY, ws_event_handler, nullptr);

        return esp_websocket_client_start(client);
    }

    void try_advance_state() {
        switch (sock.state) {
        case State::WaitingForNetwork:
            break;

        case State::WaitingForCrypto: {
            CryptoState_t crypto_state = kd_common_crypto_get_state();
            if (crypto_state == CryptoState_t::CRYPTO_STATE_VALID_CERT) {
                sock.state = State::Ready;
                try_advance_state();
            }
            break;
        }

        case State::Ready:
            show_fs_sprite("connecting");
            schedule_reconnect();
            break;

        case State::Connected:
            break;
        }
    }

    void wifi_event_handler(void*, esp_event_base_t base, int32_t id, void*) {
        if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
            if (sock.state != State::WaitingForNetwork) {
                sock.state = State::WaitingForNetwork;
            }
        }
        else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
            if (sock.state == State::WaitingForNetwork) {
                sock.state = State::WaitingForCrypto;
                if (state_check_timer) {
                    esp_timer_start_periodic(state_check_timer, 1000 * 1000);
                }
                try_advance_state();
            }
        }
    }

    void state_check_callback(void*) {
        if (sock.state == State::WaitingForCrypto) {
            try_advance_state();
        }
        else {
            esp_timer_stop(state_check_timer);
        }
    }

}  // namespace

void sockets_init() {
    client_mutex = xSemaphoreCreateMutex();
    token_mutex = xSemaphoreCreateMutex();
    sock.outbox = xQueueCreate(QUEUE_SIZE, sizeof(QueuedMessage));
    sock.inbox = xQueueCreate(QUEUE_SIZE, sizeof(QueuedMessage));

    if (!sock.outbox || !sock.inbox) {
        ESP_LOGE(TAG, "Failed to create queues");
        return;
    }

    ctl_queue = xQueueCreate(1, sizeof(CtlCmd));
    if (!ctl_queue ||
        xTaskCreate(ctl_task, "sock_ctl", 4096, nullptr, 10, &ctl_task_handle) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create control task");
        return;
    }

    msg_init(sock.outbox);

    esp_timer_create_args_t welcome_timer_args = {
        .callback = welcome_timeout_callback,
        .arg = nullptr,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "sock_welcome",
        .skip_unhandled_events = true,
    };
    esp_timer_create(&welcome_timer_args, &welcome_timeout_timer);

    esp_timer_create_args_t token_timer_args = {
        .callback = token_refresh_timer_callback,
        .arg = nullptr,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "sock_token",
        .skip_unhandled_events = true,
    };
    esp_timer_create(&token_timer_args, &token_refresh_timer);

    esp_timer_create_args_t state_timer_args = {
        .callback = state_check_callback,
        .arg = nullptr,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "sock_state",
        .skip_unhandled_events = true,
    };
    esp_timer_create(&state_timer_args, &state_check_timer);

    esp_timer_create_args_t reconnect_timer_args = {
        .callback = reconnect_timer_callback,
        .arg = nullptr,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "sock_reconn",
        .skip_unhandled_events = true,
    };
    esp_timer_create(&reconnect_timer_args, &reconnect_timer);

    esp_timer_create_args_t schedule_retry_args = {
        .callback = schedule_retry_callback,
        .arg = nullptr,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "sock_sched",
        .skip_unhandled_events = true,
    };
    esp_timer_create(&schedule_retry_args, &schedule_retry_timer);

    esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, wifi_event_handler, nullptr);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, nullptr);

    if (kd_common_is_wifi_connected()) {
        sock.state = State::WaitingForCrypto;
        esp_timer_start_periodic(state_check_timer, 1000 * 1000);
        try_advance_state();
    }
}

void sockets_deinit() {
    esp_event_handler_unregister(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, wifi_event_handler);
    esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler);

    if (token_refresh_timer) {
        esp_timer_stop(token_refresh_timer);
        esp_timer_delete(token_refresh_timer);
        token_refresh_timer = nullptr;
    }
    if (welcome_timeout_timer) {
        esp_timer_stop(welcome_timeout_timer);
        esp_timer_delete(welcome_timeout_timer);
        welcome_timeout_timer = nullptr;
    }
    if (schedule_retry_timer) {
        esp_timer_stop(schedule_retry_timer);
        esp_timer_delete(schedule_retry_timer);
        schedule_retry_timer = nullptr;
    }
    if (reconnect_timer) {
        esp_timer_stop(reconnect_timer);
        esp_timer_delete(reconnect_timer);
        reconnect_timer = nullptr;
    }
    if (state_check_timer) {
        esp_timer_stop(state_check_timer);
        esp_timer_delete(state_check_timer);
        state_check_timer = nullptr;
    }

    if (ctl_task_handle) {
        vTaskDelete(ctl_task_handle);
        ctl_task_handle = nullptr;
    }
    if (ctl_queue) {
        vQueueDelete(ctl_queue);
        ctl_queue = nullptr;
    }

    if (sock.client) {
        esp_websocket_client_stop(sock.client);
        destroy_ws_client();
    }

    sock.rx_reset();

    if (static_cert) {
        heap_caps_free(static_cert);
        static_cert = nullptr;
        static_cert_len = 0;
    }
    if (static_ds_data_ctx) {
        free(static_ds_data_ctx->esp_ds_data);
        free(static_ds_data_ctx);
        static_ds_data_ctx = nullptr;
    }

    QueuedMessage msg;
    if (sock.outbox) {
        while (xQueueReceive(sock.outbox, &msg, 0) == pdTRUE) heap_caps_free(msg.data);
        vQueueDelete(sock.outbox);
    }
    if (sock.inbox) {
        while (xQueueReceive(sock.inbox, &msg, 0) == pdTRUE) heap_caps_free(msg.data);
        vQueueDelete(sock.inbox);
    }

    if (token_mutex) {
        xSemaphoreTake(token_mutex, pdMS_TO_TICKS(1000));
        heap_caps_free(device_token);
        device_token = nullptr;
        token_expires_at = 0;
        xSemaphoreGive(token_mutex);
        vSemaphoreDelete(token_mutex);
        token_mutex = nullptr;
    }

    if (client_mutex) {
        vSemaphoreDelete(client_mutex);
        client_mutex = nullptr;
    }
}

bool sockets_is_connected() {
    return is_connected();
}

void sockets_flush_outbox() {
    process_queues();
}

void sockets_on_schedule_received() {
    schedule_retry_count = 0;
    if (schedule_retry_timer) esp_timer_stop(schedule_retry_timer);
}

char* sockets_get_device_token_copy() {
    if (!token_mutex) return nullptr;
    if (xSemaphoreTake(token_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) return nullptr;

    char* copy = nullptr;
    if (device_token) {
        size_t len = strlen(device_token) + 1;
        copy = static_cast<char*>(heap_caps_malloc(len, MALLOC_CAP_SPIRAM));
        if (copy) memcpy(copy, device_token, len);
    }
    xSemaphoreGive(token_mutex);
    return copy;
}

void sockets_request_token_refresh() {
    int64_t now_us = esp_timer_get_time();
    if (last_refresh_req_us > 0 && (now_us - last_refresh_req_us) < TOKEN_REFRESH_MIN_INTERVAL_US) {
        return;
    }
    if (send_text("{\"type\":\"token.refresh\"}")) {
        last_refresh_req_us = now_us;
    }
}

#include "sockets.h"
#include "handlers.h"
#include "messages.h"

#include <cstring>

#include <sdkconfig.h>
#include <esp_log.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>

#include <cJSON.h>
#include <kd_common.h>
#include <kd_crypto.h>
#ifdef CONFIG_KD_COMMON_CONSOLE_ENABLE
#include <kd_console.h>
#endif
#include <koios/cloudlink.h>
#include <kd/v1/matrx.pb-c.h>

#include "apps.h"
#include "scheduler.h"

static const char* TAG = "sockets";

namespace {

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

    esp_timer_handle_t schedule_retry_timer = nullptr;
    int schedule_retry_count = 0;
    constexpr int64_t SCHEDULE_RETRY_BASE_US = 10 * 1000 * 1000;
    constexpr int64_t SCHEDULE_RETRY_STEP_US = 10 * 1000 * 1000;
    constexpr int64_t SCHEDULE_RETRY_MAX_US = 30 * 1000 * 1000;

    int64_t next_schedule_retry_delay() {
        int64_t delay = SCHEDULE_RETRY_BASE_US + (schedule_retry_count * SCHEDULE_RETRY_STEP_US);
        return (delay > SCHEDULE_RETRY_MAX_US) ? SCHEDULE_RETRY_MAX_US : delay;
    }

    void schedule_retry_callback(void*) {
        if (koios_cloudlink_is_ready() && !scheduler_has_schedule()) {
            schedule_retry_count++;
            int64_t next_delay = next_schedule_retry_delay();
            ESP_LOGW(TAG, "No schedule received, retrying (attempt %d, next in %llds)",
                schedule_retry_count, next_delay / 1000000);
            msg_send_schedule_request();
            esp_timer_start_once(schedule_retry_timer, next_delay);
        }
    }

    void on_state_change(koios_cloud_state_t state) {
        switch (state) {
        case KOIOS_CLOUD_STATE_CONNECTING:
            show_fs_sprite("connecting");
            break;
        case KOIOS_CLOUD_STATE_READY:
            show_fs_sprite("ready");
            break;
        case KOIOS_CLOUD_STATE_WAITING:
            break;
        }
    }

    void on_session_ready() {
        char device_id[48];
        if (koios_cloudlink_get_device_id(device_id, sizeof(device_id))) {
            kd_common_mdns_add_svc_record("_koiosdigital", "device_id", device_id);
        }

        scheduler_on_connect();
        msg_send_device_info();
        msg_send_claim_if_needed();
        msg_send_cert_report();

        schedule_retry_count = 0;
        if (schedule_retry_timer) {
            esp_timer_stop(schedule_retry_timer);
            esp_timer_start_once(schedule_retry_timer, next_schedule_retry_delay());
        }
    }

    void on_disconnect() {
        if (schedule_retry_timer) esp_timer_stop(schedule_retry_timer);
        scheduler_on_disconnect();
    }

    void on_timezone(const char* tz) {
        // Quiet windows are evaluated in device-local wall-clock, so the device
        // must adopt the cloud-provided IANA timezone. This overrides the
        // IP-geolocation fallback (and disables auto-fetch) with an authoritative value.
        if (!tz || !tz[0]) return;
        ESP_LOGI(TAG, "Applying cloud timezone: %s", tz);
        kd_common_set_timezone(tz);
    }

    void on_message(const uint8_t* data, size_t len) {
        auto* msg = kd__v1__matrx_message__unpack(&spiram_allocator, len, data);
        if (msg) {
            handle_message(msg);
            kd__v1__matrx_message__free_unpacked(msg, &spiram_allocator);
        }
        else {
            ESP_LOGW(TAG, "Failed to unpack message (%zu bytes)", len);
        }
    }

    char* on_cert_renew_csr() {
        esp_err_t err = kd_common_crypto_regenerate_csr();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "CSR regeneration failed: %s", esp_err_to_name(err));
            return nullptr;
        }

        size_t csr_len = 0;
        if (kd_common_get_csr(nullptr, &csr_len) != ESP_OK || csr_len == 0) {
            ESP_LOGE(TAG, "No CSR available after regeneration");
            return nullptr;
        }

        auto* csr = static_cast<char*>(malloc(csr_len + 1));
        if (!csr) return nullptr;

        if (kd_common_get_csr(csr, &csr_len) != ESP_OK) {
            free(csr);
            return nullptr;
        }
        csr[csr_len] = '\0';
        return csr;
    }

    void on_cert_renewed(const char* cert_pem, const char* chain_pem) {
        if (!cert_pem) return;

        // Store the full chain (leaf + issuing intermediates), not just the
        // leaf: edge mTLS validates the device against the platform root, so
        // the device must present the intermediates in its TLS handshake to
        // build a path to that root. chain_pem is leaf-first and complete;
        // fall back to the bare leaf only if the server omitted it.
        const char* to_store = (chain_pem && chain_pem[0]) ? chain_pem : cert_pem;
        size_t len = strlen(to_store);

        esp_err_t err = kd_common_set_device_cert(to_store, len);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to store renewed cert: %s", esp_err_to_name(err));
            return;
        }
        ESP_LOGI(TAG, "Renewed certificate stored (%zu bytes, %s)", len,
            to_store == chain_pem ? "fullchain" : "leaf only");
    }

    int cmd_test_cert_renew(int argc, char** argv) {
        if (!koios_cloudlink_is_ready()) {
            printf("[ERROR] Not connected to cloud\n");
            return 1;
        }

        esp_err_t err = kd_common_crypto_regenerate_csr();
        if (err != ESP_OK) {
            printf("[ERROR] CSR regeneration failed: %s\n", esp_err_to_name(err));
            return 1;
        }

        size_t csr_len = 0;
        if (kd_common_get_csr(nullptr, &csr_len) != ESP_OK || csr_len == 0) {
            printf("[ERROR] No CSR available\n");
            return 1;
        }

        auto* csr = static_cast<char*>(malloc(csr_len + 1));
        if (!csr) {
            printf("[ERROR] Allocation failed\n");
            return 1;
        }

        if (kd_common_get_csr(csr, &csr_len) != ESP_OK) {
            free(csr);
            printf("[ERROR] Failed to read CSR\n");
            return 1;
        }
        csr[csr_len] = '\0';

        cJSON* req = cJSON_CreateObject();
        if (!req) { free(csr); return 1; }
        cJSON_AddStringToObject(req, "type", "cert.renew_request");
        cJSON_AddStringToObject(req, "csr", csr);
        free(csr);

        char* json = cJSON_PrintUnformatted(req);
        cJSON_Delete(req);
        if (!json) {
            printf("[ERROR] JSON serialization failed\n");
            return 1;
        }

        bool sent = koios_cloudlink_send_text(json);
        cJSON_free(json);
        if (sent) {
            printf("[OK] Cert renewal request sent; watch for cert.renew_response\n");
        } else {
            printf("[ERROR] Failed to queue renewal request\n");
        }
        return sent ? 0 : 1;
    }

}

void sockets_init() {
    esp_timer_create_args_t schedule_retry_args = {
        .callback = schedule_retry_callback,
        .arg = nullptr,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "sock_sched",
        .skip_unhandled_events = true,
    };
    esp_timer_create(&schedule_retry_args, &schedule_retry_timer);

    koios_cloudlink_config_t cfg = {};
    cfg.url = SOCKETS_URL;
    cfg.auth_mode = KOIOS_CLOUD_AUTH_MTLS;
    cfg.on_state_change = on_state_change;
    cfg.on_session_ready = on_session_ready;
    cfg.on_disconnect = on_disconnect;
    cfg.on_message = on_message;
    cfg.on_timezone = on_timezone;
    cfg.on_cert_renew_csr = on_cert_renew_csr;
    cfg.on_cert_renewed = on_cert_renewed;
    cfg.variant = FIRMWARE_VARIANT;
    koios_cloudlink_init(&cfg);

#ifdef CONFIG_KD_COMMON_CONSOLE_ENABLE
    kd_console_register_cmd("test_cert_renew",
        "Send a cert renewal request to the server (requires active session)",
        cmd_test_cert_renew);
#endif
}

void sockets_deinit() {
    koios_cloudlink_deinit();
    if (schedule_retry_timer) {
        esp_timer_stop(schedule_retry_timer);
        esp_timer_delete(schedule_retry_timer);
        schedule_retry_timer = nullptr;
    }
}

bool sockets_is_connected() {
    return koios_cloudlink_is_connected();
}

void sockets_on_schedule_received() {
    schedule_retry_count = 0;
    if (schedule_retry_timer) esp_timer_stop(schedule_retry_timer);
}

char* sockets_get_device_token_copy() {
    return koios_cloudlink_get_token_copy();
}

void sockets_request_token_refresh() {
    koios_cloudlink_request_token_refresh();
}

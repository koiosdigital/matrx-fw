// Thin app layer over koios-sdk's cloudlink: matrx-specific protobuf
// dispatch, connection sprites, and the schedule-request retry loop.
// Connection lifecycle (backoff, welcome, token, failure cascade) lives
// in the SDK.

#include "sockets.h"
#include "handlers.h"
#include "messages.h"

#include <cstring>

#include <sdkconfig.h>
#include <esp_log.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>

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

    // Retry schedule requests until the server delivers one.
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
        scheduler_on_connect();
        msg_send_device_info();
        msg_send_claim_if_needed();

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

}  // namespace

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
    // Hardware SKU for cloud-side OTA classification (fw.class twin report).
    // Dev builds stay unclassified so a bench board never claims a SKU.
    if (strcmp(FIRMWARE_VARIANT, "devel") != 0) {
        static constexpr char kDeviceClass[] = CONFIG_IDF_TARGET "-" FIRMWARE_VARIANT;
        cfg.device_class = kDeviceClass;
    }
    koios_cloudlink_init(&cfg);
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

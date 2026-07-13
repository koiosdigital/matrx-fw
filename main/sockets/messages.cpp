#include "messages.h"

#include <esp_log.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <kd_common.h>
#include <koios/cloudlink.h>
#include <kd/v1/common.pb-c.h>

#include "apps.h"
#include "config.h"

static const char* TAG = "messages";

namespace {

    int64_t g_last_claim_ms = 0;

    constexpr int64_t CLAIM_RETRY_MS = 5000;

}  // namespace

// Wrap a oneof payload in a MatrxMessage and enqueue it.
#define MSG_QUEUE_ONE(case_, field_, ptr_)                       \
    do {                                                         \
        Kd__V1__MatrxMessage _m = KD__V1__MATRX_MESSAGE__INIT;   \
        _m.message_case = (case_);                               \
        _m.field_ = (ptr_);                                      \
        msg_queue(&_m);                                          \
    } while (0)

bool msg_queue(const Kd__V1__MatrxMessage* message) {
    if (message == nullptr) return false;

    size_t len = kd__v1__matrx_message__get_packed_size(message);
    auto* buf = static_cast<uint8_t*>(heap_caps_malloc(len, MALLOC_CAP_SPIRAM));
    if (buf == nullptr) {
        ESP_LOGE(TAG, "Failed to alloc %zu bytes", len);
        return false;
    }

    kd__v1__matrx_message__pack(message, buf);

    bool ok = koios_cloudlink_send(buf, len);
    if (!ok) {
        ESP_LOGW(TAG, "Failed to queue message (%zu bytes)", len);
    }
    heap_caps_free(buf);
    return ok;
}

void msg_send_device_info() {
    Kd__V1__DeviceInfo info = KD__V1__DEVICE_INFO__INIT;
    info.width = CONFIG_MATRIX_WIDTH;
    info.height = CONFIG_MATRIX_HEIGHT;
    info.has_light_sensor = true;

    MSG_QUEUE_ONE(KD__V1__MATRX_MESSAGE__MESSAGE_DEVICE_INFO, device_info, &info);
}

void msg_send_device_config() {
    system_config_t cfg = config_get();

    Kd__V1__DeviceConfig device_cfg = KD__V1__DEVICE_CONFIG__INIT;
    device_cfg.screen_enabled = cfg.screen_enabled;
    device_cfg.screen_brightness = cfg.screen_brightness;
    device_cfg.auto_brightness_enabled = cfg.auto_brightness_enabled;
    device_cfg.screen_off_lux = cfg.screen_off_lux;
    device_cfg.is_quiet_now = config_is_quiet_now();
    uint8_t count = cfg.quiet_window_count;
    if (count > MATRX_MAX_QUIET_WINDOWS) count = MATRX_MAX_QUIET_WINDOWS;

    const Kd__V1__MatrxQuietWindow window_init = KD__V1__MATRX_QUIET_WINDOW__INIT;
    Kd__V1__MatrxQuietWindow windows[MATRX_MAX_QUIET_WINDOWS];
    Kd__V1__MatrxQuietWindow* window_ptrs[MATRX_MAX_QUIET_WINDOWS];
    for (uint8_t i = 0; i < count; i++) {
        windows[i] = window_init;
        windows[i].day_mask = cfg.quiet_windows[i].day_mask;
        windows[i].start_hour = cfg.quiet_windows[i].start_hour;
        windows[i].start_min = cfg.quiet_windows[i].start_min;
        windows[i].end_hour = cfg.quiet_windows[i].end_hour;
        windows[i].end_min = cfg.quiet_windows[i].end_min;
        windows[i].enabled = cfg.quiet_windows[i].enabled;
        window_ptrs[i] = &windows[i];
    }
    device_cfg.n_quiet_windows = count;
    device_cfg.quiet_windows = (count > 0) ? window_ptrs : nullptr;

    MSG_QUEUE_ONE(KD__V1__MATRX_MESSAGE__MESSAGE_DEVICE_CONFIG, device_config, &device_cfg);
}

void msg_send_claim_if_needed() {
    int64_t now = esp_timer_get_time() / 1000;
    if (g_last_claim_ms > 0 && (now - g_last_claim_ms) < CLAIM_RETRY_MS) {
        return;
    }

    size_t token_len = 0;
    if (kd_common_get_claim_token(nullptr, &token_len) != ESP_OK || token_len == 0) {
        return;
    }

    auto* token = static_cast<uint8_t*>(heap_caps_malloc(token_len, MALLOC_CAP_SPIRAM));
    if (token == nullptr) return;

    if (kd_common_get_claim_token(reinterpret_cast<char*>(token), &token_len) != ESP_OK || token_len == 0) {
        ESP_LOGE(TAG, "Claim required but failed to retrieve token");
        heap_caps_free(token);
        return;
    }

    Kd__V1__ClaimDevice claim = KD__V1__CLAIM_DEVICE__INIT;
    claim.claim_token.data = token;
    claim.claim_token.len = token_len;

    MSG_QUEUE_ONE(KD__V1__MATRX_MESSAGE__MESSAGE_CLAIM_DEVICE, claim_device, &claim);
    g_last_claim_ms = now;
    heap_caps_free(token);
}

void msg_upload_coredump() {
}

void msg_send_currently_displaying(const App_t* app) {
    if (app == nullptr) return;

    Kd__V1__CurrentlyDisplayingApp disp = KD__V1__CURRENTLY_DISPLAYING_APP__INIT;
    disp.uuid.data = const_cast<uint8_t*>(app->uuid);
    disp.uuid.len = 16;

    MSG_QUEUE_ONE(KD__V1__MATRX_MESSAGE__MESSAGE_CURRENTLY_DISPLAYING_APP, currently_displaying_app, &disp);
}

void msg_send_schedule_request() {
    Kd__V1__ScheduleRequest req = KD__V1__SCHEDULE_REQUEST__INIT;

    MSG_QUEUE_ONE(KD__V1__MATRX_MESSAGE__MESSAGE_SCHEDULE_REQUEST, schedule_request, &req);
}

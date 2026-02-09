#include "messages.h"

#include <esp_log.h>
#include <esp_heap_caps.h>
#include <esp_partition.h>
#include <esp_app_desc.h>
#include <esp_timer.h>
#include <kd_common.h>
#include <kd/v1/common.pb-c.h>

#include "apps.h"
#include "config.h"

static const char* TAG = "messages";

namespace {

    QueueHandle_t g_outbox = nullptr;
    bool g_needs_claim = false;
    int64_t g_last_claim_ms = 0;

    constexpr int64_t CLAIM_RETRY_MS = 5000;

}  // namespace

void msg_init(QueueHandle_t outbox) {
    g_outbox = outbox;
}

bool msg_queue(const Kd__V1__MatrxMessage* message) {
    if (message == nullptr || g_outbox == nullptr) return false;

    size_t len = kd__v1__matrx_message__get_packed_size(message);
    auto* buf = static_cast<uint8_t*>(heap_caps_malloc(len, MALLOC_CAP_SPIRAM));
    if (buf == nullptr) {
        ESP_LOGE(TAG, "Failed to alloc %zu bytes", len);
        return false;
    }

    kd__v1__matrx_message__pack(message, buf);

    struct { uint8_t* data; size_t len; } msg = { buf, len };
    if (xQueueSend(g_outbox, &msg, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGW(TAG, "Outbox full");
        free(buf);
        return false;
    }
    return true;
}

void msg_send_device_info() {
    Kd__V1__DeviceInfo info = KD__V1__DEVICE_INFO__INIT;
    info.width = CONFIG_MATRIX_WIDTH;
    info.height = CONFIG_MATRIX_HEIGHT;
    info.has_light_sensor = true;

    Kd__V1__MatrxMessage msg = KD__V1__MATRX_MESSAGE__INIT;
    msg.message_case = KD__V1__MATRX_MESSAGE__MESSAGE_DEVICE_INFO;
    msg.device_info = &info;
    msg_queue(&msg);
}

void msg_send_device_config() {
    system_config_t cfg = config_get();

    Kd__V1__DeviceConfig device_cfg = KD__V1__DEVICE_CONFIG__INIT;
    device_cfg.screen_enabled = cfg.screen_enabled;
    device_cfg.screen_brightness = cfg.screen_brightness;
    device_cfg.auto_brightness_enabled = cfg.auto_brightness_enabled;
    device_cfg.screen_off_lux = cfg.screen_off_lux;

    Kd__V1__MatrxMessage msg = KD__V1__MATRX_MESSAGE__INIT;
    msg.message_case = KD__V1__MATRX_MESSAGE__MESSAGE_DEVICE_CONFIG;
    msg.device_config = &device_cfg;
    msg_queue(&msg);
}

void msg_send_claim_if_needed() {
    g_needs_claim = true;

    int64_t now = esp_timer_get_time() / 1000;
    if (g_last_claim_ms > 0 && (now - g_last_claim_ms) < CLAIM_RETRY_MS) {
        return;
    }

    // Query actual token length first
    size_t token_len = 0;
    if (kd_common_get_claim_token(nullptr, &token_len) != ESP_OK || token_len == 0) {
        return;
    }

    // Allocate only what's needed
    auto* token = static_cast<uint8_t*>(heap_caps_malloc(token_len, MALLOC_CAP_SPIRAM));
    if (token == nullptr) return;

    if (kd_common_get_claim_token(reinterpret_cast<char*>(token), &token_len) != ESP_OK || token_len == 0) {
        free(token);
        return;
    }

    Kd__V1__ClaimDevice claim = KD__V1__CLAIM_DEVICE__INIT;
    claim.claim_token.data = token;
    claim.claim_token.len = token_len;

    Kd__V1__MatrxMessage msg = KD__V1__MATRX_MESSAGE__INIT;
    msg.message_case = KD__V1__MATRX_MESSAGE__MESSAGE_CLAIM_DEVICE;
    msg.claim_device = &claim;

    msg_queue(&msg);
    g_last_claim_ms = now;
    free(token);

    ESP_LOGI(TAG, "Sent claim request");
}

void msg_upload_coredump() {
    // Coredump upload temporarily disabled for RAM optimization
    // TODO: Implement chunked streaming when re-enabling
    /*
    const esp_partition_t* part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_COREDUMP, "coredump");
    if (part == nullptr) return;

    size_t size = part->size;
    auto* data = static_cast<uint8_t*>(heap_caps_malloc(size, MALLOC_CAP_SPIRAM));
    if (data == nullptr) return;

    if (esp_partition_read(part, 0, data, size) != ESP_OK) {
        free(data);
        return;
    }

    // Check if erased (all 0xFF)
    bool erased = true;
    for (size_t i = 0; i < 256 && i < size; i++) {
        if (data[i] != 0xFF) { erased = false; break; }
    }

    if (!erased) {
        ESP_LOGI(TAG, "Uploading coredump (%zu bytes)", size);

        const esp_app_desc_t* app = esp_app_get_description();

        Kd__V1__UploadCoreDump upload = KD__V1__UPLOAD_CORE_DUMP__INIT;
        upload.core_dump.data = data;
        upload.core_dump.len = size;
        upload.firmware_project = const_cast<char*>(app->project_name);
        upload.firmware_version = const_cast<char*>(app->version);
#ifdef FIRMWARE_VARIANT
        upload.firmware_variant = const_cast<char*>(FIRMWARE_VARIANT);
#endif

        Kd__V1__MatrxMessage msg = KD__V1__MATRX_MESSAGE__INIT;
        msg.message_case = KD__V1__MATRX_MESSAGE__MESSAGE_UPLOAD_CORE_DUMP;
        msg.upload_core_dump = &upload;

        if (msg_queue(&msg)) {
            esp_partition_erase_range(part, 0, size);
        }
    }

    free(data);
    */
}

void msg_request_app_render(const App_t* app) {
    if (app == nullptr) return;

    Kd__V1__AppRenderRequest req = KD__V1__APP_RENDER_REQUEST__INIT;
    req.app_uuid.data = const_cast<uint8_t*>(app->uuid);
    req.app_uuid.len = 16;

    req.data_sha256.data = const_cast<uint8_t*>(app->sha256);
    req.data_sha256.len = 32;

    req.preferred_chunk_size = APP_TRANSFER_CHUNK_SIZE;

    Kd__V1__MatrxMessage msg = KD__V1__MATRX_MESSAGE__INIT;
    msg.message_case = KD__V1__MATRX_MESSAGE__MESSAGE_APP_RENDER_REQUEST;
    msg.app_render_request = &req;
    msg_queue(&msg);
}

void msg_send_currently_displaying(const App_t* app) {
    if (app == nullptr) return;

    Kd__V1__CurrentlyDisplayingApp disp = KD__V1__CURRENTLY_DISPLAYING_APP__INIT;
    disp.uuid.data = const_cast<uint8_t*>(app->uuid);
    disp.uuid.len = 16;

    Kd__V1__MatrxMessage msg = KD__V1__MATRX_MESSAGE__INIT;
    msg.message_case = KD__V1__MATRX_MESSAGE__MESSAGE_CURRENTLY_DISPLAYING_APP;
    msg.currently_displaying_app = &disp;
    msg_queue(&msg);
}

void msg_send_schedule_request() {
    Kd__V1__ScheduleRequest req = KD__V1__SCHEDULE_REQUEST__INIT;

    Kd__V1__MatrxMessage msg = KD__V1__MATRX_MESSAGE__INIT;
    msg.message_case = KD__V1__MATRX_MESSAGE__MESSAGE_SCHEDULE_REQUEST;
    msg.schedule_request = &req;
    msg_queue(&msg);
}

void msg_send_cert_renew_request(const char* csr, size_t csr_len) {
    if (csr == nullptr || csr_len == 0) {
        ESP_LOGE(TAG, "Invalid CSR");
        return;
    }

    Kd__V1__CertRenewRequest req = KD__V1__CERT_RENEW_REQUEST__INIT;
    req.csr.data = reinterpret_cast<uint8_t*>(const_cast<char*>(csr));
    req.csr.len = csr_len;

    Kd__V1__MatrxMessage msg = KD__V1__MATRX_MESSAGE__INIT;
    msg.message_case = KD__V1__MATRX_MESSAGE__MESSAGE_CERT_RENEW_REQUEST;
    msg.cert_renew_request = &req;

    msg_queue(&msg);
    ESP_LOGI(TAG, "Sent cert renew request");
}
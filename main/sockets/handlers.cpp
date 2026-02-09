#include "handlers.h"
#include "messages.h"

#include <esp_log.h>
#include <kd_common.h>

#include <esp_heap_caps.h>

#include "display.h"
#include "apps.h"
#include "config.h"
#include "scheduler.h"

static const char* TAG = "handlers";

namespace {

    void handle_schedule(Kd__V1__Schedule* schedule) {
        if (schedule == nullptr) return;

        ESP_LOGI(TAG, "Schedule received (%zu items)", schedule->n_schedule_items);
        apps_sync_schedule(schedule->schedule_items, schedule->n_schedule_items);
        scheduler_on_schedule_received();
    }

    void handle_app_render_response(Kd__V1__AppRenderResponse* response) {
        if (response == nullptr) return;
        if (response->app_uuid.len != 16) {
            ESP_LOGW(TAG, "Invalid app UUID length: %zu", response->app_uuid.len);
            return;
        }

        App_t* app = app_find(response->app_uuid.data);
        if (!app) {
            ESP_LOGW(TAG, "App not found for render response");
            return;
        }

        // Chunked transfer: allocate buffer and wait for chunks
        if (response->total_size == 0 || response->total_chunks == 0) {
            ESP_LOGW(TAG, "Invalid render response: size=%u, chunks=%u",
                     response->total_size, response->total_chunks);
            scheduler_on_render_response(response->app_uuid.data, false);
            return;
        }

        ESP_LOGI(TAG, "App render response: %u bytes in %u chunks",
                 response->total_size, response->total_chunks);

        if (!app_transfer_start(app, response->total_size, response->total_chunks,
                                response->data_sha256.data)) {
            ESP_LOGE(TAG, "Failed to start transfer");
            scheduler_on_render_response(response->app_uuid.data, false);
        }
    }

    void handle_app_data_chunk(Kd__V1__AppDataChunk* chunk) {
        if (chunk == nullptr) return;
        if (chunk->app_uuid.len != 16) {
            ESP_LOGW(TAG, "Invalid chunk UUID length: %zu", chunk->app_uuid.len);
            return;
        }

        App_t* app = app_find(chunk->app_uuid.data);
        if (!app) {
            ESP_LOGW(TAG, "App not found for chunk");
            return;
        }

        if (!app_transfer_add_chunk(app, chunk->chunk_index, chunk->data.data, chunk->data.len)) {
            ESP_LOGE(TAG, "Failed to add chunk %u", chunk->chunk_index);
            app_transfer_cancel(app);
            scheduler_on_render_response(app->uuid, false);
            return;
        }

        // Check if transfer is complete
        if (app_transfer_is_complete(app)) {
            bool success = app_transfer_finalize(app);
            scheduler_on_render_response(app->uuid, success);
            if (success) {
                ESP_LOGI(TAG, "App data transfer complete");
            }
        }
    }

    void handle_device_config(const Kd__V1__DeviceConfig* cfg) {
        if (cfg == nullptr) return;

        system_config_t new_cfg = config_get();
        new_cfg.screen_enabled = cfg->screen_enabled;
        new_cfg.screen_brightness = (cfg->screen_brightness <= 255)
            ? static_cast<uint8_t>(cfg->screen_brightness) : new_cfg.screen_brightness;
        new_cfg.auto_brightness_enabled = cfg->auto_brightness_enabled;
        new_cfg.screen_off_lux = (cfg->screen_off_lux <= 65535)
            ? static_cast<uint16_t>(cfg->screen_off_lux) : new_cfg.screen_off_lux;

        config_set(&new_cfg);
        ESP_LOGI(TAG, "Applied device config");
    }

    void handle_join_response(Kd__V1__JoinResponse* response) {
        if (response == nullptr) return;

        ESP_LOGI(TAG, "Join: claimed=%d, needs_claimed=%d",
            response->is_claimed, response->needs_claimed);

        msg_send_device_info();

        bool needs_claim = response->needs_claimed || !response->is_claimed;
        if (!needs_claim) {
            kd_common_clear_claim_token();
        }
        else {
            msg_send_claim_if_needed();
        }
    }

    void handle_factory_reset(Kd__V1__FactoryResetRequest* request) {
        const char* reason = (request && request->reason) ? request->reason : nullptr;
        ESP_LOGI(TAG, "Factory reset requested");
        perform_factory_reset(reason);
    }

    void handle_cert_renew_required(Kd__V1__CertRenewRequired* request) {
        if (request != nullptr && request->reason != nullptr) {
            ESP_LOGI(TAG, "Cert renewal required: %s", request->reason);
        } else {
            ESP_LOGI(TAG, "Cert renewal required");
        }

        // Get CSR - it should already exist from provisioning
        size_t csr_len = 0;
        if (kd_common_get_csr(nullptr, &csr_len) != ESP_OK || csr_len == 0) {
            ESP_LOGE(TAG, "No CSR available for renewal");
            return;
        }

        auto* csr_buf = static_cast<char*>(heap_caps_malloc(csr_len + 1, MALLOC_CAP_SPIRAM));
        if (csr_buf == nullptr) {
            ESP_LOGE(TAG, "Failed to allocate CSR buffer");
            return;
        }

        if (kd_common_get_csr(csr_buf, &csr_len) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to get CSR");
            heap_caps_free(csr_buf);
            return;
        }
        csr_buf[csr_len] = '\0';

        msg_send_cert_renew_request(csr_buf, csr_len);
        heap_caps_free(csr_buf);
    }

    void handle_cert_renew_response(Kd__V1__CertRenewResponse* response) {
        if (response == nullptr) return;

        if (!response->success) {
            ESP_LOGE(TAG, "Cert renewal failed: %s",
                response->error ? response->error : "unknown error");
            return;
        }

        if (response->device_cert.data == nullptr || response->device_cert.len == 0) {
            ESP_LOGE(TAG, "Cert renewal response missing certificate");
            return;
        }

        ESP_LOGI(TAG, "Received new certificate (%zu bytes)", response->device_cert.len);

        esp_err_t err = kd_common_set_device_cert(
            reinterpret_cast<const char*>(response->device_cert.data),
            response->device_cert.len);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to store new certificate: %s", esp_err_to_name(err));
            return;
        }

        ESP_LOGI(TAG, "Certificate renewed successfully");
        // Server will disconnect us to reconnect with new cert
    }

}  // namespace

void handle_message(Kd__V1__MatrxMessage* message) {
    if (message == nullptr) return;

    switch (message->message_case) {
    case KD__V1__MATRX_MESSAGE__MESSAGE_SCHEDULE:
        handle_schedule(message->schedule);
        break;
    case KD__V1__MATRX_MESSAGE__MESSAGE_APP_RENDER_RESPONSE:
        handle_app_render_response(message->app_render_response);
        break;
    case KD__V1__MATRX_MESSAGE__MESSAGE_APP_DATA_CHUNK:
        handle_app_data_chunk(message->app_data_chunk);
        break;
    case KD__V1__MATRX_MESSAGE__MESSAGE_DEVICE_CONFIG_REQUEST:
        msg_send_device_config();
        break;
    case KD__V1__MATRX_MESSAGE__MESSAGE_DEVICE_CONFIG:
        handle_device_config(message->device_config);
        break;
    case KD__V1__MATRX_MESSAGE__MESSAGE_JOIN_RESPONSE:
        handle_join_response(message->join_response);
        break;
    case KD__V1__MATRX_MESSAGE__MESSAGE_FACTORY_RESET_REQUEST:
        handle_factory_reset(message->factory_reset_request);
        break;
    case KD__V1__MATRX_MESSAGE__MESSAGE_CERT_RENEW_REQUIRED:
        handle_cert_renew_required(message->cert_renew_required);
        break;
    case KD__V1__MATRX_MESSAGE__MESSAGE_CERT_RENEW_RESPONSE:
        handle_cert_renew_response(message->cert_renew_response);
        break;
    default:
        ESP_LOGD(TAG, "Unhandled message: %d", message->message_case);
        break;
    }
}

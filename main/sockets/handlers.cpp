#include "handlers.h"
#include "messages.h"

#include <esp_log.h>
#include <kd_common.h>

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

        ESP_LOGI(TAG, "RX render response: uuid=...%02x%02x, displayable=%d, size=%u, chunks=%u",
                 response->app_uuid.data[14], response->app_uuid.data[15],
                 response->displayable, response->total_size, response->total_chunks);

        App_t* app = app_find(response->app_uuid.data);
        if (!app) {
            ESP_LOGW(TAG, "App not found for render response");
            return;
        }

        // Update displayable flag from server
        app_set_displayable(app, response->displayable);

        // Case 1: Not displayable (no chunks will follow)
        if (!response->displayable) {
            ESP_LOGI(TAG, "App not displayable, clearing data");
            app_clear_data(app);
            memset(app->sha256, 0, sizeof(app->sha256));
            scheduler_on_render_response(response->app_uuid.data, true, false);
            return;
        }

        // Case 2: Displayable but empty (no chunks will follow)
        if (response->total_size == 0) {
            ESP_LOGI(TAG, "App render response: empty (clearing data)");
            app_clear_data(app);
            memset(app->sha256, 0, sizeof(app->sha256));
            scheduler_on_render_response(response->app_uuid.data, true, true);
            return;
        }

        // Case 3: Displayable with data (chunks will follow)
        if (response->total_chunks == 0) {
            ESP_LOGW(TAG, "Invalid render response: size=%u but chunks=0",
                     response->total_size);
            scheduler_on_render_response(response->app_uuid.data, false, true);
            return;
        }

        ESP_LOGI(TAG, "App render response: %u bytes in %u chunks",
                 response->total_size, response->total_chunks);

        if (!app_transfer_start(app, response->total_size, response->total_chunks,
                                response->data_sha256.data)) {
            ESP_LOGE(TAG, "Failed to start transfer");
            scheduler_on_render_response(response->app_uuid.data, false, true);
        }
        // Chunks will arrive via handle_app_data_chunk
    }

    void handle_app_data_chunk(Kd__V1__AppDataChunk* chunk) {
        if (chunk == nullptr) return;
        if (chunk->app_uuid.len != 16) {
            ESP_LOGW(TAG, "Invalid chunk UUID length: %zu", chunk->app_uuid.len);
            return;
        }

        ESP_LOGI(TAG, "RX chunk: idx=%u, len=%zu, uuid=...%02x%02x",
                 chunk->chunk_index, chunk->data.len,
                 chunk->app_uuid.data[14], chunk->app_uuid.data[15]);

        // Zero-length chunk = empty app data (backwards compat with non-chunked server)
        if (chunk->data.len == 0) {
            ESP_LOGI(TAG, "Zero-length chunk, treating as empty app");
            App_t* app = app_find(chunk->app_uuid.data);
            if (app) {
                app_clear_data(app);
                memset(app->sha256, 0, sizeof(app->sha256));
                scheduler_on_render_response(chunk->app_uuid.data, true, app->displayable);
            }
            return;
        }

        App_t* app = app_find(chunk->app_uuid.data);
        if (!app) {
            ESP_LOGW(TAG, "App not found for chunk");
            return;
        }

        if (!app_transfer_add_chunk(app, chunk->chunk_index, chunk->data.data, chunk->data.len)) {
            ESP_LOGE(TAG, "Failed to add chunk %u (transfer active=%d, buffer=%p)",
                     chunk->chunk_index, app->transfer.active, app->transfer.buffer);
            app_transfer_cancel(app);
            scheduler_on_render_response(app->uuid, false, app->displayable);
            return;
        }

        // Check if transfer is complete
        if (app_transfer_is_complete(app)) {
            bool success = app_transfer_finalize(app);
            scheduler_on_render_response(app->uuid, success, app->displayable);
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

    void handle_pin_state_change(Kd__V1__ScheduleItemSetPinState* msg) {
        if (msg == nullptr) return;
        if (msg->uuid.len != 16) {
            ESP_LOGW(TAG, "Invalid pin state UUID length: %zu", msg->uuid.len);
            return;
        }

        ESP_LOGI(TAG, "Pin state change: uuid=...%02x%02x, pinned=%d",
                 msg->uuid.data[14], msg->uuid.data[15], msg->pinned);

        App_t* app = app_find(msg->uuid.data);
        if (!app) {
            ESP_LOGW(TAG, "App not found for pin state change");
            return;
        }

        app->pinned = msg->pinned;
        scheduler_on_pin_state_changed(msg->uuid.data, msg->pinned);
    }

}  // namespace

void handle_message(Kd__V1__MatrxMessage* message) {
    if (message == nullptr) return;

    switch (message->message_case) {
    case KD__V1__MATRX_MESSAGE__MESSAGE_SCHEDULE:
        handle_schedule(message->schedule);
        break;
    case KD__V1__MATRX_MESSAGE__MESSAGE_SCHEDULE_ITEM_SET_PIN_STATE:
        handle_pin_state_change(message->schedule_item_set_pin_state);
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
    default:
        ESP_LOGD(TAG, "Unhandled message: %d", message->message_case);
        break;
    }
}

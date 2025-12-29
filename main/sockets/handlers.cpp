#include "handlers.h"
#include "messages.h"

#include <esp_log.h>
#include <kd_common.h>

#include "display.h"
#include "apps.h"
#include "config.h"
#include "scheduler.h"
#include "../cert_renewal/cert_renewal.h"

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

        bool success = response->app_data.len > 0;
        ESP_LOGI(TAG, "App render response (%zu bytes, success=%d)",
            response->app_data.len, success);

        memcpy(app->sha256, response->data_sha256.data, sizeof(app->sha256));

        app_set_data(app, response->app_data.data, response->app_data.len);
        scheduler_on_render_response(response->app_uuid.data, success);
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

    void handle_cert_response(Kd__V1__CertResponse* response) {
        if (cert_renewal_handle_response(response)) {
            ESP_LOGI(TAG, "Cert renewed, reconnect required");
            // TODO: trigger reconnect
        }
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
    case KD__V1__MATRX_MESSAGE__MESSAGE_CERT_RESPONSE:
        handle_cert_response(message->cert_response);
        break;
    default:
        ESP_LOGD(TAG, "Unhandled message: %d", message->message_case);
        break;
    }
}

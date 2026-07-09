#include "handlers.h"
#include "messages.h"

#include <esp_log.h>
#include <kd_common.h>

#include "apps.h"
#include "config.h"
#include "scheduler.h"
#include "sockets.h"

static const char* TAG = "handlers";

namespace {

    bool validate_uuid(const ProtobufCBinaryData& uuid, const char* context) {
        if (uuid.len != 16) {
            ESP_LOGW(TAG, "Invalid %s UUID length: %zu", context, uuid.len);
            return false;
        }
        return true;
    }

    void handle_schedule(Kd__V1__Schedule* schedule) {
        if (schedule == nullptr) return;

        sockets_on_schedule_received();
        apps_sync_schedule(schedule->schedule_items, schedule->n_schedule_items);
        scheduler_on_schedule_received();
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

        // Quiet-hours windows (is_quiet_now is device-computed and ignored here).
        new_cfg.quiet_window_count = 0;
        for (size_t i = 0; i < cfg->n_quiet_windows
                 && new_cfg.quiet_window_count < MATRX_MAX_QUIET_WINDOWS; i++) {
            const Kd__V1__MatrxQuietWindow* w = cfg->quiet_windows[i];
            if (w == nullptr) continue;

            uint8_t day_mask = static_cast<uint8_t>(w->day_mask & 0x7F);
            if (day_mask == 0) continue;  // no active days -> drop

            quiet_window_t& dst = new_cfg.quiet_windows[new_cfg.quiet_window_count];
            dst.day_mask = day_mask;
            dst.start_hour = (w->start_hour <= 23) ? static_cast<uint8_t>(w->start_hour) : 0;
            dst.start_min = (w->start_min <= 59) ? static_cast<uint8_t>(w->start_min) : 0;
            dst.end_hour = (w->end_hour <= 23) ? static_cast<uint8_t>(w->end_hour) : 0;
            dst.end_min = (w->end_min <= 59) ? static_cast<uint8_t>(w->end_min) : 0;
            dst.enabled = w->enabled;
            new_cfg.quiet_window_count++;
        }

        config_set(&new_cfg);
    }

    void handle_join_response(Kd__V1__JoinResponse* response) {
        if (response == nullptr) return;

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
        perform_factory_reset(reason);
    }

    void handle_pin_state_change(Kd__V1__ScheduleItemSetPinState* msg) {
        if (msg == nullptr) return;
        if (!validate_uuid(msg->uuid, "pin state")) return;

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
        break;
    }
}

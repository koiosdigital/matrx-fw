#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "sdkconfig.h"
#include "esp_event.h"

#include "kd_common.h"

#include "display.h"
#include "sockets.h"
#include "sprites.h"
#include "scheduler.h"
#include "daughterboard.h"

static const char* TAG = "main";

// Daughterboard event handler
static void daughterboard_event_handler(void* arg, esp_event_base_t event_base,
    int32_t event_id, void* event_data) {
    if (event_base == DAUGHTERBOARD_EVENTS) {
        switch (event_id) {
        case DAUGHTERBOARD_EVENT_BUTTON_A_PRESSED: {
            button_event_t* btn_event = (button_event_t*)event_data;
            ESP_LOGI(TAG, "Button A pressed at timestamp: %lu", btn_event->timestamp);
            break;
        }
        case DAUGHTERBOARD_EVENT_BUTTON_B_PRESSED: {
            button_event_t* btn_event = (button_event_t*)event_data;
            ESP_LOGI(TAG, "Button B pressed at timestamp: %lu", btn_event->timestamp);
            break;
        }
        case DAUGHTERBOARD_EVENT_BUTTON_C_PRESSED: {
            button_event_t* btn_event = (button_event_t*)event_data;
            ESP_LOGI(TAG, "Button C pressed at timestamp: %lu", btn_event->timestamp);
            break;
        }
        case DAUGHTERBOARD_EVENT_LIGHT_READING: {
            light_reading_t* light_event = (light_reading_t*)event_data;
            ESP_LOGI(TAG, "Light reading: %d lux at timestamp: %lu",
                light_event->lux, light_event->timestamp);
            break;
        }
        default:
            ESP_LOGW(TAG, "Unknown daughterboard event: %ld", event_id);
            break;
        }
    }
}

extern "C" void app_main(void)
{
    //event loop
    esp_event_loop_create_default();

    // Register daughterboard event handler
    esp_event_handler_register(DAUGHTERBOARD_EVENTS, ESP_EVENT_ANY_ID,
        &daughterboard_event_handler, NULL);

    sprites_init();
    display_init();

    kd_common_set_provisioning_pop_token_format(ProvisioningPOPTokenFormat_t::NUMERIC_6);
    kd_common_init();

    scheduler_init();
    sockets_init();

    // Initialize daughterboard (light sensor and buttons)
    esp_err_t ret = daughterboard_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize daughterboard: %s", esp_err_to_name(ret));
    }

    for (;;) {
        // Main loop - can be used for other tasks or just to keep the app running
        vTaskDelay(pdMS_TO_TICKS(5000)); // Sleep for 1 secon
        ESP_LOGI(TAG, "free total heap: %" PRIu32, esp_get_free_heap_size());
    }

    vTaskSuspend(NULL);
}

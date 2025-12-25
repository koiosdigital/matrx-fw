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
#include "config.h"

static const char* TAG = "main";

extern "C" void app_main(void)
{
    //event loop
    esp_event_loop_create_default();

    display_init();

    ESP_LOGI(TAG, "post display Free internal memory: %d bytes, ext: %d bytes", esp_get_free_internal_heap_size(), esp_get_free_heap_size());

    // Initialize daughterboard (light sensor and buttons)
    esp_err_t ret = daughterboard_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize daughterboard: %s", esp_err_to_name(ret));
    }

    kd_common_set_provisioning_pop_token_format(ProvisioningPOPTokenFormat_t::NUMERIC_6);
    kd_common_init();

    ESP_LOGI(TAG, "post kdc Free internal memory: %d bytes, ext: %d bytes", esp_get_free_internal_heap_size(), esp_get_free_heap_size());

    // Initialize config module after kd_common_init (which initializes NVS)
    ret = config_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize config module: %s", esp_err_to_name(ret));
    }

    scheduler_init();

    ESP_LOGI(TAG, "post scheduler Free internal memory: %d bytes, ext: %d bytes", esp_get_free_internal_heap_size(), esp_get_free_heap_size());
    sockets_init();

    ESP_LOGI(TAG, "post sockets Free internal memory: %d bytes, ext: %d bytes", esp_get_free_internal_heap_size(), esp_get_free_heap_size());
    vTaskSuspend(NULL);
}

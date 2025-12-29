#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "sdkconfig.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_system.h"

#include "kd_common.h"

#include "display.h"
#include "webp_player.h"
#include "sockets.h"
#include "apps.h"
#include "scheduler.h"
#include "daughterboard.h"
#include "config.h"

static const char* TAG = "main";

extern "C" void app_main(void)
{
    //event loop
    esp_event_loop_create_default();

    display_init();

    // Initialize WebP player after display
    esp_err_t ret = webp_player_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize WebP player: %s", esp_err_to_name(ret));
    }

    // Show boot sprite
    show_fs_sprite("boot");
    vTaskDelay(pdMS_TO_TICKS(1200));

    ESP_LOGI(TAG, "post display Free internal memory: %d bytes, ext: %d bytes", esp_get_free_internal_heap_size(), esp_get_free_heap_size());

    // Initialize daughterboard (light sensor and buttons)
    ret = daughterboard_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize daughterboard: %s", esp_err_to_name(ret));
    }

    ESP_LOGI(TAG, "post daughterboard Free internal memory: %d bytes, ext: %d bytes", esp_get_free_internal_heap_size(), esp_get_free_heap_size());

    // Check if factory reset buttons are pressed (handle before kd_common_init)
    bool factory_reset_requested = daughterboard_is_button_pressed(0) && daughterboard_is_button_pressed(2);

    // Handle factory reset BEFORE kd_common_init
    if (factory_reset_requested) {
        ESP_LOGI(TAG, "Factory reset buttons detected, showing hold sprite");
        show_fs_sprite("factory_reset_hold");

        // Initialize NVS manually before factory reset
        ret = nvs_flash_init();
        if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
            nvs_flash_erase();
            ret = nvs_flash_init();
        }

        // Wait for 3 seconds while buttons are held
        int hold_duration_ms = 0;
        const int required_hold_ms = 3000;
        const int check_interval_ms = 100;

        while (hold_duration_ms < required_hold_ms) {
            if (!daughterboard_is_button_pressed(0) || !daughterboard_is_button_pressed(2)) {
                ESP_LOGD(TAG, "Buttons released before 3 seconds, restarting");
                vTaskDelay(pdMS_TO_TICKS(100));
                esp_restart();
            }
            vTaskDelay(pdMS_TO_TICKS(check_interval_ms));
            hold_duration_ms += check_interval_ms;
        }

        // Perform factory reset (WiFi not initialized yet, will use NVS fallback)
        perform_factory_reset("button hold");
    }

    kd_common_set_provisioning_pop_token_format(ProvisioningPOPTokenFormat_t::NUMERIC_6);

    // Show keygen sprite if key generation will occur
#ifndef KD_COMMON_CRYPTO_DISABLE
    if (kd_common_crypto_will_generate_key()) {
        show_fs_sprite("keygen");
    }
#endif

    kd_common_init();

    ESP_LOGI(TAG, "post kdc Free internal memory: %d bytes, ext: %d bytes", esp_get_free_internal_heap_size(), esp_get_free_heap_size());

    // Initialize app manager
    apps_init();

    // Initialize scheduler (registers event handlers)
    scheduler_init();
    scheduler_start();

    // Initialize config module after kd_common_init (which initializes NVS)
    ret = config_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize config module: %s", esp_err_to_name(ret));
    }

    sockets_init();

    ESP_LOGI(TAG, "post sockets Free internal memory: %d bytes, ext: %d bytes", esp_get_free_internal_heap_size(), esp_get_free_heap_size());
    vTaskDelete(nullptr);
}

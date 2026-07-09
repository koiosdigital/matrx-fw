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
#include <koios/ota.h>

#include "display.h"
#include "webp_player.h"
#include "sockets.h"
#include "render_fetch.h"
#include "apps.h"
#include "scheduler.h"
#include "daughterboard.h"
#include "config.h"

static const char* TAG = "main";

extern "C" void app_main(void)
{
    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create event loop: %s", esp_err_to_name(err));
        return;
    }
    display_init();

    esp_err_t ret;
    ret = webp_player_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize WebP player: %s", esp_err_to_name(ret));
    }

    show_fs_sprite("boot");
    vTaskDelay(pdMS_TO_TICKS(1200));

    ret = daughterboard_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize daughterboard: %s", esp_err_to_name(ret));
    }

    bool factory_reset_requested = daughterboard_is_button_pressed(0) && daughterboard_is_button_pressed(2);

    if (factory_reset_requested) {
        show_fs_sprite("factory_reset_hold");

        ret = nvs_flash_init();
        if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
            nvs_flash_erase();
            ret = nvs_flash_init();
        }

        int hold_duration_ms = 0;
        const int required_hold_ms = 3000;
        const int check_interval_ms = 100;

        while (hold_duration_ms < required_hold_ms) {
            if (!daughterboard_is_button_pressed(0) || !daughterboard_is_button_pressed(2)) {
                vTaskDelay(pdMS_TO_TICKS(100));
                esp_restart();
            }
            vTaskDelay(pdMS_TO_TICKS(check_interval_ms));
            hold_duration_ms += check_interval_ms;
        }

        perform_factory_reset("button hold");
    }

#ifdef CONFIG_KD_COMMON_CRYPTO_ENABLE
    if (kd_common_crypto_will_generate_key()) {
        show_fs_sprite("keygen");
    }
#endif

    kd_common_init();

    koios_ota_init(nullptr);

    apps_init();

    scheduler_init();
    scheduler_start();

    ret = config_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize config module: %s", esp_err_to_name(ret));
    }

    render_fetch_init();
    sockets_init();

    vTaskDelete(nullptr);
}

#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "sdkconfig.h"
#include "nvs_flash.h"

#include "crypto.h"
#include "wifi.h"
#include "provisioning.h"
#include "display.h"
#include "sockets.h"
#include "sprites.h"
#include "ota.h"

static const char* TAG = "matrx";

#define DEVELOPER_DEVICE false

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "Hello world!");

    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    //also initialize factory NVS partition
    ret = nvs_flash_init_partition(NVS_CRYPTO_PARTITION);
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase_partition(NVS_CRYPTO_PARTITION));
        ret = nvs_flash_init_partition(NVS_CRYPTO_PARTITION);
    }

    sprites_init();
    display_init();

    provisioning_init();
    wifi_init();
    sockets_init();

    crypto_init();
    ota_init();

    while (1) {
        ESP_LOGI(TAG, "Hello world!");
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
}

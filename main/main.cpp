#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "sdkconfig.h"
#include "nvs_flash.h"
#include "esp_event.h"

#include "crypto.h"
#include "wifi.h"
#include "provisioning.h"
#include "display.h"
#include "sockets.h"
#include "sprites.h"
#include "scheduler.h"
#include "ota.h"

static const char* TAG = "matrx";

#define DEVELOPER_DEVICE false

extern "C" void app_main(void)
{
    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        ret = nvs_flash_init();
    }

    //event loop
    esp_event_loop_create_default();

    sprites_init();
    display_init();

    while (1) {
        //dump memory stats
        ESP_LOGI(TAG, "Free heap: %" PRIu32, esp_get_free_heap_size());
        ESP_LOGW(TAG, "low watermark: %d", uxTaskGetStackHighWaterMark(NULL));
        vTaskDelay(pdMS_TO_TICKS(3000));
    }

    provisioning_init();
    crypto_init();

    //scheduler_init();

    wifi_init();
    //sockets_init();

    //ota_init();


}

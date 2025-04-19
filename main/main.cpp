#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "sdkconfig.h"
#include "nvs_flash.h"
#include "esp_event.h"

#include "kd_common.h"

#include "display.h"
#include "sockets.h"
#include "sprites.h"
#include "scheduler.h"

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

    kd_common_init();

    scheduler_init();
    sockets_init();
    //ota_init();

    vTaskSuspend(NULL);
}

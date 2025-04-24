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

extern "C" void app_main(void)
{
    //event loop
    esp_event_loop_create_default();

    sprites_init();
    display_init();

    kd_common_init();

    scheduler_init();
    sockets_init();

    vTaskSuspend(NULL);
}

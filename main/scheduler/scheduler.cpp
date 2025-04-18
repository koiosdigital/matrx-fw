#include "scheduler.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "esp_log.h"
#include "esp_timer.h"

#include "sprites.h"
#include "sockets.h"

#include <string.h>

#define max(a, b) (((a) > (b)) ? (a) : (b))
#define min(a, b) (((a) < (b)) ? (a) : (b))

ScheduleItem_t schedule_items[MAX_SCHEDULE_ITEMS] = { 0 };
TaskHandle_t xSchedulerTask = NULL;
uint32_t current_schedule_item = 0;

typedef enum ScheduleTaskNotification_t {
    SCHEDULE_TASK_START,
    SCHEDULE_TASK_STOP,
    SCHEDULE_TASK_SKIP,
    SCHEDULE_TASK_PIN,
    SCHEDULE_TASK_PREPARE_NEXT,
    SCHEDULE_TASK_ADVANCE,
} ScheduleTaskNotification_t;

static const char* TAG = "scheduler";

SemaphoreHandle_t schedule_mutex = NULL;

ScheduleItem_t* find_schedule_item(uint8_t* schedule_item_uuid) {
    for (int i = 0; i < MAX_SCHEDULE_ITEMS; i++) {
        ScheduleItem_t* schedule_item = &schedule_items[i];
        if (memcmp(schedule_item->uuid, schedule_item_uuid, UUID_SIZE_BYTES == 0)) {
            return schedule_item;
        }
    }

    return NULL;
}

void scheduler_set_schedule(Matrx__ScheduleResponse* schedule_response) {
    if (schedule_response->n_schedule_items == 0) {
        ESP_LOGI(TAG, "no schedule items");
        scheduler_clear();
        return;
    }

    xSemaphoreTake(schedule_mutex, portMAX_DELAY);

    size_t n_schedule_items = schedule_response->n_schedule_items;
    for (int i = 0; i < n_schedule_items; i++) {
        Matrx__ScheduleItem* item = schedule_response->schedule_items[i];

        if (item->uuid.len != UUID_SIZE_BYTES) {
            ESP_LOGE(TAG, "invalid UUID size");
            continue;
        }

        ScheduleItem_t* matched_item = find_schedule_item(item->uuid.data);

        ScheduleItem_t* schedule_item = &schedule_items[i];
        memcpy(schedule_item->uuid, item->uuid.data, UUID_SIZE_BYTES);

        schedule_item->flags.pinned = item->pinned;
        schedule_item->flags.skipped_server = item->skipped_by_server;
        schedule_item->flags.skipped_user = item->skipped_by_user;
        schedule_item->flags.display_time = item->display_time;

        if (matched_item != NULL) {
            schedule_item->sprite = matched_item->sprite;
        }
        else {
            if (schedule_item->sprite != NULL) {
                sprite_free(schedule_item->sprite);
            }

            schedule_item->sprite = sprite_allocate();
        }
    }

    for (int i = n_schedule_items; i < MAX_SCHEDULE_ITEMS; i++) {
        if (schedule_items[i].sprite != NULL) {
            sprite_free(schedule_items[i].sprite);
        }
        memset(&schedule_items[i], 0, sizeof(ScheduleItem_t));
    }

    xSemaphoreGive(schedule_mutex);
    xTaskNotify(xSchedulerTask, ScheduleTaskNotification_t::SCHEDULE_TASK_START, eSetValueWithOverwrite);
}

void scheduler_clear() {
    xSemaphoreTake(schedule_mutex, portMAX_DELAY);

    xTaskNotify(xSchedulerTask, ScheduleTaskNotification_t::SCHEDULE_TASK_STOP, eSetValueWithOverwrite);

    for (int i = 0; i < MAX_SCHEDULE_ITEMS; i++) {
        if (schedule_items[i].sprite != NULL) {
            sprite_free(schedule_items[i].sprite);
        }
        memset(&schedule_items[i], 0, sizeof(ScheduleItem_t));
    }

    xSemaphoreGive(schedule_mutex);
}

void scheduler_skip_schedule_item(uint8_t* schedule_item_uuid) {
    xSemaphoreTake(schedule_mutex, portMAX_DELAY);

    ScheduleItem_t* schedule_item = find_schedule_item(schedule_item_uuid);

    if (schedule_item != NULL) {
        schedule_item->flags.skipped_user = true;
    }

    xSemaphoreGive(schedule_mutex);
}

void scheduler_pin_schedule_item(uint8_t* schedule_item_uuid) {
    xSemaphoreTake(schedule_mutex, portMAX_DELAY);

    ScheduleItem_t* schedule_item = find_schedule_item(schedule_item_uuid);

    //unpin all
    for (int i = 0; i < MAX_SCHEDULE_ITEMS; i++) {
        schedule_items[i].flags.pinned = false;
    }

    if (schedule_item != NULL) {
        schedule_item->flags.pinned = true;
    }

    xSemaphoreGive(schedule_mutex);
    xTaskNotify(xSchedulerTask, ScheduleTaskNotification_t::SCHEDULE_TASK_ADVANCE, eSetValueWithOverwrite);
}

bool schedule_item_valid_uuid(ScheduleItem_t* item) {
    return item->uuid[0] != 0 || item->uuid[1] != 0 || item->uuid[2] != 0 || item->uuid[3] != 0;
}

void scheduler_pin_current_schedule_item() {
    xTaskNotify(xSchedulerTask, ScheduleTaskNotification_t::SCHEDULE_TASK_PIN, eSetValueWithOverwrite);
}

void scheduler_skip_current_schedule_item() {
    xTaskNotify(xSchedulerTask, ScheduleTaskNotification_t::SCHEDULE_TASK_SKIP, eSetValueWithOverwrite);
}

size_t get_schedule_size() {
    size_t size = 0;

    for (int i = 0; i < MAX_SCHEDULE_ITEMS; i++) {
        ScheduleItem_t* schedule_item = &schedule_items[i];
        if (schedule_item_valid_uuid(schedule_item)) {
            size++;
        }
    }

    return size;
}

bool scheduler_has_schedule() {
    return get_schedule_size() > 0;
}

//Called 5 seconds before an item is displayed, allows for preloading and returns the ScheduleItem_t to display
//Also requests rerenders of skipped (by server) items to ensure they still need to be skipped
ScheduleItem_t* scheduler_jump_next_item() {
    if (xSemaphoreTake(schedule_mutex, pdMS_TO_TICKS(100)) == pdFALSE) {
        ESP_LOGI(TAG, "couldn't prepare next item: mutex timeout");
        return NULL;
    }

    if (!scheduler_has_schedule()) {
        xSemaphoreGive(schedule_mutex);
        return NULL;
    }

    ScheduleItem_t* next_item = NULL;
    uint32_t next_schedule_item = current_schedule_item;
    uint32_t iterations = 0;

    //if our current item is pinned, we don't need to do anything
    if (schedule_items[current_schedule_item].flags.pinned) {
        next_item = &schedule_items[current_schedule_item];

        //but request a rerender
        request_render(schedule_items[current_schedule_item].uuid);
        goto exit;
    }

    while (true) {
        next_schedule_item++;
        iterations++;

        //loop break
        if (iterations > MAX_SCHEDULE_ITEMS + 1) {
            xSemaphoreGive(schedule_mutex);
            return NULL;
        }

        //wrap around
        if (next_schedule_item >= MAX_SCHEDULE_ITEMS) {
            next_schedule_item = 0;
        }

        //skip if not valid
        if (!schedule_item_valid_uuid(&schedule_items[next_schedule_item])) {
            continue;
        }

        //skip if marked skipped by server
        if (schedule_items[next_schedule_item].flags.skipped_server) {
            request_render(schedule_items[next_schedule_item].uuid);
            continue;
        }

        //skip if marked skipped by user, doesn't need a rerender
        if (schedule_items[next_schedule_item].flags.skipped_user) {
            continue;
        }

        //if we don't have a sprite for this item yet, request a rerender and skip
        if (schedule_items[next_schedule_item].sprite == NULL) {
            request_render(schedule_items[next_schedule_item].uuid);
            continue;
        }
    }

    next_item = &schedule_items[next_schedule_item];
    request_render(schedule_items[next_schedule_item].uuid);

exit:
    xSemaphoreGive(schedule_mutex);
    return next_item;
}

void scheduler_prepare_timer_handler(void* arg) {
    xTaskNotify(xSchedulerTask, ScheduleTaskNotification_t::SCHEDULE_TASK_PREPARE_NEXT, eSetValueWithOverwrite);
}

void scheduler_display_timer_handler(void* arg) {
    xTaskNotify(xSchedulerTask, ScheduleTaskNotification_t::SCHEDULE_TASK_ADVANCE, eSetValueWithOverwrite);
}

void scheduler_task(void* pvParameter) {
    ScheduleTaskNotification_t notification;
    ScheduleItem_t* next_item = NULL;
    esp_timer_handle_t prepare_timer = NULL;
    esp_timer_handle_t display_timer = NULL;

    //setup timers
    esp_timer_create_args_t prepare_timer_args = {
        .callback = &scheduler_prepare_timer_handler,
        .arg = NULL,
        .name = "prepare_timer",
    };

    esp_timer_create_args_t display_timer_args = {
        .callback = &scheduler_display_timer_handler,
        .arg = NULL,
        .name = "display_timer",
    };

    esp_timer_create(&prepare_timer_args, &prepare_timer);
    esp_timer_create(&display_timer_args, &display_timer);

    while (1) {
        if (xTaskNotifyWait(0, ULONG_MAX, (uint32_t*)&notification, portMAX_DELAY) == pdTRUE) {
            switch (notification) {
            case SCHEDULE_TASK_START:
                if (next_item == NULL) {
                    next_item = scheduler_jump_next_item();
                }

                if (next_item == NULL) {
                    ESP_LOGI(TAG, "no schedule items");
                    continue;
                }

                esp_timer_start_once(prepare_timer, 0);
                esp_timer_start_periodic(display_timer, 1000);

                break;
            case SCHEDULE_TASK_STOP:
                next_item = NULL;
                break;
            case SCHEDULE_TASK_SKIP:
                xSemaphoreTake(schedule_mutex, portMAX_DELAY);
                schedule_items[current_schedule_item].flags.skipped_user = !schedule_items[current_schedule_item].flags.skipped_user;
                xSemaphoreGive(schedule_mutex);
                xTaskNotify(xSchedulerTask, ScheduleTaskNotification_t::SCHEDULE_TASK_ADVANCE, eSetValueWithOverwrite);
                break;
            case SCHEDULE_TASK_PIN:
                xSemaphoreTake(schedule_mutex, portMAX_DELAY);
                schedule_items[current_schedule_item].flags.pinned = !schedule_items[current_schedule_item].flags.pinned;
                xSemaphoreGive(schedule_mutex);
                xTaskNotify(xSchedulerTask, ScheduleTaskNotification_t::SCHEDULE_TASK_ADVANCE, eSetValueWithOverwrite);
                break;
            case SCHEDULE_TASK_PREPARE_NEXT:
                next_item = scheduler_jump_next_item();
                if (next_item == NULL) {
                    ESP_LOGI(TAG, "no schedule items");
                    xTaskNotify(xSchedulerTask, ScheduleTaskNotification_t::SCHEDULE_TASK_STOP, eSetValueWithOverwrite);
                    request_schedule();
                    continue;
                }
                break;
            case SCHEDULE_TASK_ADVANCE:
                if (next_item == NULL) {
                    continue;
                }

                show_sprite(next_item->sprite);

                xSemaphoreTake(schedule_mutex, portMAX_DELAY);

                //advance to the next item in the schedule by setting a timer
                esp_timer_stop(prepare_timer);

                uint64_t display_time_ms = next_item->flags.display_time * 1000;

                uint64_t prepare_timeout = max(display_time_ms - PREPARE_TIME, 0);
                esp_timer_start_once(prepare_timer, prepare_timeout);

                //and then advance to the next item in the schedule at the display time
                esp_timer_stop(display_timer);
                esp_timer_start_once(display_timer, display_time_ms);
                break;
            }
        }
    }
}

void scheduler_init() {
    schedule_mutex = xSemaphoreCreateBinary();
    xTaskCreate(scheduler_task, "scheduler", 2048, NULL, 5, &xSchedulerTask);
}
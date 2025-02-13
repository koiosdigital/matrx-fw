#include "scheduler.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "esp_log.h"

#include "sprites.h"

#include <string.h>

ScheduleItem_t* schedule_head;
uint8_t* fs_sprite_buf = NULL;

typedef enum ScheduleTaskNotification_t {
    SCHEDULE_TASK_NOTIFICATION_PAUSE,
    SCHEDULE_TASK_NOTIFICATION_RESUME,
    SCHEDULE_TASK_SKIP,
    SCHEDULE_TASK_PIN,
} ScheduleTaskNotification_t;

static const char* TAG = "scheduler";

SemaphoreHandle_t schedule_mutex = NULL;

void scheduler_task(void* pvParameter) {
    ScheduleTaskNotification_t notification;

    while (1) {
        if (xTaskNotifyWait(0, ULONG_MAX, (uint32_t*)&notification, pdMS_TO_TICKS(1000)) == pdTRUE) {
            switch (notification) {
            case SCHEDULE_TASK_NOTIFICATION_PAUSE:
                ESP_LOGI(TAG, "pause");
                break;
            case SCHEDULE_TASK_NOTIFICATION_RESUME:
                ESP_LOGI(TAG, "resume");
                break;
            case SCHEDULE_TASK_SKIP:
                ESP_LOGI(TAG, "skip");
                break;
            case SCHEDULE_TASK_PIN:
                ESP_LOGI(TAG, "pin");
                break;
            default:
                break;
            }
        }
    }
}

void scheduler_set_schedule(ScheduleItem_t* head) {
    xSemaphoreTake(schedule_mutex, portMAX_DELAY);

    ScheduleItem_t* current = head;
    ScheduleItem_t* prev = NULL;

    while (current != NULL) {
        ScheduleItem_t* new_item = (ScheduleItem_t*)malloc(sizeof(ScheduleItem_t));

        new_item->schedule_item_uuid = (char*)malloc(strlen(current->schedule_item_uuid) + 1);
        strcpy(new_item->schedule_item_uuid, current->schedule_item_uuid);

        new_item->schedule_position = current->schedule_position;
        new_item->display_time = current->display_time;
        new_item->pinned = current->pinned;
        new_item->skipped = current->skipped;

        new_item->next = NULL;

        if (prev == NULL) {
            schedule_head = new_item;
        }
        else {
            prev->next = new_item;
        }

        prev = new_item;
        current = current->next;
    }

    xSemaphoreGive(schedule_mutex);
}

//Runs every 60 seconds, used to clear out any sprites that are not in the schedule
void scheduler_clear_unused_sprites() {
    xSemaphoreTake(schedule_mutex, portMAX_DELAY);

    ScheduleItem_t* current = schedule_head;
    RAMSprite_t* sprite_current = sprites_get_head();
    RAMSprite_t* sprite_head = sprite_current;

    RAMSprite_t* sprite_prev = NULL;

    while (sprite_current != NULL) {
        bool found = false;

        while (current != NULL) {
            if (strcmp(current->schedule_item_uuid, sprite_current->uuid) == 0) {
                found = true;
                break;
            }

            current = current->next;
        }

        if (!found) {
            //delete the sprite
            if (sprite_prev == NULL) {
                sprite_head = sprite_current->next;
            }
            else {
                sprite_prev->next = sprite_current->next;
            }

            free(sprite_current->uuid);
            free(sprite_current->data);
            free(sprite_current);

            sprite_current = sprite_head;
        }
        else {
            sprite_prev = sprite_current;
            sprite_current = sprite_current->next;
        }
    }

    xSemaphoreGive(schedule_mutex);
}

void scheduler_init() {
    //initialize the linked list
    schedule_head = NULL;

    schedule_mutex = xSemaphoreCreateBinary();
}

void scheduler_clear() {
    xSemaphoreTake(schedule_mutex, portMAX_DELAY);

    ScheduleItem_t* current = schedule_head;
    ScheduleItem_t* next;

    while (current != NULL) {
        next = current->next;
        free(current->schedule_item_uuid);
        free(current);
        current = next;
    }

    schedule_head = NULL;

    xSemaphoreGive(schedule_mutex);
}

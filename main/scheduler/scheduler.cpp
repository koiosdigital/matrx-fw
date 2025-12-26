#include "scheduler.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "esp_log.h"

#include "sprites.h"
#include "sockets.h"
#include "daughterboard.h"
#include "display.h"

#include <string.h>

static const char* TAG = "scheduler";

// Forward declarations
static void request_renders_for_upcoming_items(void);
static bool has_displayable_items(void);

// Scheduler state
ScheduleItem_t schedule_items[MAX_SCHEDULE_ITEMS] = { 0 };
TaskHandle_t xSchedulerTask = NULL;
SemaphoreHandle_t schedule_mutex = NULL;

// Current scheduler state
uint32_t current_schedule_item = 0;
bool scheduler_running = false;
bool has_valid_schedule = false;

// Task notifications
typedef enum ScheduleTaskNotification_t {
    SCHEDULE_TASK_SKIP_CURRENT,
    SCHEDULE_TASK_PIN_CURRENT,
    SCHEDULE_TASK_NEXT_ITEM,
    SCHEDULE_TASK_PREVIOUS_ITEM,
} ScheduleTaskNotification_t;

// Button event handler for scheduler navigation
static void button_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_base != DAUGHTERBOARD_EVENTS) {
        return;
    }

    switch (event_id) {
    case DAUGHTERBOARD_EVENT_BUTTON_A_PRESSED:
        if (xSchedulerTask != NULL) {
            xTaskNotify(xSchedulerTask, SCHEDULE_TASK_PREVIOUS_ITEM, eSetValueWithOverwrite);
        }
        break;

    case DAUGHTERBOARD_EVENT_BUTTON_C_PRESSED:
        if (xSchedulerTask != NULL) {
            xTaskNotify(xSchedulerTask, SCHEDULE_TASK_NEXT_ITEM, eSetValueWithOverwrite);
        }
        break;

    default:
        break;
    }
}

// Helper functions
bool schedule_item_valid_uuid(ScheduleItem_t* item) {
    return item->uuid[0] != 0 || item->uuid[1] != 0 || item->uuid[2] != 0 || item->uuid[3] != 0;
}

ScheduleItem_t* find_schedule_item(uint8_t* schedule_item_uuid) {
    for (int i = 0; i < MAX_SCHEDULE_ITEMS; i++) {
        ScheduleItem_t* schedule_item = &schedule_items[i];
        if (memcmp(schedule_item->uuid, schedule_item_uuid, UUID_SIZE_BYTES) == 0) {
            return schedule_item;
        }
    }
    return NULL;
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

static bool has_displayable_items() {
    for (int i = 0; i < MAX_SCHEDULE_ITEMS; i++) {
        ScheduleItem_t* item = &schedule_items[i];
        if (schedule_item_valid_uuid(item) &&
            !item->flags.skipped_user &&
            !item->flags.skipped_server) {
            return true;
        }
    }
    return false;
}

// Helper function to find next valid displayable item (wraps around)
static uint32_t find_next_displayable_item(uint32_t start_index) {
    uint32_t next = start_index + 1;
    uint32_t checked = 0;

    while (checked < MAX_SCHEDULE_ITEMS) {
        // Wrap around
        if (next >= MAX_SCHEDULE_ITEMS || !schedule_item_valid_uuid(&schedule_items[next])) {
            next = 0;
        }

        // Found a displayable item
        if (schedule_item_valid_uuid(&schedule_items[next]) &&
            !schedule_items[next].flags.skipped_user &&
            !schedule_items[next].flags.skipped_server) {
            return next;
        }

        next++;
        checked++;
    }

    // No displayable items found, return start index
    return start_index;
}

// Main scheduler loop that runs every second
TickType_t sprite_start_tick = 0;
uint64_t last_schedule_request_time = 0;
bool need_to_skip = false;

static void request_renders_for_upcoming_items() {
    if (!has_valid_schedule) {
        return;
    }

    ScheduleItem_t* current_item = &schedule_items[current_schedule_item];
    if (!schedule_item_valid_uuid(current_item)) {
        return;
    }

    uint64_t current_time = pdTICKS_TO_MS(xTaskGetTickCount());
    uint64_t time_since_start = current_time - pdTICKS_TO_MS(sprite_start_tick);
    uint32_t display_duration = current_item->flags.display_time * 1000;

    // Check if we're within PREPARE_TIME seconds before the next item
    if (time_since_start >= (display_duration - PREPARE_TIME) && time_since_start < display_duration) {
        // If pinned item, request render for itself
        if (current_item->flags.pinned) {
            request_render(current_item->uuid);
            return;
        }

        // Request render for the next displayable item
        uint32_t next = find_next_displayable_item(current_schedule_item);
        if (next != current_schedule_item && schedule_item_valid_uuid(&schedule_items[next])) {
            request_render(schedule_items[next].uuid);
        }
    }
}

void scheduler_task(void* pvParameter) {
    while (1) {
        // Wait for 1 second tick
        vTaskDelay(pdMS_TO_TICKS(1000));

        if (!scheduler_running) {
            continue;
        }

        xSemaphoreTake(schedule_mutex, portMAX_DELAY);

        // Handle task notifications
        uint32_t notification_value = 0;
        if (xTaskNotifyWait(0, ULONG_MAX, &notification_value, 0) == pdTRUE) {
            switch (notification_value) {
            case SCHEDULE_TASK_SKIP_CURRENT:
                need_to_skip = true;
                break;
            case SCHEDULE_TASK_PIN_CURRENT:
                // Find and pin current item
                if (has_valid_schedule && current_schedule_item < MAX_SCHEDULE_ITEMS) {
                    ScheduleItem_t* current_item = &schedule_items[current_schedule_item];
                    if (schedule_item_valid_uuid(current_item)) {
                        // Unpin all other items first
                        for (int i = 0; i < MAX_SCHEDULE_ITEMS; i++) {
                            schedule_items[i].flags.pinned = false;
                        }
                        current_item->flags.pinned = true;
                        ESP_LOGD(TAG, "Pinned current schedule item %d", current_schedule_item);
                    }
                }
                break;
            case SCHEDULE_TASK_NEXT_ITEM:
                need_to_skip = true;
                break;
            case SCHEDULE_TASK_PREVIOUS_ITEM:
                // Go to previous valid item
                if (has_valid_schedule) {
                    uint32_t prev_item = current_schedule_item;
                    do {
                        if (prev_item == 0) {
                            // Find last valid item
                            prev_item = MAX_SCHEDULE_ITEMS - 1;
                            while (prev_item > 0 && !schedule_item_valid_uuid(&schedule_items[prev_item])) {
                                prev_item--;
                            }
                        }
                        else {
                            prev_item--;
                        }
                    } while (prev_item != current_schedule_item &&
                        (schedule_items[prev_item].flags.skipped_user ||
                            schedule_items[prev_item].flags.skipped_server));

                    if (prev_item != current_schedule_item) {
                        current_schedule_item = prev_item;
                        sprite_start_tick = xTaskGetTickCount();
                        show_sprite(schedule_items[current_schedule_item].sprite);
                        // Notify server of currently displaying sprite
                        sockets_send_currently_displaying(schedule_items[current_schedule_item].uuid);
                        ESP_LOGD(TAG, "Moved to previous item %d", current_schedule_item);
                    }
                }
                break;
            }
        }

        // Check if we need to request a new schedule
        uint64_t current_time = pdTICKS_TO_MS(xTaskGetTickCount());
        if (!has_valid_schedule || (current_time - last_schedule_request_time > (60 * 5 * 1000))) {
            request_schedule();
            ESP_LOGD(TAG, "Requesting latest schedule...");
            last_schedule_request_time = current_time;

            // Only skip loop if no schedule at all
            if (!has_valid_schedule) {
                xSemaphoreGive(schedule_mutex);
                continue;
            }
        }

        // Request renders for upcoming items
        request_renders_for_upcoming_items();

        // Main schedule timing logic
        if (has_valid_schedule && current_schedule_item < MAX_SCHEDULE_ITEMS) {
            ScheduleItem_t* current_item = &schedule_items[current_schedule_item];

            if (schedule_item_valid_uuid(current_item)) {
                uint64_t time_since_start = current_time - pdTICKS_TO_MS(sprite_start_tick);
                uint32_t display_duration = current_item->flags.display_time * 1000; // Convert to milliseconds

                // Check if it's time to move to next item
                if (time_since_start >= display_duration || need_to_skip) {
                    if (current_item->flags.pinned && !need_to_skip) {
                        // Pinned item: stay on current item, just reset timer
                        sprite_start_tick = xTaskGetTickCount();
                    }
                    else {
                        // Move to next displayable item
                        uint32_t next_item = find_next_displayable_item(current_schedule_item);

                        // Check if we actually found a different item
                        if (next_item == current_schedule_item) {
                            ESP_LOGW(TAG, "All items are skipped, clearing display");
                            display_clear();
                        }
                        else {
                            current_schedule_item = next_item;
                            sprite_start_tick = xTaskGetTickCount();

                            // Display the sprite
                            if (schedule_items[current_schedule_item].sprite != NULL) {
                                show_sprite(schedule_items[current_schedule_item].sprite);
                            }

                            // Notify server of currently displaying sprite
                            sockets_send_currently_displaying(schedule_items[current_schedule_item].uuid);

                            ESP_LOGD(TAG, "Advanced to schedule item %d", current_schedule_item);
                        }
                    }

                    need_to_skip = false;
                }
            }
        }

        // Check if there are any displayable items, if not clear the display
        if (has_valid_schedule && !has_displayable_items()) {
            display_clear();
            ESP_LOGD(TAG, "No displayable items available, clearing display");
        }

        xSemaphoreGive(schedule_mutex);
    }
}

void scheduler_set_schedule(Kd__V1__MatrxSchedule* schedule_response) {
    if (schedule_response == NULL || schedule_response->n_schedule_items == 0) {
        scheduler_clear();
        return;
    }

    xSemaphoreTake(schedule_mutex, portMAX_DELAY);

    for (int i = 0; i < (int)schedule_response->n_schedule_items; i++) {
        Kd__V1__ScheduleItem* item = schedule_response->schedule_items[i];
        if (item == NULL || item->uuid.len != UUID_SIZE_BYTES || item->uuid.data == NULL) {
            continue;
        }

        ScheduleItem_t* matched_item = find_schedule_item(item->uuid.data);
        ScheduleItem_t* schedule_item = &schedule_items[i];

        memcpy(schedule_item->uuid, item->uuid.data, UUID_SIZE_BYTES);
        schedule_item->flags.pinned = item->pinned;
        schedule_item->flags.skipped_server = false;
        schedule_item->flags.skipped_user = item->skipped;
        schedule_item->flags.display_time = item->display_time;
        // Preserve existing sprite if item already exists
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

    // Clear unused slots
    for (int i = (int)schedule_response->n_schedule_items; i < MAX_SCHEDULE_ITEMS; i++) {
        if (schedule_items[i].sprite != NULL) {
            sprite_free(schedule_items[i].sprite);
        }
        memset(&schedule_items[i], 0, sizeof(ScheduleItem_t));
    }

    has_valid_schedule = true;
    scheduler_running = true;

    // Start with first valid, non-skipped item
    current_schedule_item = 0;
    while (current_schedule_item < MAX_SCHEDULE_ITEMS &&
        (schedule_items[current_schedule_item].flags.skipped_user ||
            schedule_items[current_schedule_item].flags.skipped_server ||
            !schedule_item_valid_uuid(&schedule_items[current_schedule_item]))) {
        current_schedule_item++;
    }

    // If no valid items found, start with first item anyway
    if (current_schedule_item >= MAX_SCHEDULE_ITEMS) {
        current_schedule_item = 0;
    }

    sprite_start_tick = xTaskGetTickCount();

    //request renders for the first few items
    for (int j = 0; j < 3; j++) {
        uint32_t index = current_schedule_item + j;
        if (index >= MAX_SCHEDULE_ITEMS) {
            index = index % MAX_SCHEDULE_ITEMS;
        }
        ScheduleItem_t* item = &schedule_items[index];
        if (schedule_item_valid_uuid(item) && !item->flags.skipped_user) {
            request_render(item->uuid);
        }
    }

    // Check if there are any displayable items
    if (!has_displayable_items()) {
        display_clear();
        ESP_LOGD(TAG, "No displayable items in new schedule, clearing display");
    }
    else {
        // Display the first sprite immediately
        if (schedule_items[current_schedule_item].sprite != NULL) {
            show_sprite(schedule_items[current_schedule_item].sprite);
        }
        // Notify server of currently displaying sprite
        sockets_send_currently_displaying(schedule_items[current_schedule_item].uuid);
    }

    ESP_LOGD(TAG, "Schedule updated with %d items, starting with item %d", schedule_response->n_schedule_items, current_schedule_item);
    xSemaphoreGive(schedule_mutex);
}

void scheduler_clear() {
    xSemaphoreTake(schedule_mutex, portMAX_DELAY);

    for (int i = 0; i < MAX_SCHEDULE_ITEMS; i++) {
        if (schedule_items[i].sprite != NULL) {
            sprite_free(schedule_items[i].sprite);
        }
        memset(&schedule_items[i], 0, sizeof(ScheduleItem_t));
    }

    has_valid_schedule = false;
    scheduler_running = false;
    current_schedule_item = 0;

    // Clear the display when schedule is cleared
    display_clear();
    ESP_LOGD(TAG, "Schedule cleared, display cleared");

    xSemaphoreGive(schedule_mutex);
}

void scheduler_skip_schedule_item(uint8_t* schedule_item_uuid) {
    xSemaphoreTake(schedule_mutex, portMAX_DELAY);

    ScheduleItem_t* schedule_item = find_schedule_item(schedule_item_uuid);
    if (schedule_item != NULL) {
        schedule_item->flags.skipped_user = true;

        // Check if no items are displayable after this skip
        if (!has_displayable_items()) {
            display_clear();
            ESP_LOGD(TAG, "No displayable items remaining after skip, clearing display");
        }
    }

    xSemaphoreGive(schedule_mutex);
}

void scheduler_pin_schedule_item(uint8_t* schedule_item_uuid) {
    xSemaphoreTake(schedule_mutex, portMAX_DELAY);

    // Unpin all items first
    for (int i = 0; i < MAX_SCHEDULE_ITEMS; i++) {
        schedule_items[i].flags.pinned = false;
    }

    // Find and pin the specific item
    ScheduleItem_t* schedule_item = find_schedule_item(schedule_item_uuid);
    if (schedule_item != NULL) {
        schedule_item->flags.pinned = true;

        // If this item is different from current, switch to it
        for (int i = 0; i < MAX_SCHEDULE_ITEMS; i++) {
            if (&schedule_items[i] == schedule_item) {
                current_schedule_item = i;
                sprite_start_tick = xTaskGetTickCount();
                if (schedule_item->sprite != NULL) {
                    show_sprite(schedule_item->sprite);
                }
                // Notify server of currently displaying sprite
                sockets_send_currently_displaying(schedule_item->uuid);
                ESP_LOGD(TAG, "Switched to and pinned schedule item %d", i);
                break;
            }
        }
    }

    xSemaphoreGive(schedule_mutex);
}

void scheduler_pin_current_schedule_item() {
    if (xSchedulerTask != NULL) {
        xTaskNotify(xSchedulerTask, SCHEDULE_TASK_PIN_CURRENT, eSetValueWithOverwrite);
    }
}

void scheduler_skip_current_schedule_item() {
    if (xSchedulerTask != NULL) {
        xTaskNotify(xSchedulerTask, SCHEDULE_TASK_SKIP_CURRENT, eSetValueWithOverwrite);
    }
}

void scheduler_goto_next_item() {
    if (xSchedulerTask != NULL) {
        xTaskNotify(xSchedulerTask, SCHEDULE_TASK_NEXT_ITEM, eSetValueWithOverwrite);
    }
}

void scheduler_goto_previous_item() {
    if (xSchedulerTask != NULL) {
        xTaskNotify(xSchedulerTask, SCHEDULE_TASK_PREVIOUS_ITEM, eSetValueWithOverwrite);
    }
}

void scheduler_start() {
    xSemaphoreTake(schedule_mutex, portMAX_DELAY);
    scheduler_running = true;
    xSemaphoreGive(schedule_mutex);
}

void scheduler_stop() {
    xSemaphoreTake(schedule_mutex, portMAX_DELAY);
    scheduler_running = false;
    xSemaphoreGive(schedule_mutex);
}

void scheduler_init() {
    // Initialize timing variables
    sprite_start_tick = 0;
    last_schedule_request_time = 0;
    need_to_skip = false;

    schedule_mutex = xSemaphoreCreateBinary();
    xSemaphoreGive(schedule_mutex);

    // Create the scheduler task
    xTaskCreate(scheduler_task, "scheduler", 6144, NULL, 5, &xSchedulerTask);

    esp_event_handler_register(DAUGHTERBOARD_EVENTS, DAUGHTERBOARD_EVENT_BUTTON_A_PRESSED, button_event_handler, NULL);
    esp_event_handler_register(DAUGHTERBOARD_EVENTS, DAUGHTERBOARD_EVENT_BUTTON_C_PRESSED, button_event_handler, NULL);
}
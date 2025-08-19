#include "scheduler.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "esp_log.h"

#include "sprites.h"
#include "sockets.h"
#include "daughterboard.h"

#include <string.h>

static const char* TAG = "scheduler";

// Scheduler state
ScheduleItem_t schedule_items[MAX_SCHEDULE_ITEMS] = { 0 };
TaskHandle_t xSchedulerTask = NULL;
SemaphoreHandle_t schedule_mutex = NULL;

// Current scheduler state
uint32_t current_schedule_item = 0;
uint32_t current_display_time_remaining = 0;
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

// Find next valid item to display
uint32_t find_next_valid_item() {
    if (!has_valid_schedule) {
        return MAX_SCHEDULE_ITEMS; // Invalid index
    }

    // If current item is pinned and valid, stay on it
    if (current_schedule_item < MAX_SCHEDULE_ITEMS &&
        schedule_item_valid_uuid(&schedule_items[current_schedule_item]) &&
        schedule_items[current_schedule_item].flags.pinned) {
        return current_schedule_item;
    }

    // Find next valid, non-skipped item
    uint32_t next_item = current_schedule_item;
    for (uint32_t iterations = 0; iterations < MAX_SCHEDULE_ITEMS; iterations++) {
        next_item = (next_item + 1) % MAX_SCHEDULE_ITEMS;

        if (!schedule_item_valid_uuid(&schedule_items[next_item])) {
            continue;
        }

        if (schedule_items[next_item].flags.skipped_user) {
            continue;
        }

        // Found valid item
        return next_item;
    }

    return MAX_SCHEDULE_ITEMS; // No valid items found
}

// Find previous valid item to display
uint32_t find_previous_valid_item() {
    if (!has_valid_schedule) {
        return MAX_SCHEDULE_ITEMS; // Invalid index
    }

    // Find previous valid, non-skipped item
    uint32_t prev_item = current_schedule_item;
    for (uint32_t iterations = 0; iterations < MAX_SCHEDULE_ITEMS; iterations++) {
        // Move backwards with wraparound
        if (prev_item == 0) {
            prev_item = MAX_SCHEDULE_ITEMS - 1;
        }
        else {
            prev_item--;
        }

        if (!schedule_item_valid_uuid(&schedule_items[prev_item])) {
            continue;
        }

        if (schedule_items[prev_item].flags.skipped_user) {
            continue;
        }

        // Found valid item
        return prev_item;
    }

    return MAX_SCHEDULE_ITEMS; // No valid items found
}

// Request render for items that need it
void request_renders_for_upcoming_items() {
    if (!has_valid_schedule) {
        return;
    }

    uint32_t next_item = find_next_valid_item();
    if (next_item >= MAX_SCHEDULE_ITEMS) {
        return;
    }

    // Handle current item rerender scenarios
    if (next_item == current_schedule_item) {
        // Pinned items: request rerender 1 second before expiry
        if (schedule_items[current_schedule_item].flags.pinned && current_display_time_remaining <= 1) {
            request_render(schedule_items[current_schedule_item].uuid);
        }
        // Single item schedules: request rerender exactly 3 seconds before expiry
        else if (get_schedule_size() == 1 && current_display_time_remaining == 3) {
            request_render(schedule_items[current_schedule_item].uuid);
        }
        return;
    }

    // Multi-item schedules: request rerender for next item 3 seconds before current item expires
    if (get_schedule_size() > 1 && current_display_time_remaining == 3) {
        request_render(schedule_items[next_item].uuid);
    }

    // Helper function to check if item needs rendering
    auto needs_render = [](uint32_t item_idx) {
        if (schedule_items[item_idx].sprite == NULL) {
            return true;
        }
        // Use thread-safe function to check if sprite has data
        size_t len = sprite_get_length(schedule_items[item_idx].sprite);
        return len == 0 || schedule_items[item_idx].flags.skipped_server;
        };

    // Request render for next item if needed
    if (needs_render(next_item)) {
        request_render(schedule_items[next_item].uuid);
    }

    // Look ahead and request renders for upcoming items (skip user-skipped items)
    uint32_t look_ahead_item = next_item;
    for (int i = 0; i < 3; i++) {
        look_ahead_item = (look_ahead_item + 1) % MAX_SCHEDULE_ITEMS;

        if (!schedule_item_valid_uuid(&schedule_items[look_ahead_item]) ||
            schedule_items[look_ahead_item].flags.skipped_user) {
            continue;
        }

        if (needs_render(look_ahead_item)) {
            request_render(schedule_items[look_ahead_item].uuid);
        }
    }
}

// Main scheduler loop that runs every second
void scheduler_task(void* pvParameter) {
    TickType_t last_wake_time = xTaskGetTickCount();

    while (1) {
        // Wait for 1 second tick
        vTaskDelayUntil(&last_wake_time, pdMS_TO_TICKS(1000));

        // Check for notifications (skip/pin commands)
        uint32_t notification_value;
        if (xTaskNotifyWait(0, ULONG_MAX, &notification_value, 0) == pdTRUE) {
            ScheduleTaskNotification_t notification = (ScheduleTaskNotification_t)notification_value;

            xSemaphoreTake(schedule_mutex, portMAX_DELAY);

            switch (notification) {
            case SCHEDULE_TASK_SKIP_CURRENT:
                if (current_schedule_item < MAX_SCHEDULE_ITEMS &&
                    schedule_item_valid_uuid(&schedule_items[current_schedule_item])) {
                    schedule_items[current_schedule_item].flags.skipped_user = true;
                    schedule_items[current_schedule_item].flags.pinned = false;
                    current_display_time_remaining = 0; // Force immediate advance
                }
                break;

            case SCHEDULE_TASK_PIN_CURRENT:
                if (current_schedule_item < MAX_SCHEDULE_ITEMS &&
                    schedule_item_valid_uuid(&schedule_items[current_schedule_item])) {
                    // Unpin all items first
                    for (int i = 0; i < MAX_SCHEDULE_ITEMS; i++) {
                        schedule_items[i].flags.pinned = false;
                    }
                    // Pin current item
                    schedule_items[current_schedule_item].flags.pinned = true;
                }
                break;

            case SCHEDULE_TASK_NEXT_ITEM:
            {
                uint32_t next_item = find_next_valid_item();
                if (next_item < MAX_SCHEDULE_ITEMS && next_item != current_schedule_item) {
                    current_schedule_item = next_item;
                    current_display_time_remaining = schedule_items[next_item].flags.display_time;

                    // Unpin all items when manually navigating
                    for (int i = 0; i < MAX_SCHEDULE_ITEMS; i++) {
                        schedule_items[i].flags.pinned = false;
                    }

                    // Display the item immediately if it has valid sprite data
                    if (schedule_items[next_item].sprite != NULL &&
                        sprite_get_length(schedule_items[next_item].sprite) > 0) {
                        show_sprite(schedule_items[next_item].sprite);
                    }
                    else {
                        request_render(schedule_items[next_item].uuid);
                    }
                }
            }
            break;

            case SCHEDULE_TASK_PREVIOUS_ITEM:
            {
                uint32_t prev_item = find_previous_valid_item();
                if (prev_item < MAX_SCHEDULE_ITEMS && prev_item != current_schedule_item) {
                    current_schedule_item = prev_item;
                    current_display_time_remaining = schedule_items[prev_item].flags.display_time;

                    // Unpin all items when manually navigating
                    for (int i = 0; i < MAX_SCHEDULE_ITEMS; i++) {
                        schedule_items[i].flags.pinned = false;
                    }

                    // Display the item immediately if it has valid sprite data
                    if (schedule_items[prev_item].sprite != NULL &&
                        sprite_get_length(schedule_items[prev_item].sprite) > 0) {
                        show_sprite(schedule_items[prev_item].sprite);
                    }
                    else {
                        request_render(schedule_items[prev_item].uuid);
                    }
                }
            }
            break;
            }

            xSemaphoreGive(schedule_mutex);
        }

        // If scheduler is not running, request schedule and continue
        if (!scheduler_running) {
            continue;
        }

        xSemaphoreTake(schedule_mutex, portMAX_DELAY);

        // Request renders for upcoming items
        request_renders_for_upcoming_items();

        // Handle display timing
        if (current_display_time_remaining > 0) {
            current_display_time_remaining--;

            // If current item is pinned, reset display time when it expires
            if (current_schedule_item < MAX_SCHEDULE_ITEMS &&
                schedule_items[current_schedule_item].flags.pinned &&
                current_display_time_remaining == 0) {
                current_display_time_remaining = schedule_items[current_schedule_item].flags.display_time;
            }
        }

        // Advance to next item if display time expired and not pinned
        if (current_display_time_remaining == 0) {
            uint32_t next_item = find_next_valid_item();

            if (next_item >= MAX_SCHEDULE_ITEMS) {
                has_valid_schedule = false;
                xSemaphoreGive(schedule_mutex);
                continue;
            }

            // Skip server-skipped items
            if (schedule_items[next_item].flags.skipped_server) {
                current_schedule_item = next_item;
                current_display_time_remaining = 0; // Force immediate advance to next
                xSemaphoreGive(schedule_mutex);
                continue;
            }

            // Check if item has valid sprite data
            if (schedule_items[next_item].sprite == NULL) {
                request_render(schedule_items[next_item].uuid);
                current_schedule_item = next_item;
                current_display_time_remaining = 0; // Force immediate advance to next
                xSemaphoreGive(schedule_mutex);
                continue;
            }

            // Use thread-safe function to check sprite data
            size_t sprite_len = sprite_get_length(schedule_items[next_item].sprite);
            if (sprite_len == 0) {
                request_render(schedule_items[next_item].uuid);
                current_schedule_item = next_item;
                current_display_time_remaining = 0; // Force immediate advance to next
                xSemaphoreGive(schedule_mutex);
                continue;
            }

            // Display the item
            current_schedule_item = next_item;
            current_display_time_remaining = schedule_items[next_item].flags.display_time;

            ESP_LOGI(TAG, "Displaying item %d for %d seconds",
                current_schedule_item, current_display_time_remaining);

            show_sprite(schedule_items[next_item].sprite);
        }

        xSemaphoreGive(schedule_mutex);
    }
}

void scheduler_set_schedule(Kd__ScheduleResponse* schedule_response) {
    if (schedule_response->n_schedule_items == 0) {
        scheduler_clear();
        return;
    }

    xSemaphoreTake(schedule_mutex, portMAX_DELAY);

    size_t n_schedule_items = schedule_response->n_schedule_items;
    for (int i = 0; i < n_schedule_items; i++) {
        Kd__ScheduleItem* item = schedule_response->schedule_items[i];

        if (item->uuid.len != UUID_SIZE_BYTES) {
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
    for (int i = n_schedule_items; i < MAX_SCHEDULE_ITEMS; i++) {
        if (schedule_items[i].sprite != NULL) {
            sprite_free(schedule_items[i].sprite);
        }
        memset(&schedule_items[i], 0, sizeof(ScheduleItem_t));
    }

    has_valid_schedule = true;
    scheduler_running = true;
    current_schedule_item = 0;
    current_display_time_remaining = 0;

    ESP_LOGI(TAG, "Schedule updated with %d items", n_schedule_items);
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
    current_display_time_remaining = 0;
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

    // Unpin all items
    for (int i = 0; i < MAX_SCHEDULE_ITEMS; i++) {
        schedule_items[i].flags.pinned = false;
    }

    ScheduleItem_t* schedule_item = find_schedule_item(schedule_item_uuid);
    if (schedule_item != NULL) {
        schedule_item->flags.pinned = true;
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
    schedule_mutex = xSemaphoreCreateBinary();
    xSemaphoreGive(schedule_mutex);

    // Create the scheduler task
    xTaskCreate(scheduler_task, "scheduler", 8192, NULL, 5, &xSchedulerTask);

    esp_event_handler_register(DAUGHTERBOARD_EVENTS, DAUGHTERBOARD_EVENT_BUTTON_A_PRESSED, button_event_handler, NULL);
    esp_event_handler_register(DAUGHTERBOARD_EVENTS, DAUGHTERBOARD_EVENT_BUTTON_B_PRESSED, button_event_handler, NULL);
    esp_event_handler_register(DAUGHTERBOARD_EVENTS, DAUGHTERBOARD_EVENT_BUTTON_C_PRESSED, button_event_handler, NULL);
}
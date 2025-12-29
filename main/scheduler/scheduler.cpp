#include "scheduler.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <cstring>
#include <webp/decode.h>

#include "sprites.h"
#include "sockets.h"
#include "display.h"
#include "raii_utils.hpp"

static const char* TAG = "scheduler";

// Forward declaration - implemented in sockets.cpp
extern void send_render_request_to_server(const uint8_t* uuid);

namespace {

// Timing constants
constexpr uint64_t SCHEDULE_REFRESH_MS = 5 * 60 * 1000;  // 5 minutes
constexpr uint64_t CHECK_INTERVAL_US = 100 * 1000;       // 100ms in microseconds
constexpr uint32_t PREPARE_WINDOW_MS = 3000;             // 3 seconds before display ends
constexpr uint32_t PIN_STATUS_DURATION_MS = 1000;        // Pin/unpin overlay duration
constexpr uint32_t TASK_NOTIFY_WAIT_MS = 1000;           // Scheduler task main loop wait

// Size constants
constexpr size_t MIN_WEBP_SIZE = 12;
constexpr size_t MAX_PREPARE_REQUESTS = 5;               // Max items to prepare at once

// Task configuration
constexpr uint32_t SCHEDULER_TASK_STACK_SIZE = 3072;
constexpr UBaseType_t SCHEDULER_TASK_PRIORITY = 5;

// Notification bits for scheduler task
constexpr uint32_t NOTIFY_PREPARE = (1 << 0);
constexpr uint32_t NOTIFY_ADVANCE = (1 << 1);
constexpr uint32_t NOTIFY_RESUME_DISPLAY = (1 << 2);

struct SchedulerState {
    ScheduleItem_t items[MAX_SCHEDULE_ITEMS] = {};
    TaskHandle_t task = nullptr;
    SemaphoreHandle_t mutex = nullptr;
    esp_timer_handle_t check_timer = nullptr;
    esp_timer_handle_t pin_status_timer = nullptr;

    uint32_t current_item = 0;
    size_t item_count = 0;
    bool running = false;
    bool has_valid_schedule = false;

    TickType_t display_start_tick = 0;
    uint64_t last_schedule_request_time = 0;

    // Prepare window state
    bool in_prepare_window = false;
    uint8_t prepared_uuids[MAX_PREPARE_REQUESTS][UUID_SIZE_BYTES] = {};
    size_t prepared_count = 0;

    // Pin status overlay state
    bool showing_pin_status = false;

    bool init() {
        mutex = xSemaphoreCreateMutex();
        if (mutex == nullptr) return false;
        return true;
    }

    // --- UUID helpers ---

    bool has_valid_uuid(const ScheduleItem_t& item) const {
        for (int i = 0; i < 4; i++) {
            if (item.uuid[i] != 0) return true;
        }
        return false;
    }

    ScheduleItem_t* find_item(const uint8_t* uuid) {
        if (uuid == nullptr) return nullptr;
        for (size_t i = 0; i < item_count; i++) {
            if (std::memcmp(items[i].uuid, uuid, UUID_SIZE_BYTES) == 0) {
                return &items[i];
            }
        }
        return nullptr;
    }

    bool is_uuid_prepared(const uint8_t* uuid) const {
        for (size_t i = 0; i < prepared_count; i++) {
            if (std::memcmp(prepared_uuids[i], uuid, UUID_SIZE_BYTES) == 0) {
                return true;
            }
        }
        return false;
    }

    void mark_uuid_prepared(const uint8_t* uuid) {
        if (prepared_count >= MAX_PREPARE_REQUESTS) return;
        std::memcpy(prepared_uuids[prepared_count], uuid, UUID_SIZE_BYTES);
        prepared_count++;
    }

    void clear_prepared() {
        prepared_count = 0;
        in_prepare_window = false;
    }

    // --- Display state helpers ---

    bool is_displayable(const ScheduleItem_t& item) const {
        return has_valid_uuid(item) &&
               !item.flags.skipped_user &&
               !item.flags.skipped_server &&
               item.render_state == RenderState::HasData &&
               item.sprite != nullptr &&
               sprite_get_length(item.sprite) > 0;
    }

    bool is_render_candidate(const ScheduleItem_t& item) const {
        return has_valid_uuid(item) &&
               !item.flags.skipped_user &&
               !item.flags.skipped_server;
    }

    uint64_t get_elapsed_ms() const {
        return pdTICKS_TO_MS(xTaskGetTickCount() - display_start_tick);
    }

    uint32_t get_display_duration_ms() const {
        if (current_item >= item_count) return 0;
        return items[current_item].flags.display_time * 1000;
    }

    bool is_display_expired() const {
        return get_elapsed_ms() >= get_display_duration_ms();
    }

    bool is_in_prepare_window() const {
        uint32_t duration = get_display_duration_ms();
        uint64_t elapsed = get_elapsed_ms();
        if (duration <= PREPARE_WINDOW_MS) return true;  // Always prepare if display time is short
        return elapsed >= (duration - PREPARE_WINDOW_MS);
    }

    // --- Render request helpers ---

    void request_render(ScheduleItem_t& item) {
        if (!has_valid_uuid(item)) return;
        item.render_state = RenderState::RenderPending;
        item.render_request_tick = xTaskGetTickCount();
        send_render_request_to_server(item.uuid);
    }

    // --- Display helpers ---

    void display_current() {
        if (current_item >= item_count) {
            display_clear();
            return;
        }
        auto& item = items[current_item];
        if (!is_displayable(item)) {
            // Only blank the screen if we've received a response from the server
            // (i.e., we previously had data or server explicitly gave us nothing)
            if (item.flags.has_received_response) {
                display_clear();
            }
            // Otherwise keep showing whatever was on screen before
            return;
        }
        show_sprite(item.sprite);
        sockets_send_currently_displaying((uint8_t*)item.uuid);
    }

    void advance_to(uint32_t index) {
        if (index >= item_count) return;
        current_item = index;
        display_start_tick = xTaskGetTickCount();
        clear_prepared();  // Reset prepare state for new item
        display_current();
    }
};

SchedulerState sched;

// --- WebP Validation ---

bool validate_webp(const uint8_t* data, size_t len) {
    if (data == nullptr || len < MIN_WEBP_SIZE) return false;

    // Check RIFF header
    if (std::memcmp(data, "RIFF", 4) != 0) return false;

    // Check WEBP signature at offset 8
    if (std::memcmp(data + 8, "WEBP", 4) != 0) return false;

    // Verify with libwebp
    int width = 0, height = 0;
    if (!WebPGetInfo(data, len, &width, &height)) return false;
    if (width <= 0 || height <= 0 || width > 1024 || height > 1024) return false;

    return true;
}

// --- Core Logic ---

// Check if any item in the schedule is displayable
bool has_any_displayable() {
    for (size_t i = 0; i < sched.item_count; i++) {
        if (sched.is_displayable(sched.items[i])) {
            return true;
        }
    }
    return false;
}

// Find the next displayable item index (wrapping around)
int32_t find_next_displayable(uint32_t from_index) {
    for (size_t i = 1; i <= sched.item_count; i++) {
        uint32_t idx = (from_index + i) % sched.item_count;
        if (sched.is_displayable(sched.items[idx])) {
            return idx;
        }
    }
    return -1;
}

// Handle prepare window: request renders for upcoming items
void handle_prepare() {
    if (sched.item_count == 0) return;

    auto& current = sched.items[sched.current_item];

    // For pinned items or single-item schedules, re-request the current item
    if (current.flags.pinned || sched.item_count == 1) {
        if (!current.flags.skipped_user && !sched.is_uuid_prepared(current.uuid)) {
            ESP_LOGI(TAG, "Prepare: requesting render for current item %u (%02x%02x) (pinned/single)",
                     sched.current_item, current.uuid[0], current.uuid[1]);
            sched.request_render(current);
            sched.mark_uuid_prepared(current.uuid);
        }
        return;
    }

    // Find next items starting from current+1 and request renders
    // Request server-skipped items too (they may have recovered)
    // Continue past server-skipped items until we hit a normal item
    size_t prepared = 0;
    bool found_normal = false;

    for (size_t i = 1; i <= sched.item_count && prepared < MAX_PREPARE_REQUESTS && !found_normal; i++) {
        uint32_t idx = (sched.current_item + i) % sched.item_count;
        auto& item = sched.items[idx];

        // Skip user-skipped items entirely
        if (item.flags.skipped_user) continue;

        // Skip already prepared items (in this prepare window only)
        if (sched.is_uuid_prepared(item.uuid)) {
            // If this was a normal item, stop searching
            if (!item.flags.skipped_server) {
                found_normal = true;
            }
            continue;
        }

        // Request render (even if it already has data - get fresh data)
        ESP_LOGI(TAG, "Prepare: requesting render for item %u (%02x%02x)%s",
                 idx, item.uuid[0], item.uuid[1],
                 item.flags.skipped_server ? " (retry server-skipped)" : "");
        sched.request_render(item);
        sched.mark_uuid_prepared(item.uuid);
        prepared++;

        // If this is a normal (non-server-skipped) item, we're done
        if (!item.flags.skipped_server) {
            found_normal = true;
        }
        // Otherwise continue to find more items (server-skipped items may fail again)
    }
}

// Handle advance: move to next displayable item
void handle_advance() {
    if (sched.item_count == 0) {
        display_clear();
        return;
    }

    auto& current = sched.items[sched.current_item];

    // Pinned: reset timer and request fresh render
    if (current.flags.pinned) {
        sched.display_start_tick = xTaskGetTickCount();
        sched.clear_prepared();

        // If pinned item is displayable, keep showing it
        // Otherwise request fresh render (it may recover)
        if (sched.is_displayable(current)) {
            sched.display_current();
        } else {
            current.render_state = RenderState::NeedsRender;
            // Only blank if we've previously received data from server
            if (current.flags.has_received_response) {
                display_clear();
            }
        }
        return;
    }

    // Find and advance to next displayable
    int32_t next = find_next_displayable(sched.current_item);
    if (next >= 0) {
        sched.advance_to(next);
    } else {
        display_clear();
    }
}

// Pin status timer callback - fires after showing pinned/unpinned sprite
void pin_status_timer_callback(void*) {
    if (sched.task != nullptr) {
        xTaskNotify(sched.task, NOTIFY_RESUME_DISPLAY, eSetBits);
    }
}

// Show pin status sprite for 1 second, then resume normal display
void show_pin_status(bool pinned) {
    show_fs_sprite(pinned ? "pinned" : "unpinned");
    sched.showing_pin_status = true;

    // Start one-shot timer to resume display
    if (sched.pin_status_timer != nullptr) {
        esp_timer_stop(sched.pin_status_timer);
        esp_timer_start_once(sched.pin_status_timer, PIN_STATUS_DURATION_MS * 1000);
    }
}

// Timer callback - runs every 100ms to check timing
void IRAM_ATTR check_timer_callback(void*) {
    if (!sched.running || !sched.has_valid_schedule || sched.item_count == 0) {
        return;
    }

    if (sched.task == nullptr) return;

    // Don't advance while showing pin status overlay
    if (sched.showing_pin_status) return;

    // Check if display time expired -> notify advance
    if (sched.is_display_expired()) {
        xTaskNotifyFromISR(sched.task, NOTIFY_ADVANCE, eSetBits, nullptr);
        return;
    }

    // Check if in prepare window -> notify prepare
    if (!sched.in_prepare_window && sched.is_in_prepare_window()) {
        sched.in_prepare_window = true;
        xTaskNotifyFromISR(sched.task, NOTIFY_PREPARE, eSetBits, nullptr);
    }
}

void scheduler_task_func(void*) {
    ESP_LOGI(TAG, "Scheduler task started");

    while (true) {
        // Wait for notifications with timeout for schedule refresh
        uint32_t notification = 0;
        BaseType_t got_notify = xTaskNotifyWait(0, UINT32_MAX, &notification, pdMS_TO_TICKS(TASK_NOTIFY_WAIT_MS));

        if (!sched.running) continue;

        raii::MutexGuard lock(sched.mutex);
        if (!lock) continue;

        // Request schedule periodically
        uint64_t now_ms = pdTICKS_TO_MS(xTaskGetTickCount());
        if (!sched.has_valid_schedule ||
            (now_ms - sched.last_schedule_request_time > SCHEDULE_REFRESH_MS)) {
            request_schedule();
            sched.last_schedule_request_time = now_ms;
            if (!sched.has_valid_schedule) continue;
        }

        // Handle notifications
        if (got_notify == pdTRUE) {
            if (notification & NOTIFY_RESUME_DISPLAY) {
                sched.showing_pin_status = false;
                sched.display_current();
            }
            if (notification & NOTIFY_PREPARE) {
                handle_prepare();
            }
            if (notification & NOTIFY_ADVANCE) {
                handle_advance();
            }
        }

        // If current not displayable and not pinned, try to find next
        if (sched.current_item < sched.item_count) {
            auto& current = sched.items[sched.current_item];
            if (!sched.is_displayable(current) && !current.flags.pinned) {
                int32_t next = find_next_displayable(sched.current_item);
                if (next >= 0) {
                    sched.advance_to(next);
                } else if (current.flags.has_received_response) {
                    // No displayable items and we've received a response - blank the screen
                    display_clear();
                }
            }
        }

        // Also check if we have no displayable items at all (e.g., single pinned item that's server-skipped)
        // Only blank if at least one item has received a server response
        if (!has_any_displayable()) {
            auto& current = sched.items[sched.current_item];
            if (current.flags.has_received_response) {
                display_clear();
            }
        }
    }
}

}  // namespace

// --- Public API ---

void scheduler_init() {
    if (!sched.init()) {
        ESP_LOGE(TAG, "Failed to create mutex");
        return;
    }

    // Create scheduler task
    BaseType_t task_ret = xTaskCreate(scheduler_task_func, "scheduler",
        SCHEDULER_TASK_STACK_SIZE, nullptr, SCHEDULER_TASK_PRIORITY, &sched.task);
    if (task_ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create scheduler task");
        return;
    }

    // Create check timer (100ms periodic)
    esp_timer_create_args_t timer_args = {
        .callback = check_timer_callback,
        .arg = nullptr,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "sched_check",
        .skip_unhandled_events = true,
    };

    esp_err_t ret = esp_timer_create(&timer_args, &sched.check_timer);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create check timer: %s", esp_err_to_name(ret));
        return;
    }

    ret = esp_timer_start_periodic(sched.check_timer, CHECK_INTERVAL_US);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start check timer: %s", esp_err_to_name(ret));
        return;
    }

    // Create pin status timer (one-shot, started when needed)
    esp_timer_create_args_t pin_timer_args = {
        .callback = pin_status_timer_callback,
        .arg = nullptr,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "pin_status",
        .skip_unhandled_events = true,
    };

    ret = esp_timer_create(&pin_timer_args, &sched.pin_status_timer);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create pin status timer: %s", esp_err_to_name(ret));
    }

    ESP_LOGI(TAG, "Scheduler initialized");
}

bool scheduler_has_schedule() {
    return sched.has_valid_schedule && sched.item_count > 0;
}

void scheduler_set_schedule(Kd__V1__MatrxSchedule* schedule_response) {
    if (schedule_response == nullptr || schedule_response->n_schedule_items == 0) {
        scheduler_clear();
        return;
    }

    raii::MutexGuard lock(sched.mutex);
    if (!lock) return;

    // Process items
    size_t new_count = 0;
    for (size_t i = 0; i < schedule_response->n_schedule_items && i < MAX_SCHEDULE_ITEMS; i++) {
        Kd__V1__ScheduleItem* pb_item = schedule_response->schedule_items[i];
        if (pb_item == nullptr || pb_item->uuid.len != UUID_SIZE_BYTES || pb_item->uuid.data == nullptr) {
            continue;
        }

        ScheduleItem_t& slot = sched.items[new_count];
        ScheduleItem_t* existing = sched.find_item(pb_item->uuid.data);

        std::memcpy(slot.uuid, pb_item->uuid.data, UUID_SIZE_BYTES);
        slot.flags.pinned = pb_item->user_pinned;
        slot.flags.skipped_user = pb_item->user_skipped;
        slot.flags.skipped_server = false;
        slot.flags.display_time = pb_item->display_time;

        if (existing != nullptr && existing != &slot && existing->sprite != nullptr) {
            // Reuse existing sprite from different slot
            slot.sprite = existing->sprite;
            slot.render_state = existing->render_state;
            slot.render_request_tick = existing->render_request_tick;
            existing->sprite = nullptr;
        } else if (existing == &slot && slot.sprite != nullptr) {
            // Same slot, keep existing
        } else {
            // Allocate new sprite
            if (slot.sprite != nullptr) sprite_free(slot.sprite);
            slot.sprite = sprite_allocate();
            slot.render_state = RenderState::NeedsRender;
            slot.render_request_tick = 0;
        }

        new_count++;
    }

    // Free unused sprites from old items
    for (size_t i = new_count; i < sched.item_count; i++) {
        if (sched.items[i].sprite != nullptr) {
            sprite_free(sched.items[i].sprite);
            sched.items[i].sprite = nullptr;
        }
        std::memset(&sched.items[i], 0, sizeof(ScheduleItem_t));
    }

    sched.item_count = new_count;
    sched.has_valid_schedule = true;
    sched.running = true;

    // Check if any item is pinned - if so, start on that item
    sched.current_item = 0;
    bool found_pinned = false;
    for (size_t i = 0; i < sched.item_count; i++) {
        if (sched.items[i].flags.pinned && !sched.items[i].flags.skipped_user) {
            sched.current_item = i;
            found_pinned = true;
            break;
        }
    }

    // If no pinned item, find first non-user-skipped item
    if (!found_pinned) {
        for (size_t i = 0; i < sched.item_count; i++) {
            if (!sched.items[i].flags.skipped_user) {
                sched.current_item = i;
                break;
            }
        }
    }

    sched.display_start_tick = xTaskGetTickCount();
    sched.clear_prepared();

    // Request render for the current item immediately
    auto& current = sched.items[sched.current_item];
    if (!current.flags.skipped_user && current.render_state == RenderState::NeedsRender) {
        ESP_LOGI(TAG, "Initial render request for current item %u (%02x%02x)",
                 sched.current_item, current.uuid[0], current.uuid[1]);
        sched.request_render(current);
    }

    // Display current if ready
    if (sched.is_displayable(current)) {
        sched.display_current();
    }

    ESP_LOGI(TAG, "Schedule set: %zu items, current=%u", sched.item_count, sched.current_item);
}

void scheduler_clear() {
    raii::MutexGuard lock(sched.mutex);
    if (!lock) return;

    for (size_t i = 0; i < sched.item_count; i++) {
        if (sched.items[i].sprite != nullptr) {
            sprite_free(sched.items[i].sprite);
        }
        std::memset(&sched.items[i], 0, sizeof(ScheduleItem_t));
    }

    sched.item_count = 0;
    sched.current_item = 0;
    sched.has_valid_schedule = false;
    sched.running = false;
    sched.clear_prepared();
    display_clear();
}

void scheduler_start() {
    raii::MutexGuard lock(sched.mutex);
    if (lock) sched.running = true;
}

void scheduler_stop() {
    raii::MutexGuard lock(sched.mutex);
    if (lock) sched.running = false;
}

void scheduler_handle_render_response(const uint8_t* uuid, const uint8_t* data, size_t len, bool server_error) {
    if (uuid == nullptr) return;

    ESP_LOGI(TAG, "Render response: %02x%02x, len=%zu, error=%d", uuid[0], uuid[1], len, server_error);

    raii::MutexGuard lock(sched.mutex);
    if (!lock) return;

    ScheduleItem_t* item = sched.find_item(uuid);
    if (item == nullptr) {
        ESP_LOGW(TAG, "Item not found in schedule");
        return;
    }

    bool is_current = (&sched.items[sched.current_item] == item);

    // Mark that server has responded (enables blanking logic for this item)
    item->flags.has_received_response = true;

    // Server error or empty data
    if (server_error || data == nullptr || len == 0) {
        ESP_LOGW(TAG, "Server error or empty data");
        item->flags.skipped_server = true;
        item->render_state = RenderState::RenderPending;
        sprite_update_data(item->sprite, nullptr, 0);
        return;
    }

    // Validate WebP
    if (!validate_webp(data, len)) {
        ESP_LOGW(TAG, "Invalid WebP data");
        item->flags.skipped_server = true;
        item->render_state = RenderState::NeedsRender;
        sprite_update_data(item->sprite, nullptr, 0);
        return;
    }

    // Success
    ESP_LOGI(TAG, "Render success, is_current=%d", is_current);
    item->flags.skipped_server = false;
    item->render_state = RenderState::HasData;
    sprite_update_data(item->sprite, data, len);

    if (is_current) {
        sched.display_current();
    }
}

void scheduler_handle_button_next() {
    raii::MutexGuard lock(sched.mutex);
    if (!lock || !sched.running || sched.item_count == 0) return;

    auto& current = sched.items[sched.current_item];

    // Unpin if pinned
    if (current.flags.pinned) {
        current.flags.pinned = false;
        sockets_send_modify_schedule_item(current.uuid, false, current.flags.skipped_user);
        show_pin_status(false);
    }

    // Find next displayable item
    for (size_t i = 1; i < sched.item_count; i++) {
        uint32_t idx = (sched.current_item + i) % sched.item_count;
        if (sched.is_displayable(sched.items[idx])) {
            sched.advance_to(idx);
            return;
        }
    }
}

void scheduler_handle_button_prev() {
    raii::MutexGuard lock(sched.mutex);
    if (!lock || !sched.running || sched.item_count == 0) return;

    auto& current = sched.items[sched.current_item];

    // Unpin if pinned
    if (current.flags.pinned) {
        current.flags.pinned = false;
        sockets_send_modify_schedule_item(current.uuid, false, current.flags.skipped_user);
        show_pin_status(false);
    }

    // Find previous displayable item (search backwards)
    for (size_t i = 1; i < sched.item_count; i++) {
        uint32_t idx = (sched.current_item + sched.item_count - i) % sched.item_count;
        if (sched.is_displayable(sched.items[idx])) {
            sched.advance_to(idx);
            return;
        }
    }
}

void scheduler_handle_button_pin() {
    raii::MutexGuard lock(sched.mutex);
    if (!lock || !sched.running || sched.item_count == 0) return;

    auto& current = sched.items[sched.current_item];
    if (current.flags.pinned) return;  // Already pinned

    current.flags.pinned = true;
    sched.display_start_tick = xTaskGetTickCount();
    sockets_send_modify_schedule_item(current.uuid, true, current.flags.skipped_user);
    show_pin_status(true);
}

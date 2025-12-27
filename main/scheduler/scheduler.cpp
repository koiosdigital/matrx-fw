#include "scheduler.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <esp_log.h>

#include <cstring>

#include "sprites.h"
#include "sockets.h"
#include "daughterboard.h"
#include "display.h"
#include "raii_utils.hpp"
#include "../render_requests/render_requests.h"

static const char* TAG = "scheduler";

namespace {

    // Task notification types
    enum class ScheduleNotification : uint32_t {
        SkipCurrent,
        PinCurrent,
        TogglePin,      // Button B pressed - toggle pin state
        NextItem,
        PreviousItem
    };

    // Pin feedback duration
    constexpr TickType_t PIN_FEEDBACK_DURATION_TICKS = pdMS_TO_TICKS(500);  // 0.5 seconds

    // Encapsulated scheduler state
    struct SchedulerState {
        ScheduleItem_t items[MAX_SCHEDULE_ITEMS] = {};
        TaskHandle_t task = nullptr;
        SemaphoreHandle_t mutex = nullptr;

        // Current state
        uint32_t current_item = 0;
        bool running = false;
        bool has_valid_schedule = false;

        // Timing state
        TickType_t sprite_start_tick = 0;
        uint64_t last_schedule_request_time = 0;
        bool need_to_skip = false;

        // Pin feedback state
        bool showing_pin_feedback = false;
        bool pin_feedback_is_pinning = false;  // true = showing "pinned", false = showing "unpinned"
        TickType_t pin_feedback_start_tick = 0;

        bool init() {
            mutex = xSemaphoreCreateBinary();
            if (mutex == nullptr) return false;
            xSemaphoreGive(mutex);
            return true;
        }

        bool item_has_valid_uuid(const ScheduleItem_t& item) const {
            return item.uuid[0] != 0 || item.uuid[1] != 0 ||
                item.uuid[2] != 0 || item.uuid[3] != 0;
        }

        ScheduleItem_t* find_item(const uint8_t* uuid) {
            for (auto& item : items) {
                if (std::memcmp(item.uuid, uuid, UUID_SIZE_BYTES) == 0) {
                    return &item;
                }
            }
            return nullptr;
        }

        size_t count_valid_items() const {
            size_t count = 0;
            for (const auto& item : items) {
                if (item_has_valid_uuid(item)) count++;
            }
            return count;
        }

        bool is_displayable(const ScheduleItem_t& item) const {
            return item_has_valid_uuid(item) &&
                !item.flags.skipped_user &&
                !item.flags.skipped_server;
        }

        // Item is not user-skipped (server-skipped items are still candidates for render request)
        bool is_render_candidate(const ScheduleItem_t& item) const {
            return item_has_valid_uuid(item) && !item.flags.skipped_user;
        }

        bool is_server_skipped(const ScheduleItem_t& item) const {
            return item_has_valid_uuid(item) && item.flags.skipped_server;
        }

        bool has_displayable_items() const {
            for (const auto& item : items) {
                if (is_displayable(item)) return true;
            }
            return false;
        }

        uint64_t get_elapsed_ms() const {
            return pdTICKS_TO_MS(xTaskGetTickCount() - sprite_start_tick);
        }

        uint32_t get_display_duration_ms() const {
            return items[current_item].flags.display_time * 1000;
        }

        bool is_display_expired() const {
            return get_elapsed_ms() >= get_display_duration_ms();
        }

        bool in_prepare_window() const {
            uint64_t elapsed = get_elapsed_ms();
            uint32_t duration = get_display_duration_ms();
            return elapsed >= (duration - PREPARE_TIME) && elapsed < duration;
        }

        uint32_t find_next_displayable(uint32_t start_index) const {
            size_t valid_count = count_valid_items();
            if (valid_count == 0) return start_index;

            for (size_t i = 1; i <= valid_count; i++) {
                uint32_t idx = (start_index + i) % valid_count;
                if (is_displayable(items[idx])) return idx;
            }
            return start_index;
        }

        uint32_t find_previous_displayable(uint32_t start_index) const {
            size_t valid_count = count_valid_items();
            if (valid_count == 0) return start_index;

            for (size_t i = 1; i <= valid_count; i++) {
                uint32_t idx = (start_index + valid_count - i) % valid_count;
                if (is_displayable(items[idx])) return idx;
            }
            return start_index;
        }

        bool has_sprite_data(const ScheduleItem_t& item) const {
            return item.sprite != nullptr && sprite_get_length(item.sprite) > 0;
        }

        void display_current() {
            const auto& item = items[current_item];
            // Don't display if item is skipped or has no data
            if (!is_displayable(item) || !has_sprite_data(item)) {
                display_clear();
                return;
            }
            show_sprite(item.sprite);
            sockets_send_currently_displaying((uint8_t*)item.uuid);
        }

        void advance_to(uint32_t index) {
            ESP_LOGD(TAG, "Advancing %lu -> %lu", current_item, index);
            // Mark the item we're leaving as needing re-render next cycle
            if (item_has_valid_uuid(items[current_item])) {
                render_mark_needs_render(items[current_item].uuid);
            }
            current_item = index;
            sprite_start_tick = xTaskGetTickCount();
            display_current();
        }
    };

    SchedulerState sched;

    // Forward declarations
    void request_renders_for_upcoming();
    void handle_notification(ScheduleNotification notification);

    void button_event_handler(void*, esp_event_base_t event_base, int32_t event_id, void*) {
        if (event_base != DAUGHTERBOARD_EVENTS || sched.task == nullptr) return;

        ScheduleNotification notif;
        if (event_id == DAUGHTERBOARD_EVENT_BUTTON_A_PRESSED) {
            ESP_LOGD(TAG, "Button A pressed");
            notif = ScheduleNotification::PreviousItem;
        }
        else if (event_id == DAUGHTERBOARD_EVENT_BUTTON_B_PRESSED) {
            ESP_LOGD(TAG, "Button B pressed (TogglePin)");
            notif = ScheduleNotification::TogglePin;
        }
        else if (event_id == DAUGHTERBOARD_EVENT_BUTTON_C_PRESSED) {
            ESP_LOGD(TAG, "Button C pressed");
            notif = ScheduleNotification::NextItem;
        }
        else {
            return;
        }
        xTaskNotify(sched.task, static_cast<uint32_t>(notif), eSetValueWithOverwrite);
    }

    void log_item_state(const char* prefix, uint32_t idx, const ScheduleItem_t& item) {
        ESP_LOGD(TAG, "%s [%lu]: uuid=%02x%02x, skipped_user=%d, skipped_server=%d, pinned=%d, has_data=%d",
            prefix, idx,
            item.uuid[0], item.uuid[1],
            item.flags.skipped_user, item.flags.skipped_server, item.flags.pinned,
            sched.has_sprite_data(item) ? 1 : 0);
    }

    void request_renders_for_upcoming() {
        if (!sched.has_valid_schedule) return;
        if (!sched.in_prepare_window()) return;

        const auto& current = sched.items[sched.current_item];
        if (!sched.item_has_valid_uuid(current)) return;

        ESP_LOGD(TAG, "request_renders_for_upcoming: in prepare window, current=%lu", sched.current_item);

        // Pinned items re-render themselves
        if (current.flags.pinned) {
            ESP_LOGD(TAG, "request_renders: current is pinned, requesting");
            render_request(current.uuid);
            return;
        }

        // Request renders for all server-skipped items up to and including
        // the next normally displayable item
        size_t valid_count = sched.count_valid_items();
        if (valid_count == 0) return;

        ESP_LOGD(TAG, "request_renders: checking %zu items", valid_count);
        for (size_t i = 1; i <= valid_count; i++) {
            uint32_t idx = (sched.current_item + i) % valid_count;
            const auto& item = sched.items[idx];

            // Skip user-skipped items entirely
            if (item.flags.skipped_user) {
                ESP_LOGD(TAG, "request_renders: item %lu is user-skipped, continuing", idx);
                continue;
            }

            // Request render for this item
            if (sched.item_has_valid_uuid(item)) {
                ESP_LOGD(TAG, "request_renders: requesting item %lu (server_skipped=%d)", idx, item.flags.skipped_server);
                render_request(item.uuid);
            }

            // Stop after finding a normally displayable item (not server-skipped)
            if (!item.flags.skipped_server) {
                ESP_LOGD(TAG, "request_renders: found non-server-skipped item %lu, stopping", idx);
                break;
            }
        }
    }

    void start_pin_feedback(bool is_pinning) {
        ESP_LOGD(TAG, "start_pin_feedback: is_pinning=%d", is_pinning ? 1 : 0);
        sched.showing_pin_feedback = true;
        sched.pin_feedback_is_pinning = is_pinning;
        sched.pin_feedback_start_tick = xTaskGetTickCount();
        show_fs_sprite(is_pinning ? "pinned" : "unpinned");
    }

    void unpin_current_item() {
        auto& current = sched.items[sched.current_item];
        current.flags.pinned = false;
        start_pin_feedback(false);  // Show "unpinned"

        // Notify server of unpin
        if (sched.item_has_valid_uuid(current)) {
            sockets_send_modify_schedule_item(current.uuid, false, current.flags.skipped_user);
        }
    }

    void handle_notification(ScheduleNotification notification) {
        ESP_LOGD(TAG, "handle_notification: notif=%d, has_schedule=%d",
            static_cast<int>(notification), sched.has_valid_schedule ? 1 : 0);

        if (!sched.has_valid_schedule) {
            ESP_LOGD(TAG, "handle_notification: no valid schedule, ignoring");
            return;
        }

        auto& current = sched.items[sched.current_item];
        bool is_pinned = current.flags.pinned;
        ESP_LOGD(TAG, "handle_notification: current=%lu, is_pinned=%d", sched.current_item, is_pinned ? 1 : 0);

        // If item is pinned and any button is pressed, unpin and return to rotation
        if (is_pinned && notification != ScheduleNotification::SkipCurrent) {
            ESP_LOGD(TAG, "handle_notification: item is pinned, unpinning");
            unpin_current_item();
            // After feedback, will return to rotation via process_display_timing
            return;
        }

        switch (notification) {
        case ScheduleNotification::SkipCurrent:
            ESP_LOGD(TAG, "handle_notification: SkipCurrent");
            sched.need_to_skip = true;
            break;

        case ScheduleNotification::NextItem:
            ESP_LOGD(TAG, "handle_notification: NextItem");
            // Reset timer and advance
            sched.sprite_start_tick = xTaskGetTickCount();
            sched.need_to_skip = true;
            break;

        case ScheduleNotification::TogglePin:
            ESP_LOGD(TAG, "handle_notification: TogglePin");
            // Pin current item (already handled unpin case above)
            if (sched.item_has_valid_uuid(current)) {
                ESP_LOGD(TAG, "handle_notification: pinning current item");
                for (auto& item : sched.items) {
                    item.flags.pinned = false;
                }
                current.flags.pinned = true;
                start_pin_feedback(true);  // Show "pinned"

                // Notify server of pin
                sockets_send_modify_schedule_item(current.uuid, true, current.flags.skipped_user);
            } else {
                ESP_LOGD(TAG, "handle_notification: current item has no valid uuid");
            }
            break;

        case ScheduleNotification::PinCurrent:
            // Legacy API - just pin without feedback
            if (sched.item_has_valid_uuid(current)) {
                for (auto& item : sched.items) {
                    item.flags.pinned = false;
                }
                current.flags.pinned = true;

                // Notify server of pin
                sockets_send_modify_schedule_item(current.uuid, true, current.flags.skipped_user);
            }
            break;

        case ScheduleNotification::PreviousItem:
            // Reset timer and go to previous
            sched.sprite_start_tick = xTaskGetTickCount();
            {
                uint32_t prev = sched.find_previous_displayable(sched.current_item);
                if (prev != sched.current_item) {
                    sched.advance_to(prev);
                }
            }
            break;
        }
    }

    void process_pin_feedback() {
        if (!sched.showing_pin_feedback) return;

        TickType_t elapsed = xTaskGetTickCount() - sched.pin_feedback_start_tick;
        if (elapsed >= PIN_FEEDBACK_DURATION_TICKS) {
            sched.showing_pin_feedback = false;
            // Return to displaying current item (or clear if server-skipped in single-item mode)
            sched.sprite_start_tick = xTaskGetTickCount();
            sched.display_current();
        }
    }

    void advance_to_next_with_data() {
        // Pattern: request render, advance, request render, advance... until we find data
        size_t valid_count = sched.count_valid_items();
        ESP_LOGD(TAG, "advance_to_next_with_data: valid_count=%zu, current=%lu", valid_count, sched.current_item);

        if (valid_count == 0) {
            ESP_LOGD(TAG, "advance_to_next: no valid items, clearing");
            display_clear();
            return;
        }

        uint32_t start_item = sched.current_item;

        for (size_t attempts = 0; attempts < valid_count; attempts++) {
            // Find next item that's not user-skipped
            uint32_t next = sched.current_item;
            for (size_t i = 1; i <= valid_count; i++) {
                uint32_t idx = (sched.current_item + i) % valid_count;
                if (sched.is_render_candidate(sched.items[idx])) {
                    next = idx;
                    break;
                }
            }

            ESP_LOGD(TAG, "advance_to_next: attempt %zu, next=%lu, start=%lu", attempts, next, start_item);

            // If we wrapped around to start, no valid items
            if (next == start_item && attempts > 0) {
                ESP_LOGD(TAG, "advance_to_next: wrapped around, no displayable items with data");
                display_clear();
                return;
            }

            auto& item = sched.items[next];
            log_item_state("advance_to_next checking", next, item);

            // Request render for this item (on transition)
            if (sched.item_has_valid_uuid(item)) {
                ESP_LOGD(TAG, "advance_to_next: requesting render for item %lu", next);
                render_request(item.uuid);
            }

            // If it has sprite data, display it
            if (sched.has_sprite_data(item) && !item.flags.skipped_server) {
                ESP_LOGD(TAG, "advance_to_next: item %lu has data and not server-skipped, displaying", next);
                sched.advance_to(next);
                return;
            }

            // If server-skipped but now has data, clear the flag and display
            if (sched.has_sprite_data(item) && item.flags.skipped_server) {
                ESP_LOGD(TAG, "advance_to_next: item %lu has data, clearing server-skipped flag", next);
                item.flags.skipped_server = false;
                sched.advance_to(next);
                return;
            }

            // No data yet - advance and try next item
            ESP_LOGD(TAG, "advance_to_next: item %lu has no data (skipped_server=%d), trying next", next, item.flags.skipped_server);
            sched.current_item = next;
        }

        // Couldn't find any item with data after full cycle
        ESP_LOGD(TAG, "advance_to_next: no items with sprite data after full cycle");
        sched.sprite_start_tick = xTaskGetTickCount();
    }

    void process_display_timing() {
        auto& current = sched.items[sched.current_item];
        if (!sched.item_has_valid_uuid(current)) return;

        // If current item became non-displayable (server-skipped or data cleared), advance immediately
        if (!sched.is_displayable(current) || !sched.has_sprite_data(current)) {
            ESP_LOGD(TAG, "process_display_timing: current item %lu became non-displayable (displayable=%d, has_data=%d)",
                sched.current_item, sched.is_displayable(current) ? 1 : 0, sched.has_sprite_data(current) ? 1 : 0);
            log_item_state("process_display_timing current", sched.current_item, current);
            display_clear();
            advance_to_next_with_data();
            return;
        }

        bool should_advance = sched.is_display_expired() || sched.need_to_skip;
        if (!should_advance) return;

        ESP_LOGD(TAG, "process_display_timing: should_advance (expired=%d, need_skip=%d)",
            sched.is_display_expired() ? 1 : 0, sched.need_to_skip ? 1 : 0);

        // Pinned items just reset their timer (unless explicitly skipped)
        if (current.flags.pinned && !sched.need_to_skip) {
            ESP_LOGD(TAG, "process_display_timing: pinned item, resetting timer");
            sched.sprite_start_tick = xTaskGetTickCount();
            sched.need_to_skip = false;
            return;
        }

        sched.need_to_skip = false;
        advance_to_next_with_data();
    }

    void scheduler_task_func(void*) {
        ESP_LOGD(TAG, "Task started");
        constexpr uint64_t SCHEDULE_REFRESH_MS = 5 * 60 * 1000;  // 5 minutes
        constexpr TickType_t TICK_INTERVAL = pdMS_TO_TICKS(100);  // Check every 100ms for responsiveness

        while (true) {
            // Wait for notification or timeout - allows immediate response to button presses
            uint32_t notification_value = 0;
            bool got_notification = xTaskNotifyWait(0, ULONG_MAX, &notification_value, TICK_INTERVAL) == pdTRUE;

            if (!sched.running) continue;

            raii::MutexGuard lock(sched.mutex);
            if (!lock) continue;

            // Handle notifications immediately
            if (got_notification) {
                handle_notification(static_cast<ScheduleNotification>(notification_value));
            }

            // Process pin feedback timeout (showing "pinned"/"unpinned" sprite)
            process_pin_feedback();

            // Skip normal processing while showing pin feedback
            if (sched.showing_pin_feedback) continue;

            // Request schedule periodically
            uint64_t current_time = pdTICKS_TO_MS(xTaskGetTickCount());
            if (!sched.has_valid_schedule ||
                (current_time - sched.last_schedule_request_time > SCHEDULE_REFRESH_MS)) {
                request_schedule();
                sched.last_schedule_request_time = current_time;
                if (!sched.has_valid_schedule) continue;
            }

            request_renders_for_upcoming();

            if (!sched.has_valid_schedule || sched.current_item >= MAX_SCHEDULE_ITEMS) continue;

            process_display_timing();

            if (sched.has_valid_schedule && !sched.has_displayable_items()) {
                display_clear();
            }
        }
    }

}  // namespace

//MARK: Public API

bool schedule_item_valid_uuid(ScheduleItem_t* item) {
    return sched.item_has_valid_uuid(*item);
}

ScheduleItem_t* find_schedule_item(uint8_t* uuid) {
    return sched.find_item(uuid);
}

size_t get_schedule_size() {
    return sched.count_valid_items();
}

bool scheduler_has_schedule() {
    return sched.count_valid_items() > 0;
}

void scheduler_set_schedule(Kd__V1__MatrxSchedule* schedule_response) {
    if (schedule_response == nullptr || schedule_response->n_schedule_items == 0) {
        scheduler_clear();
        return;
    }

    raii::MutexGuard lock(sched.mutex);
    if (!lock) return;

    // Clear old render tracking so failed items can be retried
    render_clear_all();

    ESP_LOGD(TAG, "Processing %zu schedule items", schedule_response->n_schedule_items);

    for (size_t i = 0; i < schedule_response->n_schedule_items; i++) {
        Kd__V1__ScheduleItem* item = schedule_response->schedule_items[i];
        if (item == nullptr || item->uuid.len != UUID_SIZE_BYTES || item->uuid.data == nullptr) {
            continue;
        }

        ScheduleItem_t* matched = sched.find_item(item->uuid.data);
        ScheduleItem_t& slot = sched.items[i];

        std::memcpy(slot.uuid, item->uuid.data, UUID_SIZE_BYTES);
        slot.flags.pinned = item->pinned;
        slot.flags.skipped_server = false;
        slot.flags.skipped_user = item->skipped;
        slot.flags.display_time = item->display_time;

        if (matched != nullptr) {
            slot.sprite = matched->sprite;
        }
        else {
            if (slot.sprite != nullptr) sprite_free(slot.sprite);
            slot.sprite = sprite_allocate();
        }
    }

    // Clear unused slots
    for (size_t i = schedule_response->n_schedule_items; i < MAX_SCHEDULE_ITEMS; i++) {
        if (sched.items[i].sprite != nullptr) sprite_free(sched.items[i].sprite);
        std::memset(&sched.items[i], 0, sizeof(ScheduleItem_t));
    }

    sched.has_valid_schedule = true;
    sched.running = true;

    // Find first displayable item
    sched.current_item = 0;
    while (sched.current_item < MAX_SCHEDULE_ITEMS &&
        !sched.is_displayable(sched.items[sched.current_item])) {
        sched.current_item++;
    }
    if (sched.current_item >= MAX_SCHEDULE_ITEMS) {
        sched.current_item = 0;
    }

    sched.sprite_start_tick = xTaskGetTickCount();

    // Request renders for first few items
    for (int j = 0; j < 3; j++) {
        uint32_t idx = (sched.current_item + j) % MAX_SCHEDULE_ITEMS;
        auto& item = sched.items[idx];
        if (sched.item_has_valid_uuid(item) && !item.flags.skipped_user) {
            render_set_state(item.uuid, RenderState::NeedsRender);
            render_request(item.uuid);
        }
    }

    if (sched.has_displayable_items()) {
        sched.display_current();
    }
    else {
        display_clear();
    }

    ESP_LOGI(TAG, "Schedule set: %zu items, start=%lu",
        schedule_response->n_schedule_items, sched.current_item);
}

void scheduler_clear() {
    ESP_LOGD(TAG, "Clearing schedule");
    raii::MutexGuard lock(sched.mutex);
    if (!lock) return;

    for (auto& item : sched.items) {
        if (item.sprite != nullptr) sprite_free(item.sprite);
        std::memset(&item, 0, sizeof(ScheduleItem_t));
    }

    sched.has_valid_schedule = false;
    sched.running = false;
    sched.current_item = 0;

    render_clear_all();
    display_clear();
}

void scheduler_skip_schedule_item(uint8_t* uuid) {
    raii::MutexGuard lock(sched.mutex);
    if (!lock) return;

    ScheduleItem_t* item = sched.find_item(uuid);
    if (item != nullptr) {
        item->flags.skipped_user = true;
        if (!sched.has_displayable_items()) display_clear();

        // Notify server of skip
        sockets_send_modify_schedule_item(uuid, item->flags.pinned, true);
    }
}

void scheduler_pin_schedule_item(uint8_t* uuid) {
    raii::MutexGuard lock(sched.mutex);
    if (!lock) return;

    for (auto& item : sched.items) {
        item.flags.pinned = false;
    }

    ScheduleItem_t* target = sched.find_item(uuid);
    if (target != nullptr) {
        target->flags.pinned = true;
        for (size_t i = 0; i < MAX_SCHEDULE_ITEMS; i++) {
            if (&sched.items[i] == target) {
                sched.advance_to(i);
                break;
            }
        }

        // Notify server of pin
        sockets_send_modify_schedule_item(uuid, true, target->flags.skipped_user);
    }
}

void scheduler_pin_current_schedule_item() {
    if (sched.task == nullptr) return;
    xTaskNotify(sched.task, static_cast<uint32_t>(ScheduleNotification::PinCurrent), eSetValueWithOverwrite);
}

void scheduler_skip_current_schedule_item() {
    if (sched.task == nullptr) return;
    xTaskNotify(sched.task, static_cast<uint32_t>(ScheduleNotification::SkipCurrent), eSetValueWithOverwrite);
}

void scheduler_goto_next_item() {
    if (sched.task == nullptr) return;
    xTaskNotify(sched.task, static_cast<uint32_t>(ScheduleNotification::NextItem), eSetValueWithOverwrite);
}

void scheduler_goto_previous_item() {
    if (sched.task == nullptr) return;
    xTaskNotify(sched.task, static_cast<uint32_t>(ScheduleNotification::PreviousItem), eSetValueWithOverwrite);
}

void scheduler_start() {
    raii::MutexGuard lock(sched.mutex);
    if (lock) sched.running = true;
}

void scheduler_stop() {
    raii::MutexGuard lock(sched.mutex);
    if (lock) sched.running = false;
}

void scheduler_init() {
    sched.sprite_start_tick = 0;
    sched.last_schedule_request_time = 0;
    sched.need_to_skip = false;
    sched.showing_pin_feedback = false;
    sched.pin_feedback_start_tick = 0;

    if (!sched.init()) {
        ESP_LOGE(TAG, "scheduler_init: failed to create mutex");
        return;
    }

    render_requests_init();
    xTaskCreate(scheduler_task_func, "scheduler", 3072, nullptr, 5, &sched.task);

    esp_event_handler_register(DAUGHTERBOARD_EVENTS, DAUGHTERBOARD_EVENT_BUTTON_A_PRESSED,
        button_event_handler, nullptr);
    esp_event_handler_register(DAUGHTERBOARD_EVENTS, DAUGHTERBOARD_EVENT_BUTTON_B_PRESSED,
        button_event_handler, nullptr);
    esp_event_handler_register(DAUGHTERBOARD_EVENTS, DAUGHTERBOARD_EVENT_BUTTON_C_PRESSED,
        button_event_handler, nullptr);

    ESP_LOGI(TAG, "Scheduler initialized");
}

void scheduler_notify_render_complete(const uint8_t* uuid) {
    if (uuid == nullptr) return;

    ESP_LOGD(TAG, "scheduler_notify_render_complete called for uuid=%02x%02x...", uuid[0], uuid[1]);

    raii::MutexGuard lock(sched.mutex);
    if (!lock) {
        ESP_LOGD(TAG, "scheduler_notify_render_complete: failed to take mutex");
        return;
    }
    if (!sched.has_valid_schedule || !sched.running) {
        ESP_LOGD(TAG, "scheduler_notify_render_complete: not running or no schedule");
        return;
    }

    auto& current = sched.items[sched.current_item];
    ESP_LOGD(TAG, "scheduler_notify_render_complete: current_item=%lu, current_uuid=%02x%02x...",
        sched.current_item, current.uuid[0], current.uuid[1]);

    if (std::memcmp(current.uuid, uuid, UUID_SIZE_BYTES) == 0) {
        ESP_LOGD(TAG, "scheduler_notify_render_complete: is current item, displaying");
        log_item_state("notify_render_complete current", sched.current_item, current);
        sched.display_current();
    } else {
        ESP_LOGD(TAG, "scheduler_notify_render_complete: not current item, will display on next rotation");
    }
}

bool scheduler_update_sprite_data(const uint8_t* uuid, const uint8_t* data, size_t len, bool set_server_skipped) {
    if (uuid == nullptr) return false;

    raii::MutexGuard lock(sched.mutex);
    if (!lock) return false;

    ScheduleItem_t* item = sched.find_item(uuid);
    if (item == nullptr) {
        ESP_LOGD(TAG, "scheduler_update_sprite_data: item not found");
        return false;
    }

    item->flags.skipped_server = set_server_skipped;
    sprite_update_data(item->sprite, data, len);

    ESP_LOGD(TAG, "scheduler_update_sprite_data: updated uuid=%02x%02x, skipped_server=%d, len=%zu",
        uuid[0], uuid[1], set_server_skipped ? 1 : 0, len);

    return true;
}

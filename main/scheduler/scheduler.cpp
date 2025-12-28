#include "scheduler.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_log.h>
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

constexpr uint64_t SCHEDULE_REFRESH_MS = 5 * 60 * 1000;  // 5 minutes
constexpr TickType_t TICK_INTERVAL = pdMS_TO_TICKS(100);
constexpr size_t MIN_WEBP_SIZE = 12;

struct SchedulerState {
    ScheduleItem_t items[MAX_SCHEDULE_ITEMS] = {};
    TaskHandle_t task = nullptr;
    SemaphoreHandle_t mutex = nullptr;

    uint32_t current_item = 0;
    size_t item_count = 0;
    bool running = false;
    bool has_valid_schedule = false;

    TickType_t display_start_tick = 0;
    uint64_t last_schedule_request_time = 0;

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
        return has_valid_uuid(item) && !item.flags.skipped_user;
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

    bool in_prepare_window() const {
        uint64_t elapsed = get_elapsed_ms();
        uint32_t duration = get_display_duration_ms();
        if (duration <= PREPARE_WINDOW_MS) return true;  // Always in prepare window for short items
        return elapsed >= (duration - PREPARE_WINDOW_MS);
    }

    // --- Render request helpers ---

    bool should_request_render(const ScheduleItem_t& item) const {
        if (!is_render_candidate(item)) return false;

        switch (item.render_state) {
            case RenderState::NeedsRender:
                return true;
            case RenderState::RenderPending: {
                // Check for timeout
                TickType_t elapsed = xTaskGetTickCount() - item.render_request_tick;
                return pdTICKS_TO_MS(elapsed) >= RENDER_TIMEOUT_MS;
            }
            case RenderState::HasData:
                return false;
        }
        return false;
    }

    void request_render(ScheduleItem_t& item) {
        if (!has_valid_uuid(item)) return;

        ESP_LOGI(TAG, "Requesting render for %02x%02x...", item.uuid[0], item.uuid[1]);
        item.render_state = RenderState::RenderPending;
        item.render_request_tick = xTaskGetTickCount();
        send_render_request_to_server(item.uuid);
    }

    // --- Display helpers ---

    void display_current() {
        if (current_item >= item_count) {
            ESP_LOGI(TAG, "display_current: current_item=%lu >= item_count=%zu, clearing", current_item, item_count);
            display_clear();
            return;
        }

        auto& item = items[current_item];

        if (!is_displayable(item)) {
            ESP_LOGI(TAG, "display_current: item %lu not displayable (uuid=%02x%02x, user_skip=%d, server_skip=%d, state=%d, sprite=%p, len=%zu)",
                     current_item, item.uuid[0], item.uuid[1],
                     item.flags.skipped_user, item.flags.skipped_server,
                     static_cast<int>(item.render_state), item.sprite,
                     item.sprite ? sprite_get_length(item.sprite) : 0);
            display_clear();
            return;
        }

        ESP_LOGI(TAG, "display_current: showing item %lu (%02x%02x)", current_item, item.uuid[0], item.uuid[1]);
        show_sprite(item.sprite);
        sockets_send_currently_displaying((uint8_t*)item.uuid);
    }

    void advance_to(uint32_t index) {
        if (index >= item_count) {
            ESP_LOGI(TAG, "advance_to: index %lu >= item_count %zu, ignoring", index, item_count);
            return;
        }

        ESP_LOGI(TAG, "advance_to: %lu -> %lu", current_item, index);

        // Mark old item as needing re-render for next cycle (but only if different)
        if (current_item != index && current_item < item_count && has_valid_uuid(items[current_item])) {
            ESP_LOGI(TAG, "advance_to: marking old item %lu as NeedsRender", current_item);
            items[current_item].render_state = RenderState::NeedsRender;
        }

        current_item = index;
        display_start_tick = xTaskGetTickCount();
        display_current();
    }

    // Find next displayable item, returns current_item if none found
    uint32_t find_next_displayable(uint32_t from) const {
        if (item_count == 0) return from;

        for (size_t i = 1; i <= item_count; i++) {
            uint32_t idx = (from + i) % item_count;
            if (is_displayable(items[idx])) {
                return idx;
            }
        }
        return from;  // No displayable item found
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

void request_upcoming_renders() {
    if (!sched.has_valid_schedule) {
        return;
    }
    if (!sched.in_prepare_window()) {
        return;
    }
    if (sched.current_item >= sched.item_count) {
        ESP_LOGI(TAG, "request_upcoming_renders: current_item=%lu >= item_count=%zu", sched.current_item, sched.item_count);
        return;
    }

    auto& current = sched.items[sched.current_item];

    // Pinned items: request render for self
    if (current.flags.pinned) {
        ESP_LOGI(TAG, "request_upcoming_renders: pinned item, checking if render needed");
        if (sched.should_request_render(current)) {
            ESP_LOGI(TAG, "request_upcoming_renders: requesting render for pinned item");
            sched.request_render(current);
        }
        return;
    }

    // Non-pinned: request renders for upcoming items
    // Request for all server-skipped items until we find a "normal" one
    ESP_LOGI(TAG, "request_upcoming_renders: checking upcoming items from current=%lu", sched.current_item);
    for (size_t i = 1; i <= sched.item_count; i++) {
        uint32_t idx = (sched.current_item + i) % sched.item_count;
        auto& item = sched.items[idx];

        // Skip user-skipped entirely
        if (item.flags.skipped_user) continue;

        // Request render if needed
        if (sched.should_request_render(item)) {
            ESP_LOGI(TAG, "request_upcoming_renders: requesting render for item %lu", idx);
            sched.request_render(item);
        }

        // Stop after finding a non-server-skipped item
        if (!item.flags.skipped_server) break;
    }
}

void advance_to_next() {
    ESP_LOGI(TAG, "advance_to_next: item_count=%zu, current=%lu", sched.item_count, sched.current_item);

    if (sched.item_count == 0) {
        ESP_LOGI(TAG, "advance_to_next: no items, clearing display");
        display_clear();
        return;
    }

    uint32_t start = sched.current_item;

    // Try each item in order
    for (size_t attempts = 0; attempts < sched.item_count; attempts++) {
        uint32_t next = (start + 1 + attempts) % sched.item_count;
        auto& item = sched.items[next];

        ESP_LOGI(TAG, "advance_to_next: checking idx=%lu (uuid=%02x%02x, user_skip=%d, server_skip=%d, state=%d)",
                 next, item.uuid[0], item.uuid[1],
                 item.flags.skipped_user, item.flags.skipped_server, static_cast<int>(item.render_state));

        // Skip user-skipped items
        if (item.flags.skipped_user) {
            ESP_LOGI(TAG, "advance_to_next: idx=%lu user-skipped, continuing", next);
            continue;
        }

        // If server-skipped but now has data, clear the flag
        if (item.flags.skipped_server &&
            item.render_state == RenderState::HasData &&
            item.sprite != nullptr &&
            sprite_get_length(item.sprite) > 0) {
            ESP_LOGI(TAG, "advance_to_next: idx=%lu recovered from server-skip", next);
            item.flags.skipped_server = false;
        }

        // If displayable, advance to it
        if (sched.is_displayable(item)) {
            ESP_LOGI(TAG, "advance_to_next: idx=%lu is displayable, advancing", next);
            sched.advance_to(next);
            return;
        } else {
            ESP_LOGI(TAG, "advance_to_next: idx=%lu not displayable", next);
        }

        // Request render for server-skipped items we pass over
        if (item.flags.skipped_server && sched.should_request_render(item)) {
            ESP_LOGI(TAG, "advance_to_next: requesting render for server-skipped idx=%lu", next);
            sched.request_render(item);
        }
    }

    // No displayable items found
    ESP_LOGI(TAG, "advance_to_next: no displayable items found, clearing display");
    display_clear();
}

void process_display_timing() {
    if (sched.current_item >= sched.item_count) {
        return;
    }

    auto& current = sched.items[sched.current_item];

    // If current item became non-displayable, advance immediately
    if (!sched.is_displayable(current)) {
        ESP_LOGI(TAG, "process_display_timing: current item %lu not displayable (pinned=%d)",
                 sched.current_item, current.flags.pinned);

        // For pinned items, blank the screen and wait
        if (current.flags.pinned) {
            ESP_LOGI(TAG, "process_display_timing: pinned item, clearing display and waiting");
            display_clear();
            // Request render if needed
            if (sched.should_request_render(current)) {
                ESP_LOGI(TAG, "process_display_timing: requesting render for pinned item");
                sched.request_render(current);
            }
            return;
        }

        // Non-pinned: advance to next
        ESP_LOGI(TAG, "process_display_timing: advancing to next");
        advance_to_next();
        return;
    }

    // Check if display time expired
    if (!sched.is_display_expired()) return;

    ESP_LOGI(TAG, "process_display_timing: display time expired for item %lu (pinned=%d)",
             sched.current_item, current.flags.pinned);

    // Pinned items: reset timer and loop
    if (current.flags.pinned) {
        ESP_LOGI(TAG, "process_display_timing: pinned item, resetting timer");
        sched.display_start_tick = xTaskGetTickCount();
        // Re-request render for fresh data
        current.render_state = RenderState::NeedsRender;
        return;
    }

    // Non-pinned: advance to next
    ESP_LOGI(TAG, "process_display_timing: advancing to next item");
    advance_to_next();
}

void scheduler_task_func(void*) {
    ESP_LOGI(TAG, "Scheduler task started");

    while (true) {
        vTaskDelay(TICK_INTERVAL);

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

        // Request renders for upcoming items
        request_upcoming_renders();

        // Process display timing
        process_display_timing();
    }
}

}  // namespace

// --- Public API ---

void scheduler_init() {
    if (!sched.init()) {
        ESP_LOGE(TAG, "Failed to create mutex");
        return;
    }

    xTaskCreate(scheduler_task_func, "scheduler", 3072, nullptr, 5, &sched.task);
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

    ESP_LOGI(TAG, "scheduler_set_schedule: received %zu items", schedule_response->n_schedule_items);

    // Process items
    size_t new_count = 0;
    for (size_t i = 0; i < schedule_response->n_schedule_items && i < MAX_SCHEDULE_ITEMS; i++) {
        Kd__V1__ScheduleItem* pb_item = schedule_response->schedule_items[i];
        if (pb_item == nullptr || pb_item->uuid.len != UUID_SIZE_BYTES || pb_item->uuid.data == nullptr) {
            ESP_LOGW(TAG, "scheduler_set_schedule: skipping invalid item at index %zu", i);
            continue;
        }

        ScheduleItem_t& slot = sched.items[new_count];

        // Check if this item already exists (preserve sprite data)
        ScheduleItem_t* existing = sched.find_item(pb_item->uuid.data);

        ESP_LOGI(TAG, "scheduler_set_schedule: item[%zu] uuid=%02x%02x, pinned=%d, user_skip=%d, display_time=%d, existing=%p",
                 new_count, pb_item->uuid.data[0], pb_item->uuid.data[1],
                 pb_item->user_pinned, pb_item->user_skipped, pb_item->display_time, existing);

        std::memcpy(slot.uuid, pb_item->uuid.data, UUID_SIZE_BYTES);
        slot.flags.pinned = pb_item->user_pinned;
        slot.flags.skipped_user = pb_item->user_skipped;
        slot.flags.skipped_server = false;  // Reset on new schedule
        slot.flags.display_time = pb_item->display_time;

        if (existing != nullptr && existing != &slot && existing->sprite != nullptr) {
            // Reuse existing sprite from a different slot
            ESP_LOGI(TAG, "scheduler_set_schedule: reusing existing sprite from different slot");
            slot.sprite = existing->sprite;
            slot.render_state = existing->render_state;
            slot.render_request_tick = existing->render_request_tick;
            existing->sprite = nullptr;  // Prevent double-free
        } else if (existing == &slot && slot.sprite != nullptr) {
            // Same slot, keep existing sprite and state
            ESP_LOGI(TAG, "scheduler_set_schedule: keeping existing sprite in same slot (state=%d)",
                     static_cast<int>(slot.render_state));
        } else {
            // Allocate new sprite
            ESP_LOGI(TAG, "scheduler_set_schedule: allocating new sprite");
            if (slot.sprite != nullptr) {
                sprite_free(slot.sprite);
            }
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

    ESP_LOGI(TAG, "scheduler_set_schedule: running=%d, current_item=%lu, found_pinned=%d",
             sched.running, sched.current_item, found_pinned);

    // Request render for current item immediately
    if (sched.current_item < sched.item_count) {
        auto& current = sched.items[sched.current_item];
        ESP_LOGI(TAG, "scheduler_set_schedule: current item state=%d, displayable=%d",
                 static_cast<int>(current.render_state), sched.is_displayable(current));

        if (sched.should_request_render(current)) {
            ESP_LOGI(TAG, "scheduler_set_schedule: requesting render for current item");
            sched.request_render(current);
        }

        // If we already have data, display it
        if (sched.is_displayable(current)) {
            ESP_LOGI(TAG, "scheduler_set_schedule: displaying current item");
            sched.display_current();
        } else {
            ESP_LOGI(TAG, "scheduler_set_schedule: current item not displayable, clearing");
            display_clear();
        }
    }

    ESP_LOGI(TAG, "scheduler_set_schedule: complete - %zu items, starting at %lu", sched.item_count, sched.current_item);
}

void scheduler_clear() {
    raii::MutexGuard lock(sched.mutex);
    if (!lock) return;

    ESP_LOGI(TAG, "Clearing schedule");

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
    if (uuid == nullptr) {
        ESP_LOGW(TAG, "handle_render_response: uuid is null");
        return;
    }

    ESP_LOGI(TAG, "handle_render_response: uuid=%02x%02x, len=%zu, error=%d", uuid[0], uuid[1], len, server_error);

    raii::MutexGuard lock(sched.mutex);
    if (!lock) {
        ESP_LOGW(TAG, "handle_render_response: failed to acquire mutex");
        return;
    }

    ScheduleItem_t* item = sched.find_item(uuid);
    if (item == nullptr) {
        ESP_LOGI(TAG, "handle_render_response: item %02x%02x not found in schedule", uuid[0], uuid[1]);
        return;
    }

    // Find index for logging
    size_t item_idx = 0;
    for (size_t i = 0; i < sched.item_count; i++) {
        if (&sched.items[i] == item) {
            item_idx = i;
            break;
        }
    }
    bool is_current = (&sched.items[sched.current_item] == item);
    ESP_LOGI(TAG, "handle_render_response: found at idx=%zu, is_current=%d, pinned=%d",
             item_idx, is_current, item->flags.pinned);

    // Handle server error or empty data
    if (server_error || data == nullptr || len == 0) {
        ESP_LOGI(TAG, "handle_render_response: server skipped item %02x%02x", uuid[0], uuid[1]);
        item->flags.skipped_server = true;
        // Keep render_state as RenderPending so we don't immediately re-request
        // The timeout mechanism will allow retry after RENDER_TIMEOUT_MS
        item->render_state = RenderState::RenderPending;
        sprite_update_data(item->sprite, nullptr, 0);

        // If this is the current pinned item, blank screen
        if (is_current && item->flags.pinned) {
            ESP_LOGI(TAG, "handle_render_response: current pinned item skipped, clearing display");
            display_clear();
        }
        // If this is the current non-pinned item, advance
        else if (is_current) {
            ESP_LOGI(TAG, "handle_render_response: current non-pinned item skipped, advancing");
            advance_to_next();
        }
        return;
    }

    // Validate WebP data
    if (!validate_webp(data, len)) {
        ESP_LOGW(TAG, "handle_render_response: invalid WebP data for %02x%02x", uuid[0], uuid[1]);
        item->flags.skipped_server = true;
        item->render_state = RenderState::NeedsRender;
        sprite_update_data(item->sprite, nullptr, 0);
        return;
    }

    // Success - update sprite data
    item->flags.skipped_server = false;
    item->render_state = RenderState::HasData;
    sprite_update_data(item->sprite, data, len);

    ESP_LOGI(TAG, "handle_render_response: sprite data updated for %02x%02x (%zu bytes)", uuid[0], uuid[1], len);

    // If this is the current item, display it
    if (is_current) {
        ESP_LOGI(TAG, "handle_render_response: this is current item, displaying");
        sched.display_current();
    }
}

void scheduler_handle_button_next() {
    ESP_LOGI(TAG, "Button NEXT pressed");

    raii::MutexGuard lock(sched.mutex);
    if (!lock) {
        ESP_LOGW(TAG, "Button NEXT: failed to acquire mutex");
        return;
    }
    if (!sched.running) {
        ESP_LOGI(TAG, "Button NEXT: scheduler not running");
        return;
    }
    if (sched.item_count == 0) {
        ESP_LOGI(TAG, "Button NEXT: no items in schedule");
        return;
    }

    ESP_LOGI(TAG, "Button NEXT: current=%lu, item_count=%zu", sched.current_item, sched.item_count);

    auto& current = sched.items[sched.current_item];

    // If current item is pinned, unpin it and notify server
    if (current.flags.pinned) {
        ESP_LOGI(TAG, "Button NEXT: unpinning current item %02x%02x", current.uuid[0], current.uuid[1]);
        current.flags.pinned = false;
        sockets_send_modify_schedule_item(current.uuid, false, current.flags.skipped_user);
    }

    // Find next item with data and advance immediately (skip current item)
    for (size_t i = 1; i < sched.item_count; i++) {
        uint32_t idx = (sched.current_item + i) % sched.item_count;
        auto& item = sched.items[idx];

        ESP_LOGI(TAG, "Button NEXT: checking idx=%lu, user_skip=%d, server_skip=%d, render_state=%d, sprite=%p",
                 idx, item.flags.skipped_user, item.flags.skipped_server,
                 static_cast<int>(item.render_state), item.sprite);

        // Skip user-skipped items
        if (item.flags.skipped_user) {
            ESP_LOGI(TAG, "Button NEXT: idx=%lu skipped (user)", idx);
            continue;
        }

        // If server-skipped but now has data, clear the flag
        if (item.flags.skipped_server &&
            item.render_state == RenderState::HasData &&
            item.sprite != nullptr &&
            sprite_get_length(item.sprite) > 0) {
            ESP_LOGI(TAG, "Button NEXT: idx=%lu recovered from server-skip", idx);
            item.flags.skipped_server = false;
        }

        // Advance to first displayable item (must be different from current)
        if (sched.is_displayable(item)) {
            ESP_LOGI(TAG, "Button NEXT: advancing to idx=%lu", idx);
            sched.advance_to(idx);
            return;
        } else {
            ESP_LOGI(TAG, "Button NEXT: idx=%lu not displayable", idx);
        }
    }

    // No other displayable items found - stay on current item
    ESP_LOGI(TAG, "Button NEXT: no other displayable items, staying on current");
}

void scheduler_handle_button_prev() {
    ESP_LOGI(TAG, "Button PREV pressed");

    raii::MutexGuard lock(sched.mutex);
    if (!lock) {
        ESP_LOGW(TAG, "Button PREV: failed to acquire mutex");
        return;
    }
    if (!sched.running) {
        ESP_LOGI(TAG, "Button PREV: scheduler not running");
        return;
    }
    if (sched.item_count == 0) {
        ESP_LOGI(TAG, "Button PREV: no items in schedule");
        return;
    }

    ESP_LOGI(TAG, "Button PREV: current=%lu, item_count=%zu", sched.current_item, sched.item_count);

    auto& current = sched.items[sched.current_item];

    // If current item is pinned, unpin it and notify server
    if (current.flags.pinned) {
        ESP_LOGI(TAG, "Button PREV: unpinning current item %02x%02x", current.uuid[0], current.uuid[1]);
        current.flags.pinned = false;
        sockets_send_modify_schedule_item(current.uuid, false, current.flags.skipped_user);
    }

    // Find previous item with data and advance immediately (search backwards, skip current item)
    for (size_t i = 1; i < sched.item_count; i++) {
        // Go backwards: current - i, with wrap-around
        uint32_t idx = (sched.current_item + sched.item_count - i) % sched.item_count;
        auto& item = sched.items[idx];

        ESP_LOGI(TAG, "Button PREV: checking idx=%lu, user_skip=%d, server_skip=%d, render_state=%d, sprite=%p",
                 idx, item.flags.skipped_user, item.flags.skipped_server,
                 static_cast<int>(item.render_state), item.sprite);

        // Skip user-skipped items
        if (item.flags.skipped_user) {
            ESP_LOGI(TAG, "Button PREV: idx=%lu skipped (user)", idx);
            continue;
        }

        // If server-skipped but now has data, clear the flag
        if (item.flags.skipped_server &&
            item.render_state == RenderState::HasData &&
            item.sprite != nullptr &&
            sprite_get_length(item.sprite) > 0) {
            ESP_LOGI(TAG, "Button PREV: idx=%lu recovered from server-skip", idx);
            item.flags.skipped_server = false;
        }

        // Advance to first displayable item (must be different from current)
        if (sched.is_displayable(item)) {
            ESP_LOGI(TAG, "Button PREV: advancing to idx=%lu", idx);
            sched.advance_to(idx);
            return;
        } else {
            ESP_LOGI(TAG, "Button PREV: idx=%lu not displayable", idx);
        }
    }

    // No other displayable items found - stay on current item
    ESP_LOGI(TAG, "Button PREV: no other displayable items, staying on current");
}

void scheduler_handle_button_pin() {
    ESP_LOGI(TAG, "Button PIN pressed");

    raii::MutexGuard lock(sched.mutex);
    if (!lock) {
        ESP_LOGW(TAG, "Button PIN: failed to acquire mutex");
        return;
    }
    if (!sched.running) {
        ESP_LOGI(TAG, "Button PIN: scheduler not running");
        return;
    }
    if (sched.item_count == 0) {
        ESP_LOGI(TAG, "Button PIN: no items in schedule");
        return;
    }

    ESP_LOGI(TAG, "Button PIN: current=%lu, item_count=%zu", sched.current_item, sched.item_count);

    auto& current = sched.items[sched.current_item];

    // Already pinned, nothing to do
    if (current.flags.pinned) {
        ESP_LOGI(TAG, "Button PIN: item already pinned");
        return;
    }

    // Pin the current item
    current.flags.pinned = true;
    ESP_LOGI(TAG, "Button PIN: pinned item %02x%02x", current.uuid[0], current.uuid[1]);

    // Reset display timer (loop on this item)
    sched.display_start_tick = xTaskGetTickCount();

    // Notify server of pin
    sockets_send_modify_schedule_item(current.uuid, true, current.flags.skipped_user);
    ESP_LOGI(TAG, "Button PIN: notified server");
}

#include "render_requests.h"

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <cstring>
#include <webp/decode.h>

#include "../scheduler/scheduler.h"
#include "../sprites/sprites.h"

static const char* TAG = "render_requests";

// Forward declaration - implemented in sockets.cpp
extern void send_render_request_to_server(const uint8_t* uuid);

namespace {

    constexpr size_t MAX_TRACKED_RENDERS = 32;
    constexpr size_t MIN_WEBP_SIZE = 12;  // Minimum size for a valid WebP file (RIFF header)
    constexpr uint8_t WEBP_RIFF_HEADER[] = { 'R', 'I', 'F', 'F' };
    constexpr uint8_t WEBP_WEBP_HEADER[] = { 'W', 'E', 'B', 'P' };
    constexpr TickType_t RENDER_TIMEOUT_TICKS = pdMS_TO_TICKS(5000);  // 5 second timeout
    constexpr TickType_t RENDER_COOLDOWN_TICKS = pdMS_TO_TICKS(5000);  // 5 second cooldown after success
    constexpr uint8_t MAX_TIMEOUT_RETRIES = 3;  // Max retries before marking as failed

    struct RenderTrackingEntry {
        uint8_t uuid[UUID_SIZE_BYTES] = { 0 };
        RenderState state = RenderState::NeedsRender;
        uint8_t retry_count = 0;
        bool valid = false;
        TickType_t request_start_tick = 0;  // When the request was sent
        TickType_t last_success_tick = 0;   // When the last successful render completed
    };

    struct RenderRequestsState {
        RenderTrackingEntry entries[MAX_TRACKED_RENDERS] = {};
        SemaphoreHandle_t mutex = nullptr;

        bool init() {
            mutex = xSemaphoreCreateMutex();
            return mutex != nullptr;
        }

        bool uuid_matches(const RenderTrackingEntry& entry, const uint8_t* uuid) const {
            if (!entry.valid || uuid == nullptr) return false;
            return std::memcmp(entry.uuid, uuid, UUID_SIZE_BYTES) == 0;
        }

        RenderTrackingEntry* find_entry(const uint8_t* uuid) {
            if (uuid == nullptr) return nullptr;
            for (auto& entry : entries) {
                if (uuid_matches(entry, uuid)) {
                    return &entry;
                }
            }
            return nullptr;
        }

        RenderTrackingEntry* find_or_create_entry(const uint8_t* uuid) {
            if (uuid == nullptr) return nullptr;

            // First try to find existing
            RenderTrackingEntry* existing = find_entry(uuid);
            if (existing != nullptr) return existing;

            // Find empty slot
            for (auto& entry : entries) {
                if (!entry.valid) {
                    std::memcpy(entry.uuid, uuid, UUID_SIZE_BYTES);
                    entry.state = RenderState::NeedsRender;
                    entry.retry_count = 0;
                    entry.valid = true;
                    return &entry;
                }
            }

            ESP_LOGW(TAG, "No available slots for render tracking");
            return nullptr;
        }

        void clear_entry(const uint8_t* uuid) {
            RenderTrackingEntry* entry = find_entry(uuid);
            if (entry != nullptr) {
                std::memset(entry, 0, sizeof(RenderTrackingEntry));
            }
        }

        void clear_all() {
            for (auto& entry : entries) {
                std::memset(&entry, 0, sizeof(RenderTrackingEntry));
            }
        }

        size_t count_pending() const {
            size_t count = 0;
            for (const auto& entry : entries) {
                if (entry.valid && entry.state == RenderState::RenderPending) {
                    count++;
                }
            }
            return count;
        }
    };

    RenderRequestsState state;

    void log_uuid(const char* prefix, const uint8_t* uuid) {
        if (uuid == nullptr) return;
        char uuid_str[UUID_SIZE_BYTES * 2 + 1] = {};
        for (size_t i = 0; i < UUID_SIZE_BYTES; i++) {
            snprintf(uuid_str + i * 2, sizeof(uuid_str) - i * 2, "%02x", uuid[i]);
        }
        ESP_LOGD(TAG, "%s: %s", prefix, uuid_str);
    }

}  // namespace

void render_requests_init() {
    ESP_LOGD(TAG, "render_requests_init: initializing");
    if (!state.init()) {
        ESP_LOGE(TAG, "render_requests_init: failed to create mutex");
        return;
    }
    ESP_LOGD(TAG, "render_requests_init: complete");
}

bool render_request(const uint8_t* uuid) {
    if (uuid == nullptr || state.mutex == nullptr) {
        return false;
    }

    if (xSemaphoreTake(state.mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return false;
    }

    RenderTrackingEntry* entry = state.find_or_create_entry(uuid);
    if (entry == nullptr) {
        xSemaphoreGive(state.mutex);
        return false;
    }

    TickType_t current_tick = xTaskGetTickCount();

    // Check if we should skip this request (already pending and not timed out)
    if (entry->state == RenderState::RenderPending) {
        TickType_t elapsed = current_tick - entry->request_start_tick;
        if (elapsed < RENDER_TIMEOUT_TICKS) {
            xSemaphoreGive(state.mutex);
            return false;
        }
        // Request timed out
        entry->retry_count++;
        if (entry->retry_count >= MAX_TIMEOUT_RETRIES) {
            ESP_LOGD(TAG, "Max retries reached, will retry next cycle");
            entry->state = RenderState::NeedsRender;
            entry->retry_count = 0;
            xSemaphoreGive(state.mutex);
            return false;
        }
        ESP_LOGD(TAG, "Timeout, retry %d/%d", entry->retry_count, MAX_TIMEOUT_RETRIES);
    }

    // Check cooldown after successful render
    if (entry->state == RenderState::RenderComplete && entry->last_success_tick > 0) {
        TickType_t elapsed = current_tick - entry->last_success_tick;
        if (elapsed < RENDER_COOLDOWN_TICKS) {
            xSemaphoreGive(state.mutex);
            return false;
        }
    }

    log_uuid("Requesting render", uuid);
    entry->state = RenderState::RenderPending;
    entry->request_start_tick = current_tick;

    xSemaphoreGive(state.mutex);

    // Send the actual request via sockets
    send_render_request_to_server(uuid);

    return true;
}

bool render_validate_sprite_data(const uint8_t* data, size_t len) {
    if (data == nullptr || len < MIN_WEBP_SIZE) {
        ESP_LOGD(TAG, "Invalid data: null or too small (%zu)", len);
        return false;
    }

    // Check RIFF header
    if (std::memcmp(data, WEBP_RIFF_HEADER, 4) != 0) {
        ESP_LOGD(TAG, "Invalid WebP: missing RIFF header");
        return false;
    }

    // Check WEBP signature at offset 8
    if (std::memcmp(data + 8, WEBP_WEBP_HEADER, 4) != 0) {
        ESP_LOGD(TAG, "Invalid WebP: missing WEBP signature");
        return false;
    }

    // Use libwebp to validate the data can be decoded
    int width = 0, height = 0;
    if (!WebPGetInfo(data, len, &width, &height)) {
        ESP_LOGD(TAG, "Invalid WebP: decode failed");
        return false;
    }

    if (width <= 0 || height <= 0 || width > 1024 || height > 1024) {
        ESP_LOGD(TAG, "Invalid WebP: bad dimensions %dx%d", width, height);
        return false;
    }

    return true;
}

RenderResult render_response_received(const uint8_t* uuid, const uint8_t* data, size_t len, bool server_error) {
    if (uuid == nullptr) {
        return RenderResult::ItemNotFound;
    }

    log_uuid("Response received", uuid);
    ESP_LOGD(TAG, "Response: data_len=%zu, server_error=%d", len, server_error ? 1 : 0);

    if (state.mutex == nullptr) {
        return RenderResult::ItemNotFound;
    }

    if (xSemaphoreTake(state.mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return RenderResult::ItemNotFound;
    }

    RenderTrackingEntry* entry = state.find_entry(uuid);

    // Handle server error - mark as skipped
    if (server_error) {
        ESP_LOGD(TAG, "Response: server error, setting skipped_server=true");
        if (entry != nullptr) {
            entry->state = RenderState::RenderFailed;
        }
        xSemaphoreGive(state.mutex);
        // Use scheduler's locked function to update sprite data
        scheduler_update_sprite_data(uuid, nullptr, 0, true);
        return RenderResult::ServerError;
    }

    // Handle empty data - mark as skipped
    if (data == nullptr || len == 0) {
        ESP_LOGD(TAG, "Response: empty data, setting skipped_server=true");
        if (entry != nullptr) {
            entry->state = RenderState::RenderFailed;
        }
        xSemaphoreGive(state.mutex);
        scheduler_update_sprite_data(uuid, nullptr, 0, true);
        return RenderResult::InvalidData;
    }

    // Validate the sprite data
    if (!render_validate_sprite_data(data, len)) {
        if (entry != nullptr) {
            entry->state = RenderState::ValidationFailed;
            entry->retry_count++;

            // Retry immediately for validation failures (up to 3 times)
            if (entry->retry_count < 3) {
                ESP_LOGD(TAG, "Response: validation failed, retrying (%d/3)", entry->retry_count);
                xSemaphoreGive(state.mutex);
                render_request(uuid);
                return RenderResult::InvalidData;
            }
            else {
                ESP_LOGD(TAG, "Response: validation failed after max retries, setting skipped_server=true");
                entry->state = RenderState::RenderFailed;
            }
        }
        xSemaphoreGive(state.mutex);
        scheduler_update_sprite_data(uuid, nullptr, 0, true);
        return RenderResult::InvalidData;
    }

    // Validation passed - update the sprite and clear skipped_server flag
    ESP_LOGD(TAG, "Response: validation passed, clearing skipped_server, updating sprite (%zu bytes)", len);

    if (entry != nullptr) {
        entry->state = RenderState::RenderComplete;
        entry->retry_count = 0;
        entry->last_success_tick = xTaskGetTickCount();
    }

    xSemaphoreGive(state.mutex);

    // Use scheduler's locked function to update sprite data
    if (!scheduler_update_sprite_data(uuid, data, len, false)) {
        ESP_LOGD(TAG, "Response: item not found in schedule");
        return RenderResult::ItemNotFound;
    }

    // Notify scheduler so it can display immediately if this is the current item
    ESP_LOGD(TAG, "Response: notifying scheduler of render complete");
    scheduler_notify_render_complete(uuid);

    return RenderResult::Success;
}

RenderState render_get_state(const uint8_t* uuid) {
    if (uuid == nullptr || state.mutex == nullptr) {
        return RenderState::NeedsRender;
    }

    if (xSemaphoreTake(state.mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return RenderState::NeedsRender;
    }

    RenderTrackingEntry* entry = state.find_entry(uuid);
    RenderState result = (entry != nullptr) ? entry->state : RenderState::NeedsRender;

    xSemaphoreGive(state.mutex);
    return result;
}

void render_set_state(const uint8_t* uuid, RenderState new_state) {
    if (uuid == nullptr || state.mutex == nullptr) return;

    if (xSemaphoreTake(state.mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return;
    }

    RenderTrackingEntry* entry = state.find_or_create_entry(uuid);
    if (entry != nullptr) {
        entry->state = new_state;
        if (new_state == RenderState::NeedsRender) {
            entry->retry_count = 0;
        }
    }

    xSemaphoreGive(state.mutex);
}

void render_clear_all() {
    if (state.mutex == nullptr) return;

    if (xSemaphoreTake(state.mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return;
    }

    state.clear_all();

    xSemaphoreGive(state.mutex);
}

void render_mark_needs_render(const uint8_t* uuid) {
    render_set_state(uuid, RenderState::NeedsRender);
}

size_t render_get_pending_count() {
    if (state.mutex == nullptr) return 0;

    if (xSemaphoreTake(state.mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return 0;
    }

    size_t count = state.count_pending();

    xSemaphoreGive(state.mutex);
    return count;
}

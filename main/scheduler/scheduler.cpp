// Scheduler - Event-driven app schedule management
#include "scheduler.h"

#include <cstring>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <esp_event.h>

#include "apps.h"
#include "webp_player.h"
#include "display.h"
#include "messages.h"
#include "sockets.h"

static const char* TAG = "scheduler";

namespace {

    //------------------------------------------------------------------------------
    // Constants
    //------------------------------------------------------------------------------

    constexpr int64_t RETRY_INTERVAL_US = 10 * 1000 * 1000;  // 10 seconds in microseconds

    //------------------------------------------------------------------------------
    // State
    //------------------------------------------------------------------------------

    struct SchedulerState {
        // Schedule tracking
        size_t current_idx = 0;
        bool has_schedule = false;
        bool running = false;

        // Retry timer for when nothing displayable
        esp_timer_handle_t retry_timer = nullptr;
    };

    SchedulerState state;

    //------------------------------------------------------------------------------
    // Render Request
    //------------------------------------------------------------------------------

    void request_render(App_t* app) {
        if (!app) return;

        msg_request_app_render(app);
        ESP_LOGD(TAG, "Requested render for %02x%02x...", app->uuid[0], app->uuid[1]);
    }

    //------------------------------------------------------------------------------
    // Find Next Displayable App
    //------------------------------------------------------------------------------

    int find_next_displayable(size_t from_idx, bool skip_current) {
        size_t count = apps_count();
        if (count == 0) return -1;

        size_t start = skip_current ? 1 : 0;
        for (size_t i = start; i <= count; i++) {
            size_t idx = (from_idx + i) % count;
            App_t* app = apps_get_by_index(idx);
            if (app && app_is_displayable(app)) {
                return static_cast<int>(idx);
            }
        }
        return -1;
    }

    //------------------------------------------------------------------------------
    // Retry Timer
    //------------------------------------------------------------------------------

    void stop_retry_timer() {
        if (state.retry_timer) {
            esp_timer_stop(state.retry_timer);
        }
    }

    void start_retry_timer() {
        if (!state.retry_timer) {
            return;
        }

        // Stop any existing timer first
        esp_timer_stop(state.retry_timer);

        // Start one-shot timer
        esp_err_t err = esp_timer_start_once(state.retry_timer, RETRY_INTERVAL_US);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to start retry timer: %s", esp_err_to_name(err));
        }
    }

    void retry_timer_callback(void*) {
        if (!state.running || !state.has_schedule) {
            return;
        }

        ESP_LOGI(TAG, "Retry timer fired, requesting renders for all apps");

        size_t count = apps_count();
        if (count == 0) return;

        // Request render for all non-user-skipped apps
        for (size_t i = 0; i < count; i++) {
            App_t* app = apps_get_by_index(i);
            if (app && !app->skipped) {
                request_render(app);
            }
        }

        // Re-arm timer if still nothing displayable
        if (find_next_displayable(0, false) < 0) {
            start_retry_timer();
        }
    }

    //------------------------------------------------------------------------------
    // Play App Helper
    //------------------------------------------------------------------------------

    void play_app_at_index(size_t idx) {
        App_t* app = apps_get_by_index(idx);
        if (!app) {
            ESP_LOGW(TAG, "play_app_at_index(%zu): app is null", idx);
            return;
        }

        state.current_idx = idx;
        bool displayable = app_is_displayable(app);
        ESP_LOGI(TAG, "play_app_at_index(%zu): displayable=%d, len=%zu", idx, displayable, app->len);

        if (displayable) {
            stop_retry_timer();
            webp_player_play_app(app, app->display_time * 1000, true);
            ESP_LOGI(TAG, "Playing app at index %zu (duration: %us)",
                idx, app->display_time);
        }
        else {
            // No data - request render and trigger NEED_NEXT if in display mode
            request_render(app);
            webp_player_request_next();

            // Start retry timer as backup
            start_retry_timer();

            ESP_LOGI(TAG, "App at index %zu has no data, requesting render", idx);
        }
    }

    //------------------------------------------------------------------------------
    // Advance to Next App
    //------------------------------------------------------------------------------

    void advance_to_next() {
        size_t count = apps_count();

        if (count == 0) {
            display_clear();
            stop_retry_timer();
            return;
        }

        // Check if current app is pinned
        App_t* current = apps_get_by_index(state.current_idx);
        if (current && current->pinned) {
            // Pinned: stay on current, re-request render if no data
            if (app_is_displayable(current)) {
                webp_player_play_app(current, current->display_time * 1000, true);
            }
            else {
                request_render(current);
                webp_player_request_next();
                start_retry_timer();
            }
            ESP_LOGI(TAG, "Pinned app - staying on index %zu", state.current_idx);
            return;
        }

        // Find next displayable
        int next = find_next_displayable(state.current_idx, true);

        if (next >= 0) {
            play_app_at_index(static_cast<size_t>(next));
        }
        else {
            // No displayable items - request renders and trigger NEED_NEXT
            for (size_t i = 0; i < count; i++) {
                App_t* app = apps_get_by_index(i);
                if (app && !app->skipped) {
                    request_render(app);
                }
            }

            webp_player_request_next();
            start_retry_timer();
            ESP_LOGW(TAG, "No displayable apps, starting retry timer");
        }
    }

    //------------------------------------------------------------------------------
    // WebP Player Event Handlers
    //------------------------------------------------------------------------------

    void on_playing(const webp_player_playing_evt_t* evt) {
        if (!evt) return;

        // Send "currently displaying" message for RAM apps
        if (evt->source_type == WEBP_SOURCE_RAM && evt->ram_app) {
            msg_send_currently_displaying(evt->ram_app);
        }
    }

    void on_prepare_next(const webp_player_prepare_next_evt_t* evt) {
        if (!state.running || !state.has_schedule) return;

        size_t count = apps_count();
        if (count == 0) return;

        App_t* current = apps_get_by_index(state.current_idx);

        // For pinned apps or single items, re-request current
        if ((current && current->pinned) || count == 1) {
            if (current && !current->skipped) {
                request_render(current);
            }
            return;
        }

        // Request render for next 1-2 apps
        size_t requested = 0;
        for (size_t i = 1; i <= count && requested < 2; i++) {
            size_t idx = (state.current_idx + i) % count;
            App_t* app = apps_get_by_index(idx);
            if (app && !app->skipped) {
                request_render(app);
                requested++;
            }
        }

        ESP_LOGD(TAG, "PREPARE_NEXT: requested %zu renders", requested);
    }

    void on_stopped() {
        if (!state.running || !state.has_schedule) return;

        advance_to_next();
    }

    void on_error(const webp_player_error_evt_t* evt) {
        if (!state.running || !state.has_schedule) return;

        ESP_LOGW(TAG, "Player error, advancing to next");
        advance_to_next();
    }

    void on_need_next() {
        if (!state.running || !state.has_schedule) return;

        // Find next displayable app and play it
        int next = find_next_displayable(state.current_idx, true);
        if (next >= 0) {
            ESP_LOGI(TAG, "NEED_NEXT: found displayable app at index %d", next);
            play_app_at_index(static_cast<size_t>(next));
        }
        else {
            // No displayable app found, try from beginning
            next = find_next_displayable(0, false);
            if (next >= 0) {
                ESP_LOGI(TAG, "NEED_NEXT: fallback to index %d", next);
                play_app_at_index(static_cast<size_t>(next));
            }
        }
    }

    void webp_player_event_handler(void*, esp_event_base_t, int32_t event_id, void* event_data) {
        switch (event_id) {
        case WEBP_PLAYER_EVT_PLAYING:
            on_playing(static_cast<webp_player_playing_evt_t*>(event_data));
            break;
        case WEBP_PLAYER_EVT_PREPARE_NEXT:
            on_prepare_next(static_cast<webp_player_prepare_next_evt_t*>(event_data));
            break;
        case WEBP_PLAYER_EVT_STOPPED:
            on_stopped();
            break;
        case WEBP_PLAYER_EVT_ERROR:
            on_error(static_cast<webp_player_error_evt_t*>(event_data));
            break;
        case WEBP_PLAYER_EVT_NEED_NEXT:
            on_need_next();
            break;
        }
    }

}  // namespace

//------------------------------------------------------------------------------
// Public API
//------------------------------------------------------------------------------

void scheduler_init() {
    // Create retry timer
    esp_timer_create_args_t timer_args = {
        .callback = retry_timer_callback,
        .arg = nullptr,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "sched_retry",
        .skip_unhandled_events = true,
    };

    esp_err_t err = esp_timer_create(&timer_args, &state.retry_timer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create retry timer: %s", esp_err_to_name(err));
    }

    // Register for webp_player events
    esp_event_handler_register(
        WEBP_PLAYER_EVENTS,
        ESP_EVENT_ANY_ID,
        webp_player_event_handler,
        nullptr
    );

    ESP_LOGI(TAG, "Scheduler initialized");
}

void scheduler_start() {
    state.running = true;
    ESP_LOGI(TAG, "Scheduler started");
}

void scheduler_stop() {
    state.running = false;
    stop_retry_timer();
    ESP_LOGI(TAG, "Scheduler stopped");
}

bool scheduler_has_schedule() {
    return state.has_schedule && apps_count() > 0;
}

void scheduler_on_schedule_received() {
    size_t count = apps_count();

    if (count == 0) {
        state.has_schedule = false;
        display_clear();
        stop_retry_timer();
        ESP_LOGI(TAG, "Empty schedule received");
        return;
    }

    state.has_schedule = true;
    stop_retry_timer();

    // Find pinned app, or first non-skipped app
    int start_idx = -1;

    for (size_t i = 0; i < count; i++) {
        App_t* app = apps_get_by_index(i);
        if (app && app->pinned && !app->skipped) {
            start_idx = static_cast<int>(i);
            break;
        }
    }

    if (start_idx < 0) {
        // No pinned app - find first displayable
        start_idx = find_next_displayable(0, false);
    }

    if (start_idx < 0) {
        // No displayable - use first non-skipped
        for (size_t i = 0; i < count; i++) {
            App_t* app = apps_get_by_index(i);
            if (app && !app->skipped) {
                start_idx = static_cast<int>(i);
                break;
            }
        }
    }

    if (start_idx >= 0) {
        play_app_at_index(static_cast<size_t>(start_idx));
    }
    else {
        // All apps are user-skipped
        display_clear();
        ESP_LOGW(TAG, "All apps are skipped");
    }

    ESP_LOGI(TAG, "Schedule received: %zu apps, starting at index %d", count, start_idx);
}

void scheduler_on_render_response(const uint8_t* uuid, bool success) {
    if (!uuid) return;

    App_t* app = app_find(uuid);
    if (!app) {
        ESP_LOGW(TAG, "Render response for unknown app");
        return;
    }

    if (success) {
        ESP_LOGI(TAG, "Render success for %02x%02x...", uuid[0], uuid[1]);

        // Stop retry timer - we have displayable content
        stop_retry_timer();

        // Check if this is the current app - play it immediately
        App_t* current = apps_get_by_index(state.current_idx);

        if (app == current) {
            // Current app now has data, play it (interrupts any placeholder)
            webp_player_play_app(app, app->display_time * 1000, true);
            ESP_LOGI(TAG, "Current app now has data, playing");
        }
        else if (!current || !app_is_displayable(current)) {
            // Current app is null or not displayable - find and play first displayable
            int idx = find_next_displayable(0, false);
            if (idx >= 0) {
                play_app_at_index(static_cast<size_t>(idx));
                ESP_LOGI(TAG, "Current app not displayable, playing index %d", idx);
            }
        }
        else {
            // Current app is playing fine, this render was for a future app
            ESP_LOGD(TAG, "Render for non-current app, ignoring");
        }
    }
    else {
        ESP_LOGW(TAG, "Render failed for %02x%02x...", uuid[0], uuid[1]);
        // App data remains empty, retry timer will eventually re-request
    }
}

void scheduler_on_connect() {
    // Enable display mode - NEED_NEXT events can now be emitted
    webp_player_set_display_mode(true);

    ESP_LOGI(TAG, "Connected - display mode enabled");
}

void scheduler_on_disconnect() {
    state.has_schedule = false;
    stop_retry_timer();

    // Disable display mode - no NEED_NEXT events during reconnect
    webp_player_set_display_mode(false);

    // Show connecting sprite
    webp_player_play_embedded("connecting", true);

    ESP_LOGI(TAG, "Disconnected - showing connecting sprite");
}

const uint8_t* scheduler_get_current_uuid() {
    App_t* app = apps_get_by_index(state.current_idx);
    return app ? app->uuid : nullptr;
}

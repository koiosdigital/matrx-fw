// Scheduler - FSM-based app schedule management
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
#include "daughterboard.h"

static const char* TAG = "scheduler";

namespace {

    //--------------------------------------------------------------------------
    // Constants
    //--------------------------------------------------------------------------

    constexpr int64_t RETRY_INTERVAL_US = 10 * 1000 * 1000;   // 10 seconds
    constexpr int64_t PREPARE_BEFORE_US = 2 * 1000 * 1000;    // 2 seconds before duration ends

    //--------------------------------------------------------------------------
    // State Machine
    //--------------------------------------------------------------------------

    enum class State {
        IDLE,              // Not playing (stopped, disconnected, awaiting, empty)
        ROTATING_PLAYING,  // Playing app in rotation
        ROTATING_WAITING,  // Waiting for data in rotation
        SINGLE_PLAYING,    // Playing pinned app
        SINGLE_BLANK,      // Pinned app not displayable
    };

    const char* state_name(State s) {
        switch (s) {
            case State::IDLE:             return "IDLE";
            case State::ROTATING_PLAYING: return "ROTATING_PLAYING";
            case State::ROTATING_WAITING: return "ROTATING_WAITING";
            case State::SINGLE_PLAYING:   return "SINGLE_PLAYING";
            case State::SINGLE_BLANK:     return "SINGLE_BLANK";
        }
        return "UNKNOWN";
    }

    //--------------------------------------------------------------------------
    // Context
    //--------------------------------------------------------------------------

    struct Context {
        State state = State::IDLE;
        size_t current_idx = 0;
        App_t* pinned_app = nullptr;       // Non-null when in single mode
        esp_timer_handle_t prepare_timer = nullptr;
        esp_timer_handle_t retry_timer = nullptr;
        uint32_t playback_start_ms = 0;    // When current app started
        bool connected = false;
    };

    Context ctx;

    //--------------------------------------------------------------------------
    // State Transition
    //--------------------------------------------------------------------------

    void transition_to(State new_state) {
        if (ctx.state != new_state) {
            ESP_LOGI(TAG, "State: %s -> %s", state_name(ctx.state), state_name(new_state));
            ctx.state = new_state;
        }
    }

    //--------------------------------------------------------------------------
    // Timer Management
    //--------------------------------------------------------------------------

    void stop_timers() {
        if (ctx.retry_timer) {
            esp_timer_stop(ctx.retry_timer);
        }
        if (ctx.prepare_timer) {
            esp_timer_stop(ctx.prepare_timer);
        }
    }

    void start_retry_timer() {
        if (!ctx.retry_timer) return;

        esp_timer_stop(ctx.retry_timer);
        esp_err_t err = esp_timer_start_once(ctx.retry_timer, RETRY_INTERVAL_US);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to start retry timer: %s", esp_err_to_name(err));
        }
    }

    void start_prepare_timer(uint32_t duration_ms) {
        if (!ctx.prepare_timer) return;
        if (duration_ms <= 2000) return;  // Too short for prepare phase

        esp_timer_stop(ctx.prepare_timer);
        int64_t delay_us = static_cast<int64_t>(duration_ms - 2000) * 1000;
        esp_err_t err = esp_timer_start_once(ctx.prepare_timer, delay_us);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to start prepare timer: %s", esp_err_to_name(err));
        }
    }

    //--------------------------------------------------------------------------
    // Helpers
    //--------------------------------------------------------------------------

    void request_render(App_t* app) {
        if (!app) return;
        msg_request_app_render(app);
        ESP_LOGD(TAG, "Requested render for %02x%02x...", app->uuid[0], app->uuid[1]);
    }

    App_t* find_pinned_app() {
        size_t count = apps_count();
        for (size_t i = 0; i < count; i++) {
            App_t* app = apps_get_by_index(i);
            if (app && app->pinned && !app->skipped) {
                return app;
            }
        }
        return nullptr;
    }

    // Find next qualified app (has data, displayable, not skipped)
    int find_next_qualified(size_t from_idx, bool skip_current) {
        size_t count = apps_count();
        if (count == 0) return -1;

        size_t start = skip_current ? 1 : 0;
        for (size_t i = start; i <= count; i++) {
            size_t idx = (from_idx + i) % count;
            App_t* app = apps_get_by_index(idx);
            if (app && app_is_qualified(app)) {
                return static_cast<int>(idx);
            }
        }
        return -1;
    }

    // Find next non-skipped app (may not have data)
    int find_next_non_skipped(size_t from_idx, bool skip_current) {
        size_t count = apps_count();
        if (count == 0) return -1;

        size_t start = skip_current ? 1 : 0;
        for (size_t i = start; i <= count; i++) {
            size_t idx = (from_idx + i) % count;
            App_t* app = apps_get_by_index(idx);
            if (app && !app->skipped) {
                return static_cast<int>(idx);
            }
        }
        return -1;
    }

    // Request renders for next N non-skipped apps
    void prefetch_renders(size_t from_idx, size_t count_to_request) {
        size_t total = apps_count();
        size_t requested = 0;

        for (size_t i = 1; i <= total && requested < count_to_request; i++) {
            size_t idx = (from_idx + i) % total;
            App_t* app = apps_get_by_index(idx);
            if (app && !app->skipped) {
                request_render(app);
                requested++;
            }
        }
        ESP_LOGD(TAG, "Prefetched %zu renders", requested);
    }

    uint32_t now_ms() {
        return static_cast<uint32_t>(esp_timer_get_time() / 1000);
    }

    //--------------------------------------------------------------------------
    // Playback Actions
    //--------------------------------------------------------------------------

    void show_ready() {
        webp_player_play_embedded("ready", true);
    }

    void clear_screen() {
        display_clear();
    }

    void play_app(App_t* app) {
        if (!app) return;

        uint32_t duration_ms = app->display_time * 1000;
        ctx.playback_start_ms = now_ms();

        webp_player_play_app(app, duration_ms, true);
        start_prepare_timer(duration_ms);

        ESP_LOGI(TAG, "Playing app %02x%02x... (duration: %ums)",
                 app->uuid[0], app->uuid[1], duration_ms);
    }

    //--------------------------------------------------------------------------
    // State Actions
    //--------------------------------------------------------------------------

    void enter_idle() {
        stop_timers();
        ctx.pinned_app = nullptr;
        show_ready();
        transition_to(State::IDLE);
    }

    void enter_rotating_playing(size_t idx) {
        App_t* app = apps_get_by_index(idx);
        if (!app || !app_is_qualified(app)) {
            ESP_LOGW(TAG, "enter_rotating_playing: app not qualified at idx %zu", idx);
            return;
        }

        stop_timers();
        ctx.current_idx = idx;
        ctx.pinned_app = nullptr;
        play_app(app);
        transition_to(State::ROTATING_PLAYING);
    }

    void enter_rotating_waiting(size_t idx) {
        App_t* app = apps_get_by_index(idx);
        if (!app) {
            ESP_LOGW(TAG, "enter_rotating_waiting: no app at idx %zu", idx);
            enter_idle();
            return;
        }

        stop_timers();
        ctx.current_idx = idx;
        ctx.pinned_app = nullptr;

        // Request render for current and start retry timer
        request_render(app);
        start_retry_timer();
        show_ready();

        transition_to(State::ROTATING_WAITING);
    }

    void enter_single_playing(App_t* app) {
        if (!app || !app_is_qualified(app)) {
            ESP_LOGW(TAG, "enter_single_playing: app not qualified");
            return;
        }

        stop_timers();
        ctx.pinned_app = app;
        play_app(app);
        transition_to(State::SINGLE_PLAYING);
    }

    void enter_single_blank(App_t* app) {
        if (!app) {
            ESP_LOGW(TAG, "enter_single_blank: app is null");
            enter_idle();
            return;
        }

        stop_timers();
        ctx.pinned_app = app;

        request_render(app);
        start_retry_timer();
        clear_screen();

        transition_to(State::SINGLE_BLANK);
    }

    //--------------------------------------------------------------------------
    // Schedule Evaluation
    //--------------------------------------------------------------------------

    void evaluate_schedule() {
        size_t count = apps_count();

        if (count == 0) {
            ESP_LOGI(TAG, "Empty schedule");
            enter_idle();
            return;
        }

        // Check for pinned app first
        App_t* pinned = find_pinned_app();
        if (pinned) {
            ESP_LOGI(TAG, "Found pinned app");
            if (app_is_qualified(pinned)) {
                enter_single_playing(pinned);
            } else {
                enter_single_blank(pinned);
            }
            return;
        }

        // Rotating mode - find first qualified
        int idx = find_next_qualified(0, false);
        if (idx >= 0) {
            enter_rotating_playing(static_cast<size_t>(idx));
            return;
        }

        // No qualified - find first non-skipped and wait for data
        idx = find_next_non_skipped(0, false);
        if (idx >= 0) {
            enter_rotating_waiting(static_cast<size_t>(idx));
            return;
        }

        // All apps skipped
        ESP_LOGW(TAG, "All apps are skipped");
        enter_idle();
    }

    //--------------------------------------------------------------------------
    // Advance Logic (Rotating Mode)
    //--------------------------------------------------------------------------

    void advance_to_next() {
        if (ctx.state != State::ROTATING_PLAYING && ctx.state != State::ROTATING_WAITING) {
            return;
        }

        size_t count = apps_count();
        if (count == 0) {
            enter_idle();
            return;
        }

        // Find next qualified app
        int next = find_next_qualified(ctx.current_idx, true);

        if (next >= 0) {
            enter_rotating_playing(static_cast<size_t>(next));
        } else {
            // No qualified app - check if current is still the only option
            int current_check = find_next_qualified(ctx.current_idx, false);
            if (current_check >= 0 && static_cast<size_t>(current_check) == ctx.current_idx) {
                // Current is still qualified - replay it
                App_t* app = apps_get_by_index(ctx.current_idx);
                if (app) {
                    play_app(app);
                    transition_to(State::ROTATING_PLAYING);
                    return;
                }
            }

            // Find any non-skipped to wait on
            next = find_next_non_skipped(ctx.current_idx, true);
            if (next >= 0) {
                enter_rotating_waiting(static_cast<size_t>(next));
            } else {
                // Nothing available
                enter_idle();
            }
        }
    }

    //--------------------------------------------------------------------------
    // Timer Callbacks
    //--------------------------------------------------------------------------

    void retry_timer_callback(void*) {
        ESP_LOGI(TAG, "Retry timer fired");

        switch (ctx.state) {
            case State::ROTATING_WAITING: {
                // Re-request renders for non-skipped apps
                size_t count = apps_count();
                for (size_t i = 0; i < count; i++) {
                    App_t* app = apps_get_by_index(i);
                    if (app && !app->skipped) {
                        request_render(app);
                    }
                }

                // Check if any became qualified
                int idx = find_next_qualified(0, false);
                if (idx >= 0) {
                    enter_rotating_playing(static_cast<size_t>(idx));
                } else {
                    start_retry_timer();
                }
                break;
            }

            case State::SINGLE_BLANK: {
                if (ctx.pinned_app) {
                    request_render(ctx.pinned_app);
                }
                start_retry_timer();
                break;
            }

            default:
                break;
        }
    }

    void prepare_timer_callback(void*) {
        ESP_LOGD(TAG, "Prepare timer fired");

        switch (ctx.state) {
            case State::ROTATING_PLAYING:
                // Request renders for next 2 apps
                prefetch_renders(ctx.current_idx, 2);
                break;

            case State::SINGLE_PLAYING:
                // Request render for same app (refresh)
                if (ctx.pinned_app) {
                    request_render(ctx.pinned_app);
                }
                break;

            default:
                break;
        }
    }

    //--------------------------------------------------------------------------
    // WebP Player Event Handlers
    //--------------------------------------------------------------------------

    void on_playing(const webp_player_playing_evt_t* evt) {
        if (!evt) return;

        // Send "currently displaying" message for RAM apps
        if (evt->source_type == WEBP_SOURCE_RAM && evt->ram_app) {
            msg_send_currently_displaying(evt->ram_app);
        }
    }

    void on_stopped() {
        // Duration expired - decide what to do next
        switch (ctx.state) {
            case State::ROTATING_PLAYING:
                advance_to_next();
                break;

            case State::SINGLE_PLAYING:
                // Replay the pinned app
                if (ctx.pinned_app && app_is_qualified(ctx.pinned_app)) {
                    play_app(ctx.pinned_app);
                } else if (ctx.pinned_app) {
                    enter_single_blank(ctx.pinned_app);
                }
                break;

            default:
                break;
        }
    }

    void on_error(const webp_player_error_evt_t* evt) {
        ESP_LOGW(TAG, "Player error");

        switch (ctx.state) {
            case State::ROTATING_PLAYING:
                advance_to_next();
                break;

            case State::SINGLE_PLAYING:
                if (ctx.pinned_app) {
                    enter_single_blank(ctx.pinned_app);
                }
                break;

            default:
                break;
        }
    }

    void on_need_next() {
        // Player has no valid content and needs something to display
        switch (ctx.state) {
            case State::ROTATING_WAITING: {
                int idx = find_next_qualified(ctx.current_idx, false);
                if (idx >= 0) {
                    enter_rotating_playing(static_cast<size_t>(idx));
                }
                break;
            }

            case State::SINGLE_BLANK: {
                if (ctx.pinned_app && app_is_qualified(ctx.pinned_app)) {
                    enter_single_playing(ctx.pinned_app);
                }
                break;
            }

            default:
                break;
        }
    }

    void webp_player_event_handler(void*, esp_event_base_t, int32_t event_id, void* event_data) {
        switch (event_id) {
            case WEBP_PLAYER_EVT_PLAYING:
                on_playing(static_cast<webp_player_playing_evt_t*>(event_data));
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
            default:
                break;
        }
    }

}  // namespace

//------------------------------------------------------------------------------
// Public API
//------------------------------------------------------------------------------

void scheduler_init() {
    // Create retry timer
    esp_timer_create_args_t retry_args = {
        .callback = retry_timer_callback,
        .arg = nullptr,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "sched_retry",
        .skip_unhandled_events = true,
    };
    esp_timer_create(&retry_args, &ctx.retry_timer);

    // Create prepare timer
    esp_timer_create_args_t prepare_args = {
        .callback = prepare_timer_callback,
        .arg = nullptr,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "sched_prep",
        .skip_unhandled_events = true,
    };
    esp_timer_create(&prepare_args, &ctx.prepare_timer);

    // Register for webp_player events
    esp_event_handler_register(
        WEBP_PLAYER_EVENTS,
        ESP_EVENT_ANY_ID,
        webp_player_event_handler,
        nullptr
    );

    // Register for button events (A = prev, C = next)
    esp_event_handler_register(
        DAUGHTERBOARD_EVENTS,
        DAUGHTERBOARD_EVENT_BUTTON_A_PRESSED,
        [](void*, esp_event_base_t, int32_t, void*) { scheduler_prev(); },
        nullptr
    );
    esp_event_handler_register(
        DAUGHTERBOARD_EVENTS,
        DAUGHTERBOARD_EVENT_BUTTON_C_PRESSED,
        [](void*, esp_event_base_t, int32_t, void*) { scheduler_next(); },
        nullptr
    );

    ESP_LOGI(TAG, "Scheduler initialized");
}

void scheduler_start() {
    ESP_LOGI(TAG, "Scheduler started");
    // Actual work happens on connect/schedule received
}

void scheduler_stop() {
    enter_idle();
    ESP_LOGI(TAG, "Scheduler stopped");
}

bool scheduler_has_schedule() {
    return ctx.state != State::IDLE || apps_count() > 0;
}

void scheduler_on_schedule_received() {
    ESP_LOGI(TAG, "Schedule received (%zu apps)", apps_count());
    evaluate_schedule();
}

void scheduler_on_render_response(const uint8_t* uuid, bool success, bool displayable) {
    if (!uuid) return;

    App_t* app = app_find(uuid);
    if (!app) {
        ESP_LOGW(TAG, "Render response for unknown app");
        return;
    }

    ESP_LOGI(TAG, "Render response: %02x%02x... success=%d displayable=%d",
             uuid[0], uuid[1], success, displayable);

    if (!success) {
        ESP_LOGW(TAG, "Render failed for %02x%02x...", uuid[0], uuid[1]);
        return;
    }

    // Handle based on current state
    switch (ctx.state) {
        case State::ROTATING_WAITING: {
            // Check if any app is now qualified
            int idx = find_next_qualified(0, false);
            if (idx >= 0) {
                enter_rotating_playing(static_cast<size_t>(idx));
            }
            break;
        }

        case State::ROTATING_PLAYING: {
            // If current became unqualified, advance
            App_t* current = apps_get_by_index(ctx.current_idx);
            if (current && !app_is_qualified(current)) {
                advance_to_next();
            }
            break;
        }

        case State::SINGLE_BLANK: {
            // Check if pinned app is now qualified
            if (ctx.pinned_app && app_is_qualified(ctx.pinned_app)) {
                enter_single_playing(ctx.pinned_app);
            }
            break;
        }

        case State::SINGLE_PLAYING: {
            // If pinned app became not displayable, go to blank
            if (ctx.pinned_app && !displayable) {
                enter_single_blank(ctx.pinned_app);
            }
            break;
        }

        default:
            break;
    }
}

void scheduler_on_pin_state_changed(const uint8_t* uuid, bool pinned) {
    if (!uuid) return;

    ESP_LOGI(TAG, "Pin state changed: %02x%02x... pinned=%d", uuid[0], uuid[1], pinned);

    // Re-evaluate the entire schedule to handle mode switch
    evaluate_schedule();
}

void scheduler_on_connect() {
    ctx.connected = true;
    webp_player_set_display_mode(true);
    msg_send_schedule_request();
    ESP_LOGI(TAG, "Connected - requesting schedule");
}

void scheduler_on_disconnect() {
    ctx.connected = false;
    stop_timers();
    ctx.pinned_app = nullptr;

    webp_player_set_display_mode(false);
    webp_player_play_embedded("connecting", true);

    transition_to(State::IDLE);
    ESP_LOGI(TAG, "Disconnected - showing connecting sprite");
}

const uint8_t* scheduler_get_current_uuid() {
    switch (ctx.state) {
        case State::SINGLE_PLAYING:
        case State::SINGLE_BLANK:
            return ctx.pinned_app ? ctx.pinned_app->uuid : nullptr;

        case State::ROTATING_PLAYING:
        case State::ROTATING_WAITING: {
            App_t* app = apps_get_by_index(ctx.current_idx);
            return app ? app->uuid : nullptr;
        }

        default:
            return nullptr;
    }
}

void scheduler_next() {
    // Only works in rotating mode
    if (ctx.state != State::ROTATING_PLAYING && ctx.state != State::ROTATING_WAITING) {
        return;
    }

    size_t count = apps_count();
    if (count == 0) return;

    int next = find_next_qualified(ctx.current_idx, true);
    if (next >= 0) {
        enter_rotating_playing(static_cast<size_t>(next));
        ESP_LOGI(TAG, "Button: next -> index %d", next);
    }
}

void scheduler_prev() {
    // Only works in rotating mode
    if (ctx.state != State::ROTATING_PLAYING && ctx.state != State::ROTATING_WAITING) {
        return;
    }

    size_t count = apps_count();
    if (count == 0) return;

    // Search backwards for previous qualified
    for (size_t i = 1; i <= count; i++) {
        size_t idx = (ctx.current_idx + count - i) % count;
        App_t* app = apps_get_by_index(idx);
        if (app && app_is_qualified(app)) {
            enter_rotating_playing(idx);
            ESP_LOGI(TAG, "Button: prev -> index %zu", idx);
            return;
        }
    }
}

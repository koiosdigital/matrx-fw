#include "scheduler.h"

#include <cstring>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <esp_event.h>

#include "raii_utils.hpp"

#include "apps.h"
#include "webp_player.h"
#include "display.h"
#include "messages.h"
#include "render_fetch.h"
#include "sockets.h"
#include "daughterboard.h"

static const char* TAG = "scheduler";

namespace {

    constexpr int64_t RETRY_INTERVAL_US = 10 * 1000 * 1000;
    constexpr int64_t PREPARE_BEFORE_US = 2 * 1000 * 1000;

    enum class State {
        IDLE,
        ROTATING_PLAYING,
        ROTATING_WAITING,
        SINGLE_PLAYING,
        SINGLE_BLANK,
    };

    struct Context {
        State state = State::IDLE;
        size_t current_idx = 0;
        App_t* pinned_app = nullptr;
        esp_timer_handle_t prepare_timer = nullptr;
        esp_timer_handle_t retry_timer = nullptr;
        uint32_t playback_start_ms = 0;
        SemaphoreHandle_t mutex = nullptr;
        bool paused = false;  // quiet hours: pipeline suspended, events ignored
    };

    Context ctx;

    void transition_to(State new_state) {
        if (ctx.state != new_state) {
            ctx.state = new_state;
        }
    }

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
        if (duration_ms <= 2000) return;

        esp_timer_stop(ctx.prepare_timer);
        int64_t delay_us = static_cast<int64_t>(duration_ms - 2000) * 1000;
        esp_err_t err = esp_timer_start_once(ctx.prepare_timer, delay_us);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to start prepare timer: %s", esp_err_to_name(err));
        }
    }

    void request_render(App_t* app) {
        if (!app) return;
        render_fetch_request(app->uuid);
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

    int find_next_qualified(size_t from_idx, bool skip_current) {
        size_t count = apps_count();
        if (count == 0) return -1;

        size_t start = skip_current ? 1 : 0;
        for (size_t i = start; i < count; i++) {
            size_t idx = (from_idx + i) % count;
            App_t* app = apps_get_by_index(idx);
            if (app && app_is_qualified(app)) {
                return static_cast<int>(idx);
            }
        }
        return -1;
    }

    int find_next_non_skipped(size_t from_idx, bool skip_current) {
        size_t count = apps_count();
        if (count == 0) return -1;

        size_t start = skip_current ? 1 : 0;
        for (size_t i = start; i < count; i++) {
            size_t idx = (from_idx + i) % count;
            App_t* app = apps_get_by_index(idx);
            if (app && !app->skipped) {
                return static_cast<int>(idx);
            }
        }
        return -1;
    }

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
    }

    uint32_t now_ms() {
        return static_cast<uint32_t>(esp_timer_get_time() / 1000);
    }

    void show_ready() {
        webp_player_play_embedded("ready");
    }

    void clear_screen() {
        display_clear();
    }

    void play_app(App_t* app) {
        if (!app) return;

        uint32_t duration_ms = app->display_time * 1000;
        ctx.playback_start_ms = now_ms();

        webp_player_play_app(app, duration_ms);
        start_prepare_timer(duration_ms);
    }

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
        webp_player_stop();
        clear_screen();

        transition_to(State::SINGLE_BLANK);
    }

    void evaluate_schedule() {
        if (ctx.paused) return;

        size_t count = apps_count();

        if (count == 0) {
            enter_idle();
            return;
        }

        App_t* pinned = find_pinned_app();
        if (pinned) {
            if (app_is_qualified(pinned)) {
                enter_single_playing(pinned);
            } else {
                enter_single_blank(pinned);
            }
            return;
        }

        if (count == 1) {
            App_t* app = apps_get_by_index(0);
            if (app && !app->skipped) {
                if (app_is_qualified(app)) {
                    enter_single_playing(app);
                } else {
                    enter_single_blank(app);
                }
                return;
            }
        }

        int idx = find_next_qualified(0, false);
        if (idx >= 0) {
            enter_rotating_playing(static_cast<size_t>(idx));
            return;
        }

        idx = find_next_non_skipped(0, false);
        if (idx >= 0) {
            enter_rotating_waiting(static_cast<size_t>(idx));
            return;
        }

        ESP_LOGW(TAG, "All apps are skipped");
        enter_idle();
    }

    void advance_to_next() {
        if (ctx.state != State::ROTATING_PLAYING && ctx.state != State::ROTATING_WAITING) {
            return;
        }

        size_t count = apps_count();
        if (count == 0) {
            enter_idle();
            return;
        }

        int next = find_next_qualified(ctx.current_idx, true);

        if (next >= 0) {
            enter_rotating_playing(static_cast<size_t>(next));
        } else {
            int current_check = find_next_qualified(ctx.current_idx, false);
            if (current_check >= 0 && static_cast<size_t>(current_check) == ctx.current_idx) {
                App_t* app = apps_get_by_index(ctx.current_idx);
                if (app) {
                    play_app(app);
                    transition_to(State::ROTATING_PLAYING);
                    return;
                }
            }

            next = find_next_non_skipped(ctx.current_idx, true);
            if (next >= 0) {
                enter_rotating_waiting(static_cast<size_t>(next));
            } else {
                enter_idle();
            }
        }
    }

    void retry_timer_callback(void*) {
        raii::MutexGuard lock(ctx.mutex, pdMS_TO_TICKS(100));
        if (!lock) return;
        if (ctx.paused) return;

        switch (ctx.state) {
            case State::ROTATING_WAITING: {
                size_t count = apps_count();
                for (size_t i = 0; i < count; i++) {
                    App_t* app = apps_get_by_index(i);
                    if (app && !app->skipped) {
                        request_render(app);
                    }
                }

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
        raii::MutexGuard lock(ctx.mutex, pdMS_TO_TICKS(100));
        if (!lock) return;
        if (ctx.paused) return;

        switch (ctx.state) {
            case State::ROTATING_PLAYING:
                prefetch_renders(ctx.current_idx, 2);
                break;

            case State::SINGLE_PLAYING:
                if (ctx.pinned_app) {
                    request_render(ctx.pinned_app);
                }
                break;

            default:
                break;
        }
    }

    void on_playing(const webp_player_playing_evt_t* evt) {
        if (!evt) return;

        if (evt->source_type == WEBP_SOURCE_RAM && evt->ram_app) {
            msg_send_currently_displaying(evt->ram_app);
        }
    }

    void on_stopped() {
        if (ctx.paused) return;

        switch (ctx.state) {
            case State::ROTATING_PLAYING:
                advance_to_next();
                break;

            case State::SINGLE_PLAYING:
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
        if (ctx.paused) return;

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
            default:
                break;
        }
    }

}  // namespace

void scheduler_init() {
    ctx.mutex = xSemaphoreCreateMutex();

    esp_timer_create_args_t retry_args = {
        .callback = retry_timer_callback,
        .arg = nullptr,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "sched_retry",
        .skip_unhandled_events = true,
    };
    esp_timer_create(&retry_args, &ctx.retry_timer);

    esp_timer_create_args_t prepare_args = {
        .callback = prepare_timer_callback,
        .arg = nullptr,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "sched_prep",
        .skip_unhandled_events = true,
    };
    esp_timer_create(&prepare_args, &ctx.prepare_timer);

    esp_event_handler_register(
        WEBP_PLAYER_EVENTS,
        ESP_EVENT_ANY_ID,
        webp_player_event_handler,
        nullptr
    );

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
}

void scheduler_start() {
}

void scheduler_stop() {
    enter_idle();
}

void scheduler_pause() {
    raii::MutexGuard lock(ctx.mutex, pdMS_TO_TICKS(100));
    if (!lock || ctx.paused) return;

    ctx.paused = true;
    stop_timers();
    webp_player_stop();   // stop decoding -> player task goes idle (~0 CPU)
    clear_screen();       // blank the framebuffer
    transition_to(State::IDLE);
}

void scheduler_resume() {
    raii::MutexGuard lock(ctx.mutex, pdMS_TO_TICKS(100));
    if (!lock || !ctx.paused) return;

    ctx.paused = false;
    evaluate_schedule();  // re-derive playback from the current schedule
}

void scheduler_deinit() {
    enter_idle();

    esp_event_handler_unregister(WEBP_PLAYER_EVENTS, ESP_EVENT_ANY_ID, webp_player_event_handler);
    esp_event_handler_unregister(DAUGHTERBOARD_EVENTS, DAUGHTERBOARD_EVENT_BUTTON_A_PRESSED, nullptr);
    esp_event_handler_unregister(DAUGHTERBOARD_EVENTS, DAUGHTERBOARD_EVENT_BUTTON_C_PRESSED, nullptr);

    if (ctx.retry_timer) {
        esp_timer_stop(ctx.retry_timer);
        esp_timer_delete(ctx.retry_timer);
        ctx.retry_timer = nullptr;
    }
    if (ctx.prepare_timer) {
        esp_timer_stop(ctx.prepare_timer);
        esp_timer_delete(ctx.prepare_timer);
        ctx.prepare_timer = nullptr;
    }
    if (ctx.mutex) {
        vSemaphoreDelete(ctx.mutex);
        ctx.mutex = nullptr;
    }
}

bool scheduler_has_schedule() {
    return ctx.state != State::IDLE || apps_count() > 0;
}

void scheduler_on_schedule_received() {
    evaluate_schedule();
}

void scheduler_on_render_response(const uint8_t* uuid, bool success, bool displayable) {
    if (!uuid) return;
    if (ctx.paused) return;

    App_t* app = app_find(uuid);
    if (!app) {
        ESP_LOGW(TAG, "Render response for unknown app");
        return;
    }

    if (!success) {
        ESP_LOGW(TAG, "Render failed for %02x%02x...", uuid[0], uuid[1]);
        return;
    }

    switch (ctx.state) {
        case State::ROTATING_WAITING: {
            int idx = find_next_qualified(0, false);
            if (idx >= 0) {
                enter_rotating_playing(static_cast<size_t>(idx));
            }
            break;
        }

        case State::ROTATING_PLAYING: {
            App_t* current = apps_get_by_index(ctx.current_idx);
            if (current && !app_is_qualified(current)) {
                advance_to_next();
            }
            break;
        }

        case State::SINGLE_BLANK: {
            if (ctx.pinned_app && app_is_qualified(ctx.pinned_app)) {
                enter_single_playing(ctx.pinned_app);
            }
            break;
        }

        case State::SINGLE_PLAYING: {
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

    evaluate_schedule();
}

void scheduler_on_connect() {
    msg_send_schedule_request();
}

void scheduler_on_disconnect() {
    stop_timers();
    ctx.pinned_app = nullptr;

    // While paused for quiet hours the screen stays off; don't relight it with
    // the "connecting" sprite.
    if (!ctx.paused) {
        webp_player_play_embedded("connecting");
    }

    transition_to(State::IDLE);
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
    if (ctx.paused) return;
    if (ctx.state != State::ROTATING_PLAYING && ctx.state != State::ROTATING_WAITING) {
        return;
    }

    size_t count = apps_count();
    if (count == 0) return;

    int next = find_next_qualified(ctx.current_idx, true);
    if (next >= 0) {
        enter_rotating_playing(static_cast<size_t>(next));
    }
}

void scheduler_prev() {
    if (ctx.paused) return;
    if (ctx.state != State::ROTATING_PLAYING && ctx.state != State::ROTATING_WAITING) {
        return;
    }

    size_t count = apps_count();
    if (count == 0) return;

    for (size_t i = 1; i < count; i++) {
        size_t idx = (ctx.current_idx + count - i) % count;
        App_t* app = apps_get_by_index(idx);
        if (app && app_is_qualified(app)) {
            enter_rotating_playing(idx);
            return;
        }
    }
}

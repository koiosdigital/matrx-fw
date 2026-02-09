// WebP Player - Event-driven animated WebP playback task
#include "webp_player.h"
#include "display.h"
#include "static_files.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>

#include <esp_log.h>
#include <esp_heap_caps.h>
#include <webp/demux.h>

#include <cstring>
#include <atomic>

static const char* TAG = "webp_player";

ESP_EVENT_DEFINE_BASE(WEBP_PLAYER_EVENTS);

namespace {

    //------------------------------------------------------------------------------
    // Player State
    //------------------------------------------------------------------------------

    enum class PlayerState : uint8_t {
        IDLE,
        PLAYING,
        PAUSED,
    };

    //------------------------------------------------------------------------------
    // Command Types
    //------------------------------------------------------------------------------

    enum class CommandType : uint8_t {
        PLAY,
        SET_NEXT,
        STOP,
        PAUSE,
        RESUME,
    };

    struct PlayParams {
        webp_source_type_t source_type;
        App_t* ram_app;
        const char* embedded_name;
        uint32_t duration_ms;
        bool immediate;
    };

    struct Command {
        CommandType type;
        PlayParams play;  // Valid for PLAY and SET_NEXT
    };

    //------------------------------------------------------------------------------
    // Playback Info
    //------------------------------------------------------------------------------

    struct PlaybackInfo {
        webp_source_type_t source_type = WEBP_SOURCE_RAM;
        App_t* ram_app = nullptr;
        const char* embedded_name = nullptr;

        // WebP data (copied for RAM apps, direct for embedded)
        uint8_t* webp_buffer = nullptr;     // Owned copy for RAM apps (reused across playbacks)
        size_t webp_buffer_capacity = 0;    // Allocated size of webp_buffer
        const uint8_t* webp_bytes = nullptr;
        size_t webp_size = 0;               // Actual size of current WebP data

        // Duration tracking
        uint32_t requested_duration_ms = 0;
        TickType_t playback_start_tick = 0;
        uint32_t loops_completed = 0;

        // Animation info
        uint32_t frame_count = 0;
        uint32_t loop_duration_ms = 0;

        // Frame timing
        int last_frame_timestamp = 0;
        TickType_t frame_tick = 0;

        // Flags
        bool prepare_next_sent = false;

        void reset() {
            source_type = WEBP_SOURCE_RAM;
            ram_app = nullptr;
            embedded_name = nullptr;
            // DON'T free webp_buffer here - keep for reuse across playbacks
            // Buffer is only freed in webp_player_deinit() or when resizing
            webp_bytes = nullptr;
            webp_size = 0;
            // Keep webp_buffer and webp_buffer_capacity intact for reuse
            requested_duration_ms = 0;
            playback_start_tick = 0;
            loops_completed = 0;
            frame_count = 0;
            loop_duration_ms = 0;
            last_frame_timestamp = 0;
            frame_tick = 0;
            prepare_next_sent = false;
        }

        void free_buffer() {
            if (webp_buffer) {
                heap_caps_free(webp_buffer);
                webp_buffer = nullptr;
            }
            webp_buffer_capacity = 0;
        }
    };

    //------------------------------------------------------------------------------
    // Queued Next App
    //------------------------------------------------------------------------------

    struct QueuedApp {
        bool valid = false;
        webp_source_type_t source_type = WEBP_SOURCE_RAM;
        App_t* ram_app = nullptr;
        const char* embedded_name = nullptr;
        uint32_t duration_ms = 0;

        void set(const PlayParams* params) {
            valid = true;
            source_type = params->source_type;
            ram_app = params->ram_app;
            embedded_name = params->embedded_name;
            duration_ms = params->duration_ms;
        }

        void clear() {
            valid = false;
            ram_app = nullptr;
            embedded_name = nullptr;
            duration_ms = 0;
        }
    };

    //------------------------------------------------------------------------------
    // Player Context
    //------------------------------------------------------------------------------

    struct PlayerContext {
        TaskHandle_t task = nullptr;
        QueueHandle_t cmd_queue = nullptr;
        SemaphoreHandle_t decoder_mutex = nullptr;

        std::atomic<PlayerState> state{ PlayerState::IDLE };
        PlaybackInfo current;
        QueuedApp next;

        // WebP decoder
        WebPAnimDecoder* decoder = nullptr;
        WebPAnimInfo anim_info = {};
        WebPData webp_data = { nullptr, 0 };

        // Error tracking
        int decode_error_count = 0;

        // Display mode - when true, NEED_NEXT events are emitted
        // Set by scheduler when sockets connect/disconnect
        std::atomic<bool> display_mode{ false };

        // NEED_NEXT tracking - emitted when RAM app is invalid
        bool need_next_pending = false;
        TickType_t last_need_next_tick = 0;
    };

    PlayerContext player;

    //------------------------------------------------------------------------------
    // Helper: ticks to ms
    //------------------------------------------------------------------------------

    inline uint32_t ticks_to_ms(TickType_t ticks) {
        return static_cast<uint32_t>(ticks * portTICK_PERIOD_MS);
    }

    //------------------------------------------------------------------------------
    // Decoder Management
    //------------------------------------------------------------------------------

    void destroy_decoder() {
        if (player.decoder) {
            xSemaphoreTake(player.decoder_mutex, portMAX_DELAY);
            WebPAnimDecoderDelete(player.decoder);
            player.decoder = nullptr;
            xSemaphoreGive(player.decoder_mutex);
        }
    }

    esp_err_t create_decoder() {
        destroy_decoder();

        if (!player.current.webp_bytes || player.current.webp_size == 0) {
            ESP_LOGE(TAG, "No WebP data");
            return ESP_ERR_INVALID_ARG;
        }

        xSemaphoreTake(player.decoder_mutex, portMAX_DELAY);

        player.webp_data.bytes = player.current.webp_bytes;
        player.webp_data.size = player.current.webp_size;

        player.decoder = WebPAnimDecoderNew(&player.webp_data, nullptr);
        if (!player.decoder) {
            xSemaphoreGive(player.decoder_mutex);
            ESP_LOGE(TAG, "Failed to create decoder");
            return ESP_FAIL;
        }

        if (!WebPAnimDecoderGetInfo(player.decoder, &player.anim_info)) {
            WebPAnimDecoderDelete(player.decoder);
            player.decoder = nullptr;
            xSemaphoreGive(player.decoder_mutex);
            ESP_LOGE(TAG, "Failed to get anim info");
            return ESP_FAIL;
        }

        player.current.frame_count = player.anim_info.frame_count;

        // Calculate loop duration by iterating through all frames
        if (player.anim_info.frame_count > 1) {
            uint8_t* buf;
            int timestamp = 0;
            while (WebPAnimDecoderHasMoreFrames(player.decoder)) {
                if (!WebPAnimDecoderGetNext(player.decoder, &buf, &timestamp)) break;
            }
            player.current.loop_duration_ms = static_cast<uint32_t>(timestamp);
            WebPAnimDecoderReset(player.decoder);
        }
        else {
            player.current.loop_duration_ms = 0;
        }

        xSemaphoreGive(player.decoder_mutex);

        ESP_LOGI(TAG, "Decoder created: %u frames, loop %u ms",
            player.current.frame_count, player.current.loop_duration_ms);
        return ESP_OK;
    }

    //------------------------------------------------------------------------------
    // Event Emission
    //------------------------------------------------------------------------------

    void emit_playing_event() {
        webp_player_playing_evt_t evt = {};
        evt.source_type = player.current.source_type;
        evt.ram_app = player.current.ram_app;
        evt.embedded_name = player.current.embedded_name;
        evt.expected_duration_ms = player.current.requested_duration_ms;
        evt.loop_duration_ms = player.current.loop_duration_ms;
        evt.frame_count = player.current.frame_count;

        esp_event_post(WEBP_PLAYER_EVENTS, WEBP_PLAYER_EVT_PLAYING,
            &evt, sizeof(evt), 0);
    }

    void emit_error_event(int error_code) {
        webp_player_error_evt_t evt = {};
        evt.source_type = player.current.source_type;
        evt.ram_app = player.current.ram_app;
        evt.embedded_name = player.current.embedded_name;
        evt.error_code = error_code;

        esp_event_post(WEBP_PLAYER_EVENTS, WEBP_PLAYER_EVT_ERROR,
            &evt, sizeof(evt), 0);
    }

    void emit_prepare_next_event(uint32_t remaining_ms) {
        webp_player_prepare_next_evt_t evt = {};
        evt.source_type = player.current.source_type;
        evt.ram_app = player.current.ram_app;
        evt.embedded_name = player.current.embedded_name;
        evt.remaining_ms = remaining_ms;

        esp_event_post(WEBP_PLAYER_EVENTS, WEBP_PLAYER_EVT_PREPARE_NEXT,
            &evt, sizeof(evt), 0);
    }

    void emit_stopped_event() {
        esp_event_post(WEBP_PLAYER_EVENTS, WEBP_PLAYER_EVT_STOPPED,
            nullptr, 0, 0);
    }

    void emit_need_next_event() {
        esp_event_post(WEBP_PLAYER_EVENTS, WEBP_PLAYER_EVT_NEED_NEXT,
            nullptr, 0, 0);
    }

    //------------------------------------------------------------------------------
    // Duration Logic
    //------------------------------------------------------------------------------

    bool should_continue_playing() {
        // Embedded sprites loop forever
        if (player.current.source_type == WEBP_SOURCE_EMBEDDED) {
            return true;
        }

        // Unlimited duration
        if (player.current.requested_duration_ms == 0) {
            return true;
        }

        uint32_t elapsed = ticks_to_ms(xTaskGetTickCount() - player.current.playback_start_tick);

        // Animated or static: keep playing until display_time is reached
        // This naturally handles looping - after each loop completes,
        // check if we should start another one
        return elapsed < player.current.requested_duration_ms;
    }

    void check_prepare_next() {
        // Don't emit for embedded (loops forever)
        if (player.current.source_type == WEBP_SOURCE_EMBEDDED) {
            return;
        }

        // Already sent
        if (player.current.prepare_next_sent) {
            return;
        }

        // Unlimited duration
        if (player.current.requested_duration_ms == 0) {
            return;
        }

        uint32_t elapsed = ticks_to_ms(xTaskGetTickCount() - player.current.playback_start_tick);
        uint32_t remaining = 0;

        if (elapsed < player.current.requested_duration_ms) {
            remaining = player.current.requested_duration_ms - elapsed;
        }

        if (remaining <= WEBP_PLAYER_PREPARE_NEXT_MS) {
            emit_prepare_next_event(remaining);
            player.current.prepare_next_sent = true;
            ESP_LOGI(TAG, "PREPARE_NEXT emitted, %u ms remaining", remaining);
        }
    }

    //------------------------------------------------------------------------------
    // Playback Start
    //------------------------------------------------------------------------------

    esp_err_t start_playback(const PlayParams* params) {
        player.current.reset();
        player.decode_error_count = 0;

        player.current.source_type = params->source_type;
        player.current.requested_duration_ms = params->duration_ms;

        if (params->source_type == WEBP_SOURCE_RAM) {
            player.current.ram_app = params->ram_app;
            if (!params->ram_app || !params->ram_app->data || params->ram_app->len == 0) {
                ESP_LOGE(TAG, "Invalid RAM app");
                return ESP_ERR_INVALID_ARG;
            }

            size_t needed_size = params->ram_app->len;

            // Reuse existing buffer if it fits, otherwise resize
            if (player.current.webp_buffer == nullptr || player.current.webp_buffer_capacity < needed_size) {
                // Need larger buffer - free old and allocate new
                if (player.current.webp_buffer) {
                    heap_caps_free(player.current.webp_buffer);
                }
                player.current.webp_buffer = static_cast<uint8_t*>(
                    heap_caps_malloc(needed_size, MALLOC_CAP_SPIRAM));
                if (!player.current.webp_buffer) {
                    ESP_LOGE(TAG, "Failed to alloc %zu bytes", needed_size);
                    player.current.webp_buffer_capacity = 0;
                    return ESP_ERR_NO_MEM;
                }
                player.current.webp_buffer_capacity = needed_size;
            }
            // else: reuse existing buffer (it's large enough)

            // Lock app and copy
            xSemaphoreTake(params->ram_app->mutex, portMAX_DELAY);
            std::memcpy(player.current.webp_buffer, params->ram_app->data, needed_size);
            player.current.webp_size = needed_size;
            xSemaphoreGive(params->ram_app->mutex);

            player.current.webp_bytes = player.current.webp_buffer;
        }
        else {
            player.current.embedded_name = params->embedded_name;
            const uint8_t* data = nullptr;
            size_t len = 0;

            if (get_image_data(params->embedded_name, &data, &len) == 0 || !data || len == 0) {
                ESP_LOGE(TAG, "Embedded sprite not found: %s", params->embedded_name);
                return ESP_ERR_NOT_FOUND;
            }

            player.current.webp_bytes = data;
            player.current.webp_size = len;
            // Embedded sprites have unlimited duration (loop forever)
            player.current.requested_duration_ms = 0;
        }

        esp_err_t err = create_decoder();
        if (err != ESP_OK) {
            player.current.reset();
            return err;
        }

        player.current.playback_start_tick = xTaskGetTickCount();
        player.current.frame_tick = player.current.playback_start_tick;
        player.state.store(PlayerState::PLAYING);

        emit_playing_event();
        ESP_LOGI(TAG, "Playback started: %s, duration %u ms",
            params->source_type == WEBP_SOURCE_RAM ? "RAM app" : params->embedded_name,
            player.current.requested_duration_ms);

        return ESP_OK;
    }

    //------------------------------------------------------------------------------
    // Transition to Next or Idle
    //------------------------------------------------------------------------------

    void transition_to_next_or_idle() {
        destroy_decoder();
        player.current.reset();

        if (player.next.valid) {
            PlayParams params = {};
            params.source_type = player.next.source_type;
            params.ram_app = player.next.ram_app;
            params.embedded_name = player.next.embedded_name;
            params.duration_ms = player.next.duration_ms;
            params.immediate = true;

            player.next.clear();

            if (start_playback(&params) == ESP_OK) {
                return;
            }
            // Fall through to IDLE if start failed
        }

        player.state.store(PlayerState::IDLE);
        // Don't clear display - keep last frame visible until next content is ready
        emit_stopped_event();
        ESP_LOGI(TAG, "Playback stopped, going idle");
    }

    //------------------------------------------------------------------------------
    // Handle Decode Error
    //------------------------------------------------------------------------------

    void handle_decode_error() {
        player.decode_error_count++;
        ESP_LOGW(TAG, "Decode error %d/%d", player.decode_error_count, WEBP_PLAYER_RETRY_COUNT);

        if (player.decode_error_count >= WEBP_PLAYER_RETRY_COUNT) {
            ESP_LOGE(TAG, "Max retries reached, giving up");
            emit_error_event(ESP_FAIL);
            transition_to_next_or_idle();
            return;
        }

        // Retry: recreate decoder
        vTaskDelay(pdMS_TO_TICKS(WEBP_PLAYER_RETRY_DELAY_MS));
        if (create_decoder() != ESP_OK) {
            player.decode_error_count = WEBP_PLAYER_RETRY_COUNT;  // Force failure
            handle_decode_error();
        }
    }

    //------------------------------------------------------------------------------
    // Command Handlers
    //------------------------------------------------------------------------------

    void handle_play_command(const PlayParams* params) {
        PlayerState current_state = player.state.load();

        ESP_LOGI(TAG, "Play command: source=%s, immediate=%d, state=%d",
            params->source_type == WEBP_SOURCE_RAM ? "RAM" : "embedded",
            params->immediate, static_cast<int>(current_state));

        if (params->immediate || current_state == PlayerState::IDLE) {
            // Stop current and start new
            destroy_decoder();
            player.current.reset();
            player.next.clear();

            esp_err_t err = start_playback(params);
            if (err == ESP_OK) {
                // Success - clear need_next state
                player.need_next_pending = false;
                ESP_LOGI(TAG, "Playback started successfully");
            } else {
                ESP_LOGE(TAG, "start_playback failed: %s", esp_err_to_name(err));
                player.state.store(PlayerState::IDLE);
                display_clear();
                // If this was a RAM app that failed and we're in display mode, request next
                if (params->source_type == WEBP_SOURCE_RAM && player.display_mode.load()) {
                    player.need_next_pending = true;
                    player.last_need_next_tick = xTaskGetTickCount();
                    emit_need_next_event();
                    ESP_LOGI(TAG, "Need next app (invalid RAM app)");
                }
            }
        }
        else {
            // Queue as next
            player.next.set(params);
            ESP_LOGI(TAG, "Queued next app (player busy)");
        }
    }

    void handle_set_next_command(const PlayParams* params) {
        player.next.set(params);
        ESP_LOGI(TAG, "Set next app");
    }

    void handle_stop_command() {
        destroy_decoder();
        player.current.reset();
        player.next.clear();
        player.state.store(PlayerState::IDLE);
        display_clear();
        emit_stopped_event();
        ESP_LOGI(TAG, "Stopped");
    }

    void handle_pause_command() {
        if (player.state.load() == PlayerState::PLAYING) {
            player.state.store(PlayerState::PAUSED);
            ESP_LOGI(TAG, "Paused");
        }
    }

    void handle_resume_command() {
        if (player.state.load() == PlayerState::PAUSED) {
            player.current.frame_tick = xTaskGetTickCount();
            player.state.store(PlayerState::PLAYING);
            ESP_LOGI(TAG, "Resumed");
        }
    }

    void process_commands() {
        Command cmd;
        while (xQueueReceive(player.cmd_queue, &cmd, 0) == pdTRUE) {
            switch (cmd.type) {
            case CommandType::PLAY:
                handle_play_command(&cmd.play);
                break;
            case CommandType::SET_NEXT:
                handle_set_next_command(&cmd.play);
                break;
            case CommandType::STOP:
                handle_stop_command();
                break;
            case CommandType::PAUSE:
                handle_pause_command();
                break;
            case CommandType::RESUME:
                handle_resume_command();
                break;
            }
        }
    }

    //------------------------------------------------------------------------------
    // Playback Iteration
    //------------------------------------------------------------------------------

    void playback_iteration() {
        if (!xSemaphoreTake(player.decoder_mutex, pdMS_TO_TICKS(50))) {
            return;
        }

        if (!player.decoder) {
            xSemaphoreGive(player.decoder_mutex);
            return;
        }

        // Check for loop completion
        if (!WebPAnimDecoderHasMoreFrames(player.decoder)) {
            player.current.loops_completed++;

            if (!should_continue_playing()) {
                xSemaphoreGive(player.decoder_mutex);
                transition_to_next_or_idle();
                return;
            }

            WebPAnimDecoderReset(player.decoder);
            player.current.last_frame_timestamp = 0;
        }

        // Get next frame
        uint8_t* frame_buffer = nullptr;
        int timestamp = 0;

        if (!WebPAnimDecoderGetNext(player.decoder, &frame_buffer, &timestamp)) {
            xSemaphoreGive(player.decoder_mutex);
            handle_decode_error();
            return;
        }

        xSemaphoreGive(player.decoder_mutex);

        if (!frame_buffer) {
            handle_decode_error();
            return;
        }

        // Reset error count on successful decode
        player.decode_error_count = 0;

        // Render frame
        display_render_rgba_frame(frame_buffer, CONFIG_MATRIX_WIDTH, CONFIG_MATRIX_HEIGHT);

        // Check if PREPARE_NEXT should be sent
        check_prepare_next();

        // Calculate delay
        int delay_ms = timestamp - player.current.last_frame_timestamp;
        player.current.last_frame_timestamp = timestamp;

        // Static images: hold for the remaining display time
        if (player.current.frame_count == 1) {
            uint32_t elapsed = ticks_to_ms(xTaskGetTickCount() - player.current.playback_start_tick);
            if (player.current.requested_duration_ms > 0 && elapsed < player.current.requested_duration_ms) {
                // Sleep for remaining time (capped to avoid overflow)
                uint32_t remaining = player.current.requested_duration_ms - elapsed;
                delay_ms = static_cast<int>(remaining > 60000 ? 60000 : remaining);
            } else {
                delay_ms = 100;  // Default for unlimited duration
            }
        }

        if (delay_ms > 0) {
            vTaskDelayUntil(&player.current.frame_tick, pdMS_TO_TICKS(delay_ms));
        }
    }

    //------------------------------------------------------------------------------
    // Player Task
    //------------------------------------------------------------------------------

    void player_task(void*) {
        ESP_LOGI(TAG, "Player task started");

        while (true) {
            // Process any pending commands
            process_commands();

            PlayerState state = player.state.load();

            switch (state) {
            case PlayerState::IDLE:
                // Check if we need to emit NEED_NEXT periodically (only in display mode)
                if (player.need_next_pending && player.display_mode.load()) {
                    TickType_t now = xTaskGetTickCount();
                    if ((now - player.last_need_next_tick) >= pdMS_TO_TICKS(WEBP_PLAYER_NEED_NEXT_MS)) {
                        emit_need_next_event();
                        player.last_need_next_tick = now;
                        ESP_LOGD(TAG, "Need next app (periodic)");
                    }
                }
                vTaskDelay(pdMS_TO_TICKS(50));
                break;

            case PlayerState::PLAYING:
                playback_iteration();
                break;

            case PlayerState::PAUSED:
                // Just process commands, don't render
                vTaskDelay(pdMS_TO_TICKS(50));
                break;
            }
        }
    }

    //------------------------------------------------------------------------------
    // Command Queue Helpers
    //------------------------------------------------------------------------------

    esp_err_t send_command(const Command* cmd) {
        if (!player.cmd_queue) {
            return ESP_ERR_INVALID_STATE;
        }

        if (xQueueSend(player.cmd_queue, cmd, pdMS_TO_TICKS(100)) != pdTRUE) {
            ESP_LOGW(TAG, "Command queue full");
            return ESP_ERR_TIMEOUT;
        }

        return ESP_OK;
    }

}  // namespace

//------------------------------------------------------------------------------
// Public API
//------------------------------------------------------------------------------

esp_err_t webp_player_init() {
    if (player.task) {
        ESP_LOGW(TAG, "Already initialized");
        return ESP_OK;
    }

    player.cmd_queue = xQueueCreate(WEBP_PLAYER_CMD_QUEUE_SIZE, sizeof(Command));
    if (!player.cmd_queue) {
        ESP_LOGE(TAG, "Failed to create command queue");
        return ESP_ERR_NO_MEM;
    }

    player.decoder_mutex = xSemaphoreCreateMutex();
    if (!player.decoder_mutex) {
        vQueueDelete(player.cmd_queue);
        player.cmd_queue = nullptr;
        ESP_LOGE(TAG, "Failed to create decoder mutex");
        return ESP_ERR_NO_MEM;
    }

    BaseType_t ret = xTaskCreatePinnedToCore(
        player_task,
        "webp_player",
        WEBP_PLAYER_TASK_STACK_SIZE,
        nullptr,
        WEBP_PLAYER_TASK_PRIORITY,
        &player.task,
        WEBP_PLAYER_TASK_CORE
    );

    if (ret != pdPASS) {
        vSemaphoreDelete(player.decoder_mutex);
        vQueueDelete(player.cmd_queue);
        player.decoder_mutex = nullptr;
        player.cmd_queue = nullptr;
        ESP_LOGE(TAG, "Failed to create player task");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "WebP player initialized");
    return ESP_OK;
}

void webp_player_deinit() {
    if (player.task) {
        vTaskDelete(player.task);
        player.task = nullptr;
    }

    destroy_decoder();
    player.current.reset();
    player.current.free_buffer();  // Free the persistent buffer on deinit
    player.next.clear();

    if (player.decoder_mutex) {
        vSemaphoreDelete(player.decoder_mutex);
        player.decoder_mutex = nullptr;
    }

    if (player.cmd_queue) {
        vQueueDelete(player.cmd_queue);
        player.cmd_queue = nullptr;
    }

    player.state.store(PlayerState::IDLE);
    ESP_LOGI(TAG, "WebP player deinitialized");
}

esp_err_t webp_player_play_app(App_t* app, uint32_t duration_ms, bool immediate) {
    Command cmd = {};
    cmd.type = CommandType::PLAY;
    cmd.play.source_type = WEBP_SOURCE_RAM;
    cmd.play.ram_app = app;
    cmd.play.embedded_name = nullptr;
    cmd.play.duration_ms = duration_ms;
    cmd.play.immediate = immediate;

    return send_command(&cmd);
}

esp_err_t webp_player_play_embedded(const char* name, bool immediate) {
    Command cmd = {};
    cmd.type = CommandType::PLAY;
    cmd.play.source_type = WEBP_SOURCE_EMBEDDED;
    cmd.play.ram_app = nullptr;
    cmd.play.embedded_name = name;
    cmd.play.duration_ms = 0;  // Embedded always loops forever
    cmd.play.immediate = immediate;

    return send_command(&cmd);
}

esp_err_t webp_player_set_next_app(App_t* app, uint32_t duration_ms) {
    Command cmd = {};
    cmd.type = CommandType::SET_NEXT;
    cmd.play.source_type = WEBP_SOURCE_RAM;
    cmd.play.ram_app = app;
    cmd.play.embedded_name = nullptr;
    cmd.play.duration_ms = duration_ms;
    cmd.play.immediate = false;

    return send_command(&cmd);
}

esp_err_t webp_player_stop() {
    Command cmd = {};
    cmd.type = CommandType::STOP;
    return send_command(&cmd);
}

esp_err_t webp_player_pause() {
    Command cmd = {};
    cmd.type = CommandType::PAUSE;
    return send_command(&cmd);
}

esp_err_t webp_player_resume() {
    Command cmd = {};
    cmd.type = CommandType::RESUME;
    return send_command(&cmd);
}

bool webp_player_is_playing() {
    return player.state.load() == PlayerState::PLAYING;
}

bool webp_player_is_paused() {
    return player.state.load() == PlayerState::PAUSED;
}

void webp_player_set_display_mode(bool enabled) {
    player.display_mode.store(enabled);
    if (!enabled) {
        // Exiting display mode - clear need_next state
        player.need_next_pending = false;
    }
    ESP_LOGI(TAG, "Display mode: %s", enabled ? "enabled" : "disabled");
}

void webp_player_request_next() {
    if (!player.display_mode.load()) {
        // Not in display mode - ignore
        return;
    }

    // If already pending, don't emit duplicate event
    if (player.need_next_pending) {
        ESP_LOGD(TAG, "Request next: already pending");
        return;
    }

    // Set pending flag and emit immediately
    player.need_next_pending = true;
    player.last_need_next_tick = xTaskGetTickCount();
    emit_need_next_event();
    ESP_LOGI(TAG, "Requested next app");
}

#include "webp_player.h"
#include "display.h"
#include "static_files.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
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
    // Notification Bits
    //------------------------------------------------------------------------------

    constexpr uint32_t NOTIFY_PLAY = (1 << 0);
    constexpr uint32_t NOTIFY_STOP = (1 << 1);

    //------------------------------------------------------------------------------
    // Player State
    //------------------------------------------------------------------------------

    enum class State : uint8_t {
        IDLE,
        PLAYING,
    };

    //------------------------------------------------------------------------------
    // Pending Command (written by API, read by task)
    //------------------------------------------------------------------------------

    struct PendingCmd {
        std::atomic<bool> valid{ false };
        webp_source_type_t source_type = WEBP_SOURCE_RAM;
        App_t* ram_app = nullptr;
        const char* embedded_name = nullptr;
        uint32_t duration_ms = 0;
    };

    //------------------------------------------------------------------------------
    // Player Context
    //------------------------------------------------------------------------------

    struct PlayerContext {
        TaskHandle_t task = nullptr;
        SemaphoreHandle_t decoder_mutex = nullptr;

        std::atomic<State> state{ State::IDLE };
        PendingCmd pending;

        // Current playback source info (for event payloads)
        webp_source_type_t source_type = WEBP_SOURCE_RAM;
        App_t* ram_app = nullptr;
        const char* embedded_name = nullptr;

        // WebP data
        const uint8_t* webp_bytes = nullptr;
        size_t webp_size = 0;
        uint8_t* webp_buffer = nullptr;  // Owned copy for RAM apps only
        webp_source_type_t loaded_source_type = WEBP_SOURCE_EMBEDDED;  // Track what's currently loaded

        // Decoder
        WebPAnimDecoder* decoder = nullptr;
        WebPData webp_data = { nullptr, 0 };
        WebPAnimInfo anim_info = {};

        // Timing
        TickType_t playback_start = 0;
        TickType_t next_frame_tick = 0;
        uint32_t duration_ms = 0;
        int last_timestamp = 0;
        uint32_t frame_count = 0;

        // Error tracking
        int decode_error_count = 0;
    };

    PlayerContext ctx;

    //------------------------------------------------------------------------------
    // Helpers
    //------------------------------------------------------------------------------

    inline uint32_t ticks_to_ms(TickType_t ticks) {
        return static_cast<uint32_t>(ticks * portTICK_PERIOD_MS);
    }

    //------------------------------------------------------------------------------
    // Decoder Management
    //------------------------------------------------------------------------------

    void destroy_decoder() {
        if (ctx.decoder) {
            xSemaphoreTake(ctx.decoder_mutex, portMAX_DELAY);
            WebPAnimDecoderDelete(ctx.decoder);
            ctx.decoder = nullptr;
            xSemaphoreGive(ctx.decoder_mutex);
        }
    }

    esp_err_t create_decoder() {
        destroy_decoder();

        if (!ctx.webp_bytes || ctx.webp_size == 0) {
            ESP_LOGE(TAG, "No WebP data");
            return ESP_ERR_INVALID_ARG;
        }

        xSemaphoreTake(ctx.decoder_mutex, portMAX_DELAY);

        ctx.webp_data.bytes = ctx.webp_bytes;
        ctx.webp_data.size = ctx.webp_size;

        // Configure decoder for RGBA output
        WebPAnimDecoderOptions dec_options;
        WebPAnimDecoderOptionsInit(&dec_options);
        dec_options.color_mode = MODE_RGBA;
        ctx.decoder = WebPAnimDecoderNew(&ctx.webp_data, &dec_options);

        if (!ctx.decoder) {
            xSemaphoreGive(ctx.decoder_mutex);
            ESP_LOGE(TAG, "Failed to create decoder");
            return ESP_FAIL;
        }

        if (!WebPAnimDecoderGetInfo(ctx.decoder, &ctx.anim_info)) {
            WebPAnimDecoderDelete(ctx.decoder);
            ctx.decoder = nullptr;
            xSemaphoreGive(ctx.decoder_mutex);
            ESP_LOGE(TAG, "Failed to get anim info");
            return ESP_FAIL;
        }

        ctx.frame_count = ctx.anim_info.frame_count;
        xSemaphoreGive(ctx.decoder_mutex);

        ESP_LOGI(TAG, "Decoder created: %u frames, %ux%u",
            ctx.frame_count, ctx.anim_info.canvas_width, ctx.anim_info.canvas_height);
        return ESP_OK;
    }

    //------------------------------------------------------------------------------
    // Content Loading
    //------------------------------------------------------------------------------

    void free_buffer() {
        // Only free if we own the buffer (RAM content, not embedded flash)
        if (ctx.loaded_source_type == WEBP_SOURCE_RAM && ctx.webp_buffer) {
            heap_caps_free(ctx.webp_buffer);
            ctx.webp_buffer = nullptr;
        }
        ctx.webp_bytes = nullptr;
        ctx.webp_size = 0;
        ctx.loaded_source_type = WEBP_SOURCE_EMBEDDED;  // Reset to safe default
    }

    esp_err_t load_content() {
        free_buffer();

        if (ctx.source_type == WEBP_SOURCE_EMBEDDED) {
            // Embedded: direct pointer to flash (we don't own this memory)
            const uint8_t* data = nullptr;
            size_t len = 0;

            if (get_image_data(ctx.embedded_name, &data, &len) == 0 || !data || len == 0) {
                ESP_LOGE(TAG, "Embedded sprite not found: %s", ctx.embedded_name);
                return ESP_ERR_NOT_FOUND;
            }

            ctx.webp_bytes = data;
            ctx.webp_size = len;
            ctx.loaded_source_type = WEBP_SOURCE_EMBEDDED;
            ESP_LOGI(TAG, "Loaded embedded sprite: %s (%zu bytes)", ctx.embedded_name, len);
        }
        else {
            // RAM: copy to owned buffer (we own this memory)
            if (!ctx.ram_app || !ctx.ram_app->data || ctx.ram_app->len == 0) {
                ESP_LOGE(TAG, "Invalid RAM app");
                return ESP_ERR_INVALID_ARG;
            }

            ctx.webp_buffer = static_cast<uint8_t*>(
                heap_caps_malloc(ctx.ram_app->len, MALLOC_CAP_SPIRAM));
            if (!ctx.webp_buffer) {
                ESP_LOGE(TAG, "Failed to alloc %zu bytes", ctx.ram_app->len);
                return ESP_ERR_NO_MEM;
            }

            // Lock app mutex during copy
            xSemaphoreTake(ctx.ram_app->mutex, portMAX_DELAY);
            std::memcpy(ctx.webp_buffer, ctx.ram_app->data, ctx.ram_app->len);
            ctx.webp_size = ctx.ram_app->len;
            xSemaphoreGive(ctx.ram_app->mutex);

            ctx.webp_bytes = ctx.webp_buffer;
            ctx.loaded_source_type = WEBP_SOURCE_RAM;
            ESP_LOGI(TAG, "Loaded RAM app (%zu bytes)", ctx.webp_size);
        }

        return ESP_OK;
    }

    //------------------------------------------------------------------------------
    // Event Emission
    //------------------------------------------------------------------------------

    void emit_playing_event() {
        webp_player_playing_evt_t evt = {};
        evt.source_type = ctx.source_type;
        evt.ram_app = ctx.ram_app;
        evt.embedded_name = ctx.embedded_name;
        evt.duration_ms = ctx.duration_ms;
        evt.frame_count = ctx.frame_count;

        esp_event_post(WEBP_PLAYER_EVENTS, WEBP_PLAYER_EVT_PLAYING,
            &evt, sizeof(evt), 0);
    }

    void emit_error_event(int error_code) {
        webp_player_error_evt_t evt = {};
        evt.source_type = ctx.source_type;
        evt.ram_app = ctx.ram_app;
        evt.embedded_name = ctx.embedded_name;
        evt.error_code = error_code;

        esp_event_post(WEBP_PLAYER_EVENTS, WEBP_PLAYER_EVT_ERROR,
            &evt, sizeof(evt), 0);
    }

    void emit_stopped_event() {
        esp_event_post(WEBP_PLAYER_EVENTS, WEBP_PLAYER_EVT_STOPPED,
            nullptr, 0, 0);
    }

    //------------------------------------------------------------------------------
    // State Transitions
    //------------------------------------------------------------------------------

    void goto_idle() {
        destroy_decoder();
        free_buffer();
        ctx.ram_app = nullptr;
        ctx.embedded_name = nullptr;
        ctx.state.store(State::IDLE);
    }

    esp_err_t start_playback() {
        ctx.decode_error_count = 0;

        esp_err_t err = load_content();
        if (err != ESP_OK) {
            return err;
        }

        err = create_decoder();
        if (err != ESP_OK) {
            free_buffer();
            return err;
        }

        ctx.playback_start = xTaskGetTickCount();
        ctx.next_frame_tick = ctx.playback_start;
        ctx.last_timestamp = 0;
        ctx.state.store(State::PLAYING);

        emit_playing_event();
        ESP_LOGI(TAG, "Playback started: %s, duration %u ms",
            ctx.source_type == WEBP_SOURCE_RAM ? "RAM app" : ctx.embedded_name,
            ctx.duration_ms);

        return ESP_OK;
    }

    //------------------------------------------------------------------------------
    // Duration Check
    //------------------------------------------------------------------------------

    bool check_duration_expired() {
        // Embedded sprites loop forever
        if (ctx.source_type == WEBP_SOURCE_EMBEDDED) {
            return false;
        }

        // Unlimited duration
        if (ctx.duration_ms == 0) {
            return false;
        }

        uint32_t elapsed = ticks_to_ms(xTaskGetTickCount() - ctx.playback_start);
        return elapsed >= ctx.duration_ms;
    }

    //------------------------------------------------------------------------------
    // Command Handling
    //------------------------------------------------------------------------------

    void handle_pending_command() {
        // Check for stop first
        // (stop is indicated by notify without valid pending)
        if (!ctx.pending.valid.load(std::memory_order_acquire)) {
            // Stop command
            if (ctx.state.load() == State::PLAYING) {
                goto_idle();
                emit_stopped_event();
                ESP_LOGI(TAG, "Stopped by command");
            }
            return;
        }

        // Play command - copy pending data
        ctx.source_type = ctx.pending.source_type;
        ctx.ram_app = ctx.pending.ram_app;
        ctx.embedded_name = ctx.pending.embedded_name;
        ctx.duration_ms = ctx.pending.duration_ms;
        ctx.pending.valid.store(false, std::memory_order_release);

        // Stop current if playing
        if (ctx.state.load() == State::PLAYING) {
            destroy_decoder();
            free_buffer();
        }

        // Start new playback
        esp_err_t err = start_playback();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "start_playback failed: %s", esp_err_to_name(err));
            emit_error_event(err);
            goto_idle();
        }
    }

    //------------------------------------------------------------------------------
    // Decode Error Handling
    //------------------------------------------------------------------------------

    void handle_decode_error() {
        ctx.decode_error_count++;
        ESP_LOGW(TAG, "Decode error %d/%d", ctx.decode_error_count, WEBP_PLAYER_RETRY_COUNT);

        if (ctx.decode_error_count >= WEBP_PLAYER_RETRY_COUNT) {
            ESP_LOGE(TAG, "Max retries reached");
            emit_error_event(ESP_FAIL);
            display_clear();
            goto_idle();
            return;
        }

        // Retry: recreate decoder
        vTaskDelay(pdMS_TO_TICKS(WEBP_PLAYER_RETRY_DELAY_MS));
        if (create_decoder() != ESP_OK) {
            ctx.decode_error_count = WEBP_PLAYER_RETRY_COUNT;
            handle_decode_error();
        }
    }

    //------------------------------------------------------------------------------
    // Frame Decode and Render
    // Returns frame delay in ms, or -1 on error
    //------------------------------------------------------------------------------

    int decode_and_render_frame() {
        // Note: Heap checks removed from hot path - they cause race conditions
        // with kd_common_init running on the other core

        if (!xSemaphoreTake(ctx.decoder_mutex, pdMS_TO_TICKS(50))) {
            return 0;  // Busy, try again next iteration
        }

        if (!ctx.decoder) {
            xSemaphoreGive(ctx.decoder_mutex);
            return -1;
        }

        // Check for loop completion
        if (!WebPAnimDecoderHasMoreFrames(ctx.decoder)) {
            WebPAnimDecoderReset(ctx.decoder);
            ctx.last_timestamp = 0;
        }

        // Get next frame
        uint8_t* frame_buffer = nullptr;
        int timestamp = 0;

        if (!WebPAnimDecoderGetNext(ctx.decoder, &frame_buffer, &timestamp)) {
            xSemaphoreGive(ctx.decoder_mutex);
            return -1;
        }

        xSemaphoreGive(ctx.decoder_mutex);

        if (!frame_buffer) {
            return -1;
        }

        // Reset error count on successful decode
        ctx.decode_error_count = 0;

        // Render frame
        display_render_rgba_frame(frame_buffer,
            ctx.anim_info.canvas_width,
            ctx.anim_info.canvas_height);

        // Calculate delay
        int delay_ms = timestamp - ctx.last_timestamp;
        ctx.last_timestamp = timestamp;

        // Static images: hold for remaining display time
        if (ctx.frame_count == 1) {
            if (ctx.duration_ms > 0) {
                uint32_t elapsed = ticks_to_ms(xTaskGetTickCount() - ctx.playback_start);
                if (elapsed < ctx.duration_ms) {
                    uint32_t remaining = ctx.duration_ms - elapsed;
                    delay_ms = static_cast<int>(remaining > 60000 ? 60000 : remaining);
                }
                else {
                    delay_ms = 0;  // Expired
                }
            }
            else {
                delay_ms = 100;  // Unlimited duration static image
            }
        }

        return delay_ms;
    }

    //------------------------------------------------------------------------------
    // Calculate Wait Ticks (maintains timing accuracy)
    //------------------------------------------------------------------------------

    TickType_t calculate_wait_ticks(int delay_ms) {
        if (delay_ms <= 0) {
            return 0;
        }

        TickType_t target_tick = ctx.next_frame_tick + pdMS_TO_TICKS(delay_ms);
        TickType_t now = xTaskGetTickCount();

        // If we're behind, don't wait
        if (now >= target_tick) {
            ctx.next_frame_tick = now;
            return 0;
        }

        ctx.next_frame_tick = target_tick;
        return target_tick - now;
    }

    //------------------------------------------------------------------------------
    // Player Task
    //------------------------------------------------------------------------------

    void player_task(void*) {
        ESP_LOGI(TAG, "Player task started");

        while (true) {
            State state = ctx.state.load();

            if (state == State::IDLE) {
                // Block until play command
                ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
                handle_pending_command();
                continue;
            }

            // PLAYING state

            // Check if duration expired
            if (check_duration_expired()) {
                emit_stopped_event();
                goto_idle();
                ESP_LOGI(TAG, "Duration expired");
                continue;
            }

            // Decode and render frame
            int delay_ms = decode_and_render_frame();
            if (delay_ms < 0) {
                handle_decode_error();
                continue;
            }

            // Wait for frame delay OR notification
            TickType_t wait_ticks = calculate_wait_ticks(delay_ms);
            uint32_t notified = ulTaskNotifyTake(pdTRUE, wait_ticks);

            if (notified) {
                handle_pending_command();
            }
        }
    }

}  // namespace

//------------------------------------------------------------------------------
// Public API
//------------------------------------------------------------------------------

esp_err_t webp_player_init() {
    if (ctx.task) {
        ESP_LOGW(TAG, "Already initialized");
        return ESP_OK;
    }

    ctx.decoder_mutex = xSemaphoreCreateMutex();
    if (!ctx.decoder_mutex) {
        ESP_LOGE(TAG, "Failed to create decoder mutex");
        return ESP_ERR_NO_MEM;
    }

    BaseType_t ret = xTaskCreatePinnedToCore(
        player_task,
        "webp_player",
        WEBP_PLAYER_TASK_STACK_SIZE,
        nullptr,
        WEBP_PLAYER_TASK_PRIORITY,
        &ctx.task,
        WEBP_PLAYER_TASK_CORE
    );

    if (ret != pdPASS) {
        vSemaphoreDelete(ctx.decoder_mutex);
        ctx.decoder_mutex = nullptr;
        ESP_LOGE(TAG, "Failed to create player task");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "WebP player initialized");
    return ESP_OK;
}

void webp_player_deinit() {
    if (ctx.task) {
        vTaskDelete(ctx.task);
        ctx.task = nullptr;
    }

    destroy_decoder();
    free_buffer();

    if (ctx.decoder_mutex) {
        vSemaphoreDelete(ctx.decoder_mutex);
        ctx.decoder_mutex = nullptr;
    }

    ctx.state.store(State::IDLE);
    ESP_LOGI(TAG, "WebP player deinitialized");
}

esp_err_t webp_player_play_app(App_t* app, uint32_t duration_ms, bool immediate) {
    if (!ctx.task) {
        return ESP_ERR_INVALID_STATE;
    }

    // Store pending command
    ctx.pending.source_type = WEBP_SOURCE_RAM;
    ctx.pending.ram_app = app;
    ctx.pending.embedded_name = nullptr;
    ctx.pending.duration_ms = duration_ms;
    ctx.pending.valid.store(true, std::memory_order_release);

    // Notify task
    xTaskNotify(ctx.task, NOTIFY_PLAY, eSetBits);

    return ESP_OK;
}

esp_err_t webp_player_play_embedded(const char* name, bool immediate) {
    if (!ctx.task) {
        return ESP_ERR_INVALID_STATE;
    }

    // Store pending command
    ctx.pending.source_type = WEBP_SOURCE_EMBEDDED;
    ctx.pending.ram_app = nullptr;
    ctx.pending.embedded_name = name;
    ctx.pending.duration_ms = 0;  // Embedded loops forever
    ctx.pending.valid.store(true, std::memory_order_release);

    // Notify task
    xTaskNotify(ctx.task, NOTIFY_PLAY, eSetBits);

    return ESP_OK;
}

esp_err_t webp_player_stop() {
    if (!ctx.task) {
        return ESP_ERR_INVALID_STATE;
    }

    // Clear pending valid (signals stop)
    ctx.pending.valid.store(false, std::memory_order_release);

    // Notify task
    xTaskNotify(ctx.task, NOTIFY_STOP, eSetBits);

    return ESP_OK;
}

bool webp_player_is_playing() {
    return ctx.state.load() == State::PLAYING;
}

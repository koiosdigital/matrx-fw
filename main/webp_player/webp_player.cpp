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

    constexpr uint32_t NOTIFY_PLAY = (1 << 0);
    constexpr uint32_t NOTIFY_STOP = (1 << 1);

    enum class State : uint8_t {
        IDLE,
        PLAYING,
    };

    struct PendingCmd {
        std::atomic<bool> valid{ false };
        webp_source_type_t source_type = WEBP_SOURCE_RAM;
        App_t* ram_app = nullptr;
        const char* embedded_name = nullptr;
        uint32_t duration_ms = 0;
    };

    struct PlayerContext {
        TaskHandle_t task = nullptr;
        SemaphoreHandle_t decoder_mutex = nullptr;

        std::atomic<State> state{ State::IDLE };
        PendingCmd pending;

        webp_source_type_t source_type = WEBP_SOURCE_RAM;
        App_t* ram_app = nullptr;
        const char* embedded_name = nullptr;

        const uint8_t* webp_bytes = nullptr;
        size_t webp_size = 0;
        uint8_t* webp_buffer = nullptr;
        webp_source_type_t loaded_source_type = WEBP_SOURCE_EMBEDDED;

        WebPAnimDecoder* decoder = nullptr;
        WebPData webp_data = { nullptr, 0 };
        WebPAnimInfo anim_info = {};

        TickType_t playback_start = 0;
        TickType_t next_frame_tick = 0;
        uint32_t duration_ms = 0;
        int last_timestamp = 0;
        uint32_t frame_count = 0;

        int decode_error_count = 0;

        uint8_t* prev_frame = nullptr;
        int prev_w = 0;
        int prev_h = 0;
        bool prev_valid = false;
    };

    PlayerContext ctx;

    inline uint32_t ticks_to_ms(TickType_t ticks) {
        return static_cast<uint32_t>(ticks * portTICK_PERIOD_MS);
    }

    void render_frame_diffed(const uint8_t* frame, int canvas_w, int canvas_h) {
        int disp_w = 0, disp_h = 0;
        display_get_dimensions(&disp_w, &disp_h);
        if (canvas_w < disp_w) disp_w = canvas_w;
        if (canvas_h < disp_h) disp_h = canvas_h;

        const size_t needed = static_cast<size_t>(disp_w) * disp_h * 4;
        if (!ctx.prev_frame || ctx.prev_w != disp_w || ctx.prev_h != disp_h) {
            heap_caps_free(ctx.prev_frame);
            ctx.prev_frame = static_cast<uint8_t*>(
                heap_caps_malloc(needed, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
            if (!ctx.prev_frame) {
                ctx.prev_frame = static_cast<uint8_t*>(
                    heap_caps_malloc(needed, MALLOC_CAP_SPIRAM));
            }
            ctx.prev_w = disp_w;
            ctx.prev_h = disp_h;
            ctx.prev_valid = false;
        }

        if (!ctx.prev_frame || !ctx.prev_valid) {
            display_render_rgba_frame(frame, canvas_w, canvas_h);
            if (ctx.prev_frame) {
                for (int y = 0; y < disp_h; y++) {
                    std::memcpy(ctx.prev_frame + static_cast<size_t>(y) * disp_w * 4,
                        frame + static_cast<size_t>(y) * canvas_w * 4,
                        static_cast<size_t>(disp_w) * 4);
                }
                ctx.prev_valid = true;
            }
            return;
        }

        int dirty_rows = 0;
        for (int y = 0; y < disp_h; y++) {
            const uint8_t* cur_row = frame + static_cast<size_t>(y) * canvas_w * 4;
            const uint8_t* prev_row = ctx.prev_frame + static_cast<size_t>(y) * disp_w * 4;
            if (std::memcmp(cur_row, prev_row, static_cast<size_t>(disp_w) * 4) != 0) {
                dirty_rows++;
            }
        }

        if (dirty_rows == 0) {
            return;
        }

        if (dirty_rows > (disp_h * 3) / 4 && canvas_w == disp_w) {
            display_render_rgba_frame(frame, canvas_w, canvas_h);
            std::memcpy(ctx.prev_frame, frame, needed);
            return;
        }

        for (int y = 0; y < disp_h; y++) {
            const uint32_t* cur_row =
                reinterpret_cast<const uint32_t*>(frame + static_cast<size_t>(y) * canvas_w * 4);
            uint32_t* prev_row =
                reinterpret_cast<uint32_t*>(ctx.prev_frame + static_cast<size_t>(y) * disp_w * 4);

            if (std::memcmp(cur_row, prev_row, static_cast<size_t>(disp_w) * 4) == 0) {
                continue;
            }

            int first = 0;
            while (cur_row[first] == prev_row[first]) first++;
            int last = disp_w - 1;
            while (cur_row[last] == prev_row[last]) last--;

            const int span = last - first + 1;
            display_render_rgba_span(reinterpret_cast<const uint8_t*>(cur_row + first),
                first, y, span);
            std::memcpy(prev_row + first, cur_row + first, static_cast<size_t>(span) * 4);
        }
    }

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

        return ESP_OK;
    }

    void free_buffer() {
        if (ctx.loaded_source_type == WEBP_SOURCE_RAM && ctx.webp_buffer) {
            heap_caps_free(ctx.webp_buffer);
            ctx.webp_buffer = nullptr;
        }
        ctx.webp_bytes = nullptr;
        ctx.webp_size = 0;
        ctx.loaded_source_type = WEBP_SOURCE_EMBEDDED;
    }

    esp_err_t load_content() {
        free_buffer();

        if (ctx.source_type == WEBP_SOURCE_EMBEDDED) {
            const uint8_t* data = nullptr;
            size_t len = 0;

            if (get_image_data(ctx.embedded_name, &data, &len) == 0 || !data || len == 0) {
                ESP_LOGE(TAG, "Embedded sprite not found: %s", ctx.embedded_name);
                return ESP_ERR_NOT_FOUND;
            }

            ctx.webp_bytes = data;
            ctx.webp_size = len;
            ctx.loaded_source_type = WEBP_SOURCE_EMBEDDED;
        }
        else {
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

            xSemaphoreTake(ctx.ram_app->mutex, portMAX_DELAY);
            std::memcpy(ctx.webp_buffer, ctx.ram_app->data, ctx.ram_app->len);
            ctx.webp_size = ctx.ram_app->len;
            xSemaphoreGive(ctx.ram_app->mutex);

            ctx.webp_bytes = ctx.webp_buffer;
            ctx.loaded_source_type = WEBP_SOURCE_RAM;
        }

        return ESP_OK;
    }

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

    void goto_idle() {
        destroy_decoder();
        free_buffer();
        ctx.ram_app = nullptr;
        ctx.embedded_name = nullptr;
        ctx.state.store(State::IDLE);
    }

    esp_err_t start_playback() {
        ctx.decode_error_count = 0;

        ctx.prev_valid = false;

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

        return ESP_OK;
    }

    bool check_duration_expired() {
        if (ctx.source_type == WEBP_SOURCE_EMBEDDED) {
            return false;
        }

        if (ctx.duration_ms == 0) {
            return false;
        }

        uint32_t elapsed = ticks_to_ms(xTaskGetTickCount() - ctx.playback_start);
        return elapsed >= ctx.duration_ms;
    }

    void handle_pending_command() {
        if (!ctx.pending.valid.load(std::memory_order_acquire)) {
            if (ctx.state.load() == State::PLAYING) {
                goto_idle();
                emit_stopped_event();
            }
            return;
        }

        ctx.source_type = ctx.pending.source_type;
        ctx.ram_app = ctx.pending.ram_app;
        ctx.embedded_name = ctx.pending.embedded_name;
        ctx.duration_ms = ctx.pending.duration_ms;
        ctx.pending.valid.store(false, std::memory_order_release);

        if (ctx.state.load() == State::PLAYING) {
            destroy_decoder();
            free_buffer();
        }

        esp_err_t err = start_playback();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "start_playback failed: %s", esp_err_to_name(err));
            emit_error_event(err);
            goto_idle();
        }
    }

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

        vTaskDelay(pdMS_TO_TICKS(WEBP_PLAYER_RETRY_DELAY_MS));
        if (create_decoder() != ESP_OK) {
            ESP_LOGE(TAG, "Decoder recreation failed, giving up");
            emit_error_event(ESP_FAIL);
            display_clear();
            goto_idle();
        }
    }

    int decode_and_render_frame() {
        if (!xSemaphoreTake(ctx.decoder_mutex, pdMS_TO_TICKS(50))) {
            return 0;
        }

        if (!ctx.decoder) {
            xSemaphoreGive(ctx.decoder_mutex);
            return -1;
        }

        if (!WebPAnimDecoderHasMoreFrames(ctx.decoder)) {
            WebPAnimDecoderReset(ctx.decoder);
            ctx.last_timestamp = 0;
        }

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

        ctx.decode_error_count = 0;

        render_frame_diffed(frame_buffer,
            ctx.anim_info.canvas_width,
            ctx.anim_info.canvas_height);

        int delay_ms = timestamp - ctx.last_timestamp;
        ctx.last_timestamp = timestamp;

        if (ctx.frame_count == 1) {
            if (ctx.duration_ms > 0) {
                uint32_t elapsed = ticks_to_ms(xTaskGetTickCount() - ctx.playback_start);
                if (elapsed < ctx.duration_ms) {
                    uint32_t remaining = ctx.duration_ms - elapsed;
                    delay_ms = static_cast<int>(remaining > 60000 ? 60000 : remaining);
                }
                else {
                    delay_ms = 0;
                }
            }
            else {
                delay_ms = 100;
            }
        }

        return delay_ms;
    }

    TickType_t calculate_wait_ticks(int delay_ms) {
        if (delay_ms <= 0) {
            return 0;
        }

        TickType_t target_tick = ctx.next_frame_tick + pdMS_TO_TICKS(delay_ms);
        TickType_t now = xTaskGetTickCount();

        if (now >= target_tick) {
            ctx.next_frame_tick = now;
            return 0;
        }

        ctx.next_frame_tick = target_tick;
        return target_tick - now;
    }

    void player_task(void*) {
        while (true) {
            State state = ctx.state.load();

            if (state == State::IDLE) {
                ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
                handle_pending_command();
                continue;
            }

            if (check_duration_expired()) {
                emit_stopped_event();
                goto_idle();
                continue;
            }

            int delay_ms = decode_and_render_frame();
            if (delay_ms < 0) {
                handle_decode_error();
                continue;
            }

            TickType_t wait_ticks = calculate_wait_ticks(delay_ms);
            uint32_t notified = ulTaskNotifyTake(pdTRUE, wait_ticks);

            if (notified) {
                handle_pending_command();
            }
        }
    }

}  // namespace

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

    return ESP_OK;
}

void webp_player_deinit() {
    if (ctx.task) {
        vTaskDelete(ctx.task);
        ctx.task = nullptr;
    }

    destroy_decoder();
    free_buffer();

    heap_caps_free(ctx.prev_frame);
    ctx.prev_frame = nullptr;
    ctx.prev_w = 0;
    ctx.prev_h = 0;
    ctx.prev_valid = false;

    if (ctx.decoder_mutex) {
        vSemaphoreDelete(ctx.decoder_mutex);
        ctx.decoder_mutex = nullptr;
    }

    ctx.state.store(State::IDLE);
}

esp_err_t webp_player_play_app(App_t* app, uint32_t duration_ms) {
    if (!ctx.task) {
        return ESP_ERR_INVALID_STATE;
    }

    ctx.pending.source_type = WEBP_SOURCE_RAM;
    ctx.pending.ram_app = app;
    ctx.pending.embedded_name = nullptr;
    ctx.pending.duration_ms = duration_ms;
    ctx.pending.valid.store(true, std::memory_order_release);

    xTaskNotify(ctx.task, NOTIFY_PLAY, eSetBits);

    return ESP_OK;
}

esp_err_t webp_player_play_embedded(const char* name) {
    if (!ctx.task) {
        return ESP_ERR_INVALID_STATE;
    }

    ctx.pending.source_type = WEBP_SOURCE_EMBEDDED;
    ctx.pending.ram_app = nullptr;
    ctx.pending.embedded_name = name;
    ctx.pending.duration_ms = 0;
    ctx.pending.valid.store(true, std::memory_order_release);

    xTaskNotify(ctx.task, NOTIFY_PLAY, eSetBits);

    return ESP_OK;
}

esp_err_t webp_player_stop() {
    if (!ctx.task) {
        return ESP_ERR_INVALID_STATE;
    }

    ctx.pending.valid.store(false, std::memory_order_release);

    xTaskNotify(ctx.task, NOTIFY_STOP, eSetBits);

    return ESP_OK;
}

bool webp_player_is_playing() {
    return ctx.state.load() == State::PLAYING;
}

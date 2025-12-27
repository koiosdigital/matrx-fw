#include "display.h"
#include "pinout.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>

#include <esp_log.h>
#include <esp_event.h>
#include <esp_wifi.h>
#include <wifi_provisioning/manager.h>
#include <protocomm_ble.h>

#include <webp/demux.h>
#include <qrcode.h>
#include <kd_common.h>
#include "sprites.h"

#include <cstring>

#include "raii_utils.hpp"

static const char* TAG = "display";

namespace {

    // Constants
    constexpr int DECODER_RETRY_COUNT = 3;
    constexpr int DECODER_RETRY_DELAY_MS = 200;
    constexpr int SINGLE_FRAME_DELAY_MS = 1000;

    // Encapsulated display state
    struct DisplayState {
        MatrixPanel_I2S_DMA* dma_display = nullptr;
        TaskHandle_t decoder_task = nullptr;
        SemaphoreHandle_t webp_semaphore = nullptr;

        // WebP decoder state
        WebPData webp_data = { nullptr, 0 };
        WebPAnimDecoder* decoder = nullptr;
        WebPAnimInfo anim_info = {};

        // Status bar overlay
        DisplayStatusBar_t status_bar = { false, 0, 0, 0 };

        bool init_semaphore() {
            webp_semaphore = xSemaphoreCreateMutex();
            return webp_semaphore != nullptr;
        }

        void destroy_decoder() {
            if (decoder == nullptr) return;

            raii::MutexGuard lock(webp_semaphore);
            if (lock && decoder != nullptr) {
                WebPAnimDecoderDelete(decoder);
                decoder = nullptr;
            }
        }

        esp_err_t start_decoder() {
            destroy_decoder();

            if (webp_data.bytes == nullptr || webp_data.size == 0) {
                ESP_LOGE(TAG, "no sprite data");
                return ESP_ERR_INVALID_ARG;
            }

            raii::MutexGuard lock(webp_semaphore);
            if (!lock) {
                ESP_LOGE(TAG, "failed to take semaphore for decoder start");
                return ESP_ERR_TIMEOUT;
            }

            decoder = WebPAnimDecoderNew(&webp_data, nullptr);
            if (decoder == nullptr) {
                ESP_LOGE(TAG, "failed to create WebP decoder");
                return ESP_FAIL;
            }

            if (!WebPAnimDecoderGetInfo(decoder, &anim_info)) {
                ESP_LOGE(TAG, "failed to get WebP animation info");
                WebPAnimDecoderDelete(decoder);
                decoder = nullptr;
                return ESP_FAIL;
            }

            xTaskNotify(decoder_task, static_cast<uint32_t>(WebPTaskNotification_t::WEBP_START),
                eSetValueWithOverwrite);
            return ESP_OK;
        }
    };

    DisplayState disp;

    // Simple 5x7 monospaced font for digits 0-9
    constexpr uint8_t font_5x7[][7] = {
        {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}, // 0
        {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E}, // 1
        {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F}, // 2
        {0x0E, 0x11, 0x01, 0x0E, 0x01, 0x11, 0x0E}, // 3
        {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}, // 4
        {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E}, // 5
        {0x0E, 0x10, 0x10, 0x1E, 0x11, 0x11, 0x0E}, // 6
        {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}, // 7
        {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}, // 8
        {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x01, 0x0E}, // 9
    };

    void decoder_task_func(void*) {
        bool active = false;
        TickType_t anim_start_tick = 0;
        uint8_t* frame_buffer = nullptr;
        int last_timestamp = 0;

        while (true) {
            // Wait for notification when not actively displaying
            if (!active) {
                uint32_t notification_value = 0;
                if (xTaskNotifyWait(0, ULONG_MAX, &notification_value, portMAX_DELAY) == pdTRUE) {
                    if (notification_value == static_cast<uint32_t>(WebPTaskNotification_t::WEBP_START)) {
                        ESP_LOGI(TAG, "decoder notified to start");
                        active = true;
                    }
                }
            }

            raii::MutexGuard lock(disp.webp_semaphore, pdMS_TO_TICKS(100));
            if (!lock) continue;

            // Check if decoder was destroyed
            if (disp.decoder == nullptr) {
                active = false;
                continue;
            }

            // Handle animation looping
            if (!WebPAnimDecoderHasMoreFrames(disp.decoder)) {
                WebPAnimDecoderReset(disp.decoder);
                anim_start_tick = 0;
                last_timestamp = 0;
            }

            if (anim_start_tick == 0) {
                anim_start_tick = xTaskGetTickCount();
                last_timestamp = 0;
            }

            int timestamp = 0;
            bool get_next_success = WebPAnimDecoderGetNext(disp.decoder, &frame_buffer, &timestamp);

            // Release lock before potentially long operations
            lock.release();

            if (!get_next_success) {
                ESP_LOGE(TAG, "error getting next frame, restarting decoder");
                disp.destroy_decoder();

                if (disp.start_decoder() != ESP_OK) {
                    ESP_LOGE(TAG, "Failed to restart decoder, stopping");
                    active = false;
                }
                continue;
            }

            if (frame_buffer == nullptr) {
                ESP_LOGE(TAG, "frame buffer is null");
                active = false;
                continue;
            }

#if DISPLAY_ENABLED
            // Render frame to display
            int px = 0;
            for (int y = 0; y < CONFIG_MATRIX_HEIGHT; y++) {
                for (int x = 0; x < CONFIG_MATRIX_WIDTH; x++) {
                    disp.dma_display->drawPixelRGB888(x, y,
                        frame_buffer[px * 4],
                        frame_buffer[px * 4 + 1],
                        frame_buffer[px * 4 + 2]);
                    px++;
                }
            }

            // Draw status bar overlay if enabled
            if (disp.status_bar.enabled) {
                disp.dma_display->drawFastHLine(0, 0, CONFIG_MATRIX_WIDTH,
                    disp.status_bar.r, disp.status_bar.g, disp.status_bar.b);
            }
#endif

            // Calculate and apply frame delay
            int delay = timestamp - last_timestamp;
            last_timestamp = timestamp;

            // Prevent task starvation for single-frame sprites
            if (disp.anim_info.frame_count == 1 && delay <= 0) {
                delay = SINGLE_FRAME_DELAY_MS;
            }

            if (delay > 0) {
                xTaskDelayUntil(&anim_start_tick, pdMS_TO_TICKS(delay));
            }
        }
    }

    void draw_char_scaled(uint8_t* buffer, int buf_width, int buf_height, char c,
        int x, int y, int scale, uint8_t r, uint8_t g, uint8_t b) {
        if (c < '0' || c > '9') {
            return;
        }

        int char_index = c - '0';
        const uint8_t* char_data = font_5x7[char_index];

        for (int row = 0; row < 7; row++) {
            uint8_t row_data = char_data[row];
            for (int col = 0; col < 5; col++) {
                if (row_data & (1 << (4 - col))) {
                    for (int sy = 0; sy < scale; sy++) {
                        for (int sx = 0; sx < scale; sx++) {
                            int px = x + col * scale + sx;
                            int py = y + row * scale + sy;
                            if (px >= 0 && px < buf_width && py >= 0 && py < buf_height) {
                                size_t idx = (static_cast<size_t>(py) * static_cast<size_t>(buf_width)
                                    + static_cast<size_t>(px)) * 3;
                                buffer[idx] = r;
                                buffer[idx + 1] = g;
                                buffer[idx + 2] = b;
                            }
                        }
                    }
                }
            }
        }
    }

    void display_pop_code() {
        char* pop_token = kd_common_provisioning_get_pop_token();
        if (pop_token == nullptr) {
            ESP_LOGE(TAG, "POP token is NULL");
            return;
        }

        int w = 0, h = 0;
        get_display_dimensions(&w, &h);

        size_t buffer_size = get_display_buffer_size();
        auto display_buffer = static_cast<uint8_t*>(
            heap_caps_calloc(buffer_size, sizeof(uint8_t), MALLOC_CAP_SPIRAM));
        if (display_buffer == nullptr) {
            ESP_LOGE(TAG, "malloc failed: display buffer");
            return;
        }
        std::memset(display_buffer, 0, buffer_size);

        // Layout: 6 chars * 5px + 5 gaps * 2px = 40px, centered in 64px width
        size_t token_len = std::strlen(pop_token);
        constexpr int scale = 1;
        constexpr int char_width = 5 * scale;
        constexpr int char_height = 7 * scale;
        constexpr int spacing = 2;

        int total_width = static_cast<int>(token_len) * char_width
            + static_cast<int>(token_len - 1) * spacing;
        int x_start = (w - total_width) / 2;
        int y_start = (h - char_height) / 2;

        for (size_t i = 0; i < token_len; i++) {
            int x = x_start + static_cast<int>(i) * (char_width + spacing);
            draw_char_scaled(display_buffer, w, h, pop_token[i], x, y_start, scale, 255, 255, 255);
        }

        display_raw_buffer(display_buffer, buffer_size);
        free(display_buffer);

        ESP_LOGI(TAG, "Displaying POP code: %s", pop_token);
    }

    // Consolidated event handlers for provisioning and WiFi display states
    void ble_event_handler(void*, esp_event_base_t, int32_t event_id, void*) {
        if (event_id == PROTOCOMM_TRANSPORT_BLE_CONNECTED) {
            display_pop_code();
        }
        else if (event_id == PROTOCOMM_TRANSPORT_BLE_DISCONNECTED) {
            if (!kd_common_is_wifi_connected()) {
                ESP_LOGI(TAG, "BLE disconnected during provisioning, showing setup sprite");
                show_fs_sprite("setup");
            }
        }
    }

    void prov_event_handler(void*, esp_event_base_t, int32_t event_id, void*) {
        if (event_id == WIFI_PROV_START) {
            show_fs_sprite("setup");
        }
        else if (event_id == WIFI_PROV_END) {
            wifi_config_t wifi_cfg;
            if (esp_wifi_get_config(WIFI_IF_STA, &wifi_cfg) == ESP_OK &&
                std::strlen(reinterpret_cast<const char*>(wifi_cfg.sta.ssid)) != 0) {
                show_fs_sprite("connecting");
            }
        }
    }

    void wifi_event_handler(void*, esp_event_base_t, int32_t event_id, void*) {
        if (event_id == WIFI_EVENT_STA_START) {
            wifi_config_t wifi_cfg;
            if (esp_wifi_get_config(WIFI_IF_STA, &wifi_cfg) == ESP_OK &&
                std::strlen(reinterpret_cast<const char*>(wifi_cfg.sta.ssid)) != 0) {
                show_fs_sprite("connecting");
            }
        }
    }

}  // namespace

//MARK: Public API

void display_init() {
    HUB75_I2S_CFG::i2s_pins pins = {
        .r1 = R1_PIN,
        .g1 = G1_PIN,
        .b1 = B1_PIN,
        .r2 = R2_PIN,
        .g2 = G2_PIN,
        .b2 = B2_PIN,
        .a = A_PIN,
        .b = B_PIN,
        .c = C_PIN,
        .d = D_PIN,
        .e = E_PIN,
        .lat = LAT_PIN,
        .oe = OE_PIN,
        .clk = CLK_PIN
    };

    HUB75_I2S_CFG mxconfig(CONFIG_MATRIX_WIDTH, CONFIG_MATRIX_HEIGHT, 1, pins);

#if DISPLAY_ENABLED
    disp.dma_display = new MatrixPanel_I2S_DMA(mxconfig);
    disp.dma_display->begin();
    disp.dma_display->setBrightness(32);
    disp.dma_display->fillScreenRGB888(0, 0, 0);
#endif

    disp.init_semaphore();

    xTaskCreatePinnedToCore(decoder_task_func, "decoder", 4096, nullptr, 10,
        &disp.decoder_task, 1);

    // Register event handlers for provisioning display states
    esp_event_handler_register(PROTOCOMM_TRANSPORT_BLE_EVENT, ESP_EVENT_ANY_ID,
        ble_event_handler, nullptr);
    esp_event_handler_register(WIFI_PROV_EVENT, ESP_EVENT_ANY_ID,
        prov_event_handler, nullptr);
    esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_START,
        wifi_event_handler, nullptr);

    show_fs_sprite("boot");
    vTaskDelay(pdMS_TO_TICKS(1200));
}

esp_err_t display_sprite(uint8_t* p_sprite_buf, size_t sprite_buf_len) {
    if (p_sprite_buf == nullptr || sprite_buf_len == 0) {
        ESP_LOGE(TAG, "invalid sprite buffer");
        return ESP_ERR_INVALID_ARG;
    }

    // Skip if already displaying this sprite
    if (p_sprite_buf == disp.webp_data.bytes && sprite_buf_len == disp.webp_data.size) {
        return ESP_OK;
    }

    disp.destroy_decoder();

    disp.webp_data.bytes = p_sprite_buf;
    disp.webp_data.size = sprite_buf_len;

    // Start decoder with limited retries
    esp_err_t err = ESP_FAIL;
    for (int retry = 0; retry < DECODER_RETRY_COUNT; retry++) {
        err = disp.start_decoder();
        if (err == ESP_OK) break;

        ESP_LOGW(TAG, "Failed to start decoder, retry %d/%d", retry + 1, DECODER_RETRY_COUNT);
        vTaskDelay(pdMS_TO_TICKS(DECODER_RETRY_DELAY_MS));
    }

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start decoder after %d retries", DECODER_RETRY_COUNT);
    }
    return err;
}

void display_raw_buffer(uint8_t* p_raw_buf, size_t raw_buf_len) {
    disp.destroy_decoder();

    disp.webp_data.bytes = nullptr;
    disp.webp_data.size = 0;

#if DISPLAY_ENABLED
    disp.dma_display->fillScreen(0);

    if (raw_buf_len != CONFIG_MATRIX_WIDTH * CONFIG_MATRIX_HEIGHT * 3) {
        return;
    }

    for (int y = 0; y < CONFIG_MATRIX_HEIGHT; y++) {
        for (int x = 0; x < CONFIG_MATRIX_WIDTH; x++) {
            int px = (y * CONFIG_MATRIX_WIDTH + x) * 3;
            disp.dma_display->drawPixelRGB888(x, y,
                p_raw_buf[px], p_raw_buf[px + 1], p_raw_buf[px + 2]);
        }
    }
#endif
}

void display_clear() {
#if DISPLAY_ENABLED
    if (disp.dma_display != nullptr) {
        disp.destroy_decoder();
        disp.dma_display->fillScreenRGB888(0, 0, 0);
    }
#endif
}

void display_set_brightness(uint8_t brightness) {
#if DISPLAY_ENABLED
    if (disp.dma_display != nullptr) {
        disp.dma_display->setBrightness(brightness);
    }
#else
    ESP_LOGW(TAG, "Display is not enabled, cannot set brightness");
#endif
}

void display_clear_status_bar() {
    disp.status_bar.enabled = false;
}

void display_set_status_bar(uint8_t r, uint8_t g, uint8_t b) {
    disp.status_bar.enabled = true;
    disp.status_bar.r = r;
    disp.status_bar.g = g;
    disp.status_bar.b = b;
}

size_t get_display_buffer_size() {
    return CONFIG_MATRIX_WIDTH * CONFIG_MATRIX_HEIGHT * 3;
}

void get_display_dimensions(int* w, int* h) {
    *w = CONFIG_MATRIX_WIDTH;
    *h = CONFIG_MATRIX_HEIGHT;
}

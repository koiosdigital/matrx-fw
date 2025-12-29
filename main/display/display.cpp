// Display - Pure hardware layer for HUB75 LED matrix
#include "display.h"
#include "pinout.h"
#include "webp_player.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <esp_log.h>
#include <esp_event.h>
#include <esp_wifi.h>
#include <wifi_provisioning/manager.h>
#include <protocomm_ble.h>

#include <kd_common.h>
#include <esp_heap_caps.h>

#include <cstring>

static const char* TAG = "display";

namespace {

    // Timing constants
    constexpr TickType_t BOOT_SPRITE_DELAY_MS = 1200;

    // Display state
    struct DisplayState {
        MatrixPanel_I2S_DMA* dma_display = nullptr;
        DisplayStatusBar_t status_bar = { false, 0, 0, 0 };
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
            ESP_LOGE(TAG, "POP token is null");
            return;
        }

        // Stop the WebP player before displaying raw buffer
        webp_player_stop();

        vTaskDelay(pdMS_TO_TICKS(200));

        int w = 0, h = 0;
        display_get_dimensions(&w, &h);

        size_t buffer_size = display_get_buffer_size();
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

        display_render_rgb_buffer(display_buffer, buffer_size);
        free(display_buffer);

        ESP_LOGI(TAG, "Displaying POP code: %s", pop_token);
    }

    // Event handlers for provisioning display states
    void ble_event_handler(void*, esp_event_base_t, int32_t event_id, void*) {
        if (event_id == PROTOCOMM_TRANSPORT_BLE_CONNECTED) {
            display_pop_code();
        }
        else if (event_id == PROTOCOMM_TRANSPORT_BLE_DISCONNECTED) {
            if (!kd_common_is_wifi_connected()) {
                ESP_LOGI(TAG, "BLE disconnected during provisioning, showing setup sprite");
                webp_player_play_embedded("setup", true);
            }
        }
    }

    void prov_event_handler(void*, esp_event_base_t, int32_t event_id, void*) {
        if (event_id == WIFI_PROV_START) {
            webp_player_play_embedded("setup", true);
        }
        else if (event_id == WIFI_PROV_END) {
            wifi_config_t wifi_cfg;
            if (esp_wifi_get_config(WIFI_IF_STA, &wifi_cfg) == ESP_OK &&
                std::strlen(reinterpret_cast<const char*>(wifi_cfg.sta.ssid)) != 0) {
                webp_player_play_embedded("connecting", true);
            }
        }
    }

    void wifi_event_handler(void*, esp_event_base_t, int32_t event_id, void*) {
        if (event_id == WIFI_EVENT_STA_START) {
            wifi_config_t wifi_cfg;
            if (esp_wifi_get_config(WIFI_IF_STA, &wifi_cfg) == ESP_OK &&
                std::strlen(reinterpret_cast<const char*>(wifi_cfg.sta.ssid)) != 0) {
                webp_player_play_embedded("connecting", true);
            }
        }
    }

}  // namespace

//------------------------------------------------------------------------------
// Public API
//------------------------------------------------------------------------------

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

    // Register event handlers for provisioning display states
    esp_event_handler_register(PROTOCOMM_TRANSPORT_BLE_EVENT, ESP_EVENT_ANY_ID,
        ble_event_handler, nullptr);
    esp_event_handler_register(WIFI_PROV_EVENT, ESP_EVENT_ANY_ID,
        prov_event_handler, nullptr);
    esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_START,
        wifi_event_handler, nullptr);

    ESP_LOGI(TAG, "Display initialized");
}

void display_render_rgba_frame(const uint8_t* rgba_frame, int width, int height) {
#if DISPLAY_ENABLED
    if (!disp.dma_display || !rgba_frame) return;

    int px = 0;
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            disp.dma_display->drawPixelRGB888(x, y,
                rgba_frame[px * 4],
                rgba_frame[px * 4 + 1],
                rgba_frame[px * 4 + 2]);
            px++;
        }
    }

    // Draw status bar overlay if enabled
    if (disp.status_bar.enabled) {
        disp.dma_display->drawFastHLine(0, 0, width,
            disp.status_bar.r, disp.status_bar.g, disp.status_bar.b);
    }
#endif
}

void display_render_rgb_buffer(const uint8_t* rgb_buffer, size_t buffer_len) {
#if DISPLAY_ENABLED
    if (!disp.dma_display) return;

    disp.dma_display->fillScreen(0);

    if (buffer_len != CONFIG_MATRIX_WIDTH * CONFIG_MATRIX_HEIGHT * 3) {
        return;
    }

    for (int y = 0; y < CONFIG_MATRIX_HEIGHT; y++) {
        for (int x = 0; x < CONFIG_MATRIX_WIDTH; x++) {
            int px = (y * CONFIG_MATRIX_WIDTH + x) * 3;
            disp.dma_display->drawPixelRGB888(x, y,
                rgb_buffer[px], rgb_buffer[px + 1], rgb_buffer[px + 2]);
        }
    }

    // Draw status bar overlay if enabled
    if (disp.status_bar.enabled) {
        disp.dma_display->drawFastHLine(0, 0, CONFIG_MATRIX_WIDTH,
            disp.status_bar.r, disp.status_bar.g, disp.status_bar.b);
    }
#endif
}

void display_clear() {
#if DISPLAY_ENABLED
    if (disp.dma_display != nullptr) {
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

DisplayStatusBar_t display_get_status_bar() {
    return disp.status_bar;
}

size_t display_get_buffer_size() {
    return CONFIG_MATRIX_WIDTH * CONFIG_MATRIX_HEIGHT * 3;
}

void display_get_dimensions(int* width, int* height) {
    if (width) *width = CONFIG_MATRIX_WIDTH;
    if (height) *height = CONFIG_MATRIX_HEIGHT;
}

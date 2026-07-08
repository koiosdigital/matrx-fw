#include "display.h"
#include "sdkconfig.h"
#include "webp_player.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <esp_log.h>
#include <esp_event.h>
#include <esp_wifi.h>
#include <network_provisioning/manager.h>
#include <protocomm_ble.h>

#include <kd_common.h>
#include <kd_console.h>
#include <esp_heap_caps.h>
#include <driver/gpio.h>

#include <cstring>
#include <cstdio>
#include <cstdlib>

#include "sockets.h"
#include "hub75.h"

static const char* TAG = "display";

namespace {

    Hub75Config display_cfg = {
        .panel_width = CONFIG_MATRIX_WIDTH,
        .panel_height = CONFIG_MATRIX_HEIGHT,
        .shift_driver = Hub75ShiftDriver::GENERIC,
        .pins = {
            .r1 = CONFIG_R1_PIN,
            .g1 = CONFIG_G1_PIN,
            .b1 = CONFIG_B1_PIN,
            .r2 = CONFIG_R2_PIN,
            .g2 = CONFIG_G2_PIN,
            .b2 = CONFIG_B2_PIN,
            .a = CONFIG_A_PIN,
            .b = CONFIG_B_PIN,
            .c = CONFIG_C_PIN,
            .d = CONFIG_D_PIN,
            .e = CONFIG_E_PIN,
            .lat = CONFIG_LAT_PIN,
            .oe = CONFIG_OE_PIN,
            .clk = CONFIG_CLK_PIN
        },
        .gpio_drive_strength = 1,
    };

    Hub75Driver dma_display(display_cfg);

    constexpr TickType_t BOOT_SPRITE_DELAY_MS = 1200;

    constexpr uint8_t font_5x7[][7] = {
        {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E},
        {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E},
        {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F},
        {0x0E, 0x11, 0x01, 0x0E, 0x01, 0x11, 0x0E},
        {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02},
        {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E},
        {0x0E, 0x10, 0x10, 0x1E, 0x11, 0x11, 0x0E},
        {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08},
        {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E},
        {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x01, 0x0E},
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
        char* pop_token = kd_common_provisioning_get_srp_password();
        if (pop_token == nullptr) {
            ESP_LOGE(TAG, "POP token is null");
            return;
        }

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
        heap_caps_free(display_buffer);
    }

#ifdef CONFIG_KD_COMMON_CONSOLE_ENABLE
    int cmd_matrix(int argc, char** argv) {
        if (argc == 3 && strcmp(argv[1], "drive") == 0) {
            int level = atoi(argv[2]);
            if (level < 0 || level > 3) {
                printf("usage: matrix drive <0-3>\n");
                return 1;
            }
            const int pins[] = {
                display_cfg.pins.r1, display_cfg.pins.g1, display_cfg.pins.b1,
                display_cfg.pins.r2, display_cfg.pins.g2, display_cfg.pins.b2,
                display_cfg.pins.a, display_cfg.pins.b, display_cfg.pins.c,
                display_cfg.pins.d, display_cfg.pins.e,
                display_cfg.pins.lat, display_cfg.pins.oe, display_cfg.pins.clk,
            };
            for (size_t i = 0; i < sizeof(pins) / sizeof(pins[0]); i++) {
                if (pins[i] >= 0) {
                    gpio_set_drive_capability((gpio_num_t)pins[i], (gpio_drive_cap_t)level);
                }
            }
            printf("matrix drive %d applied to %zu pins (until 'matrix on' or reboot)\n",
                level, sizeof(pins) / sizeof(pins[0]));
            return 0;
        }
        if (argc != 2 || (strcmp(argv[1], "on") != 0 && strcmp(argv[1], "off") != 0)) {
            printf("usage: matrix <on|off> | matrix drive <0-3>\n");
            return 1;
        }
        bool on = (strcmp(argv[1], "on") == 0);
        if (on == dma_display.is_running()) {
            printf("matrix already %s\n", argv[1]);
            return 0;
        }
        if (on) {
            dma_display.begin();
            dma_display.set_brightness(32);
            printf("matrix on (content resumes on next sprite/rotation; reboot for full restore)\n");
        } else {
            webp_player_stop();
            vTaskDelay(pdMS_TO_TICKS(250));
            dma_display.end();
            printf("matrix off — HUB75 DMA stopped, panel pins static\n");
        }
        return 0;
    }
#endif  // CONFIG_KD_COMMON_CONSOLE_ENABLE

    void ble_event_handler(void*, esp_event_base_t, int32_t event_id, void*) {
        if (event_id == PROTOCOMM_TRANSPORT_BLE_CONNECTED) {
            display_pop_code();
        }
        else if (event_id == PROTOCOMM_TRANSPORT_BLE_DISCONNECTED) {
            if (!kd_common_is_wifi_connected()) {
                webp_player_play_embedded("setup");
            }
        }
    }

    void prov_event_handler(void*, esp_event_base_t, int32_t event_id, void*) {
        if (event_id == NETWORK_PROV_START) {
            webp_player_play_embedded("setup");
        }
        else if (event_id == NETWORK_PROV_END) {
            if (sockets_is_connected()) return;

            wifi_config_t wifi_cfg;
            if (esp_wifi_get_config(WIFI_IF_STA, &wifi_cfg) == ESP_OK &&
                std::strlen(reinterpret_cast<const char*>(wifi_cfg.sta.ssid)) != 0) {
                webp_player_play_embedded("connecting");
            }
        }
    }

    void wifi_event_handler(void*, esp_event_base_t, int32_t event_id, void*) {
        if (event_id == WIFI_EVENT_STA_START) {
            wifi_config_t wifi_cfg;
            if (esp_wifi_get_config(WIFI_IF_STA, &wifi_cfg) == ESP_OK &&
                std::strlen(reinterpret_cast<const char*>(wifi_cfg.sta.ssid)) != 0) {
                webp_player_play_embedded("connecting");
            }
        }
    }

}  // namespace

void display_init() {
#if CONFIG_DISPLAY_ENABLED
    dma_display.begin();
    dma_display.set_brightness(32);
    dma_display.clear();
#endif

    esp_event_handler_register(PROTOCOMM_TRANSPORT_BLE_EVENT, ESP_EVENT_ANY_ID,
        ble_event_handler, nullptr);
    esp_event_handler_register(NETWORK_PROV_EVENT, ESP_EVENT_ANY_ID,
        prov_event_handler, nullptr);
    esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_START,
        wifi_event_handler, nullptr);
}

void display_register_console_cmds() {
#if CONFIG_DISPLAY_ENABLED && defined(CONFIG_KD_COMMON_CONSOLE_ENABLE)
    kd_console_register_cmd("matrix",
        "matrix <on|off> | matrix drive <0-3> - HUB75 DMA stop/start, GPIO drive strength (EMI testing)",
        cmd_matrix);
#endif
}

void display_deinit() {
    esp_event_handler_unregister(PROTOCOMM_TRANSPORT_BLE_EVENT, ESP_EVENT_ANY_ID, ble_event_handler);
    esp_event_handler_unregister(NETWORK_PROV_EVENT, ESP_EVENT_ANY_ID, prov_event_handler);
    esp_event_handler_unregister(WIFI_EVENT, WIFI_EVENT_STA_START, wifi_event_handler);
}

void display_render_rgba_frame(const uint8_t* rgba_frame, int width, int height) {
#if CONFIG_DISPLAY_ENABLED
    if (!rgba_frame || width <= 0 || height <= 0) return;
    if (width > CONFIG_MATRIX_WIDTH) width = CONFIG_MATRIX_WIDTH;
    if (height > CONFIG_MATRIX_HEIGHT) height = CONFIG_MATRIX_HEIGHT;

    dma_display.draw_pixels(0, 0, static_cast<uint16_t>(width), static_cast<uint16_t>(height),
        rgba_frame, Hub75PixelFormat::RGB888_32, Hub75ColorOrder::BGR);
#endif
}

void display_render_rgba_span(const uint8_t* rgba_span, int x, int y, int width) {
#if CONFIG_DISPLAY_ENABLED
    if (!rgba_span || width <= 0) return;
    if (x < 0 || y < 0 || x >= CONFIG_MATRIX_WIDTH || y >= CONFIG_MATRIX_HEIGHT) return;
    if (x + width > CONFIG_MATRIX_WIDTH) width = CONFIG_MATRIX_WIDTH - x;

    dma_display.draw_pixels(static_cast<uint16_t>(x), static_cast<uint16_t>(y),
        static_cast<uint16_t>(width), 1,
        rgba_span, Hub75PixelFormat::RGB888_32, Hub75ColorOrder::BGR);
#endif
}

void display_render_rgb_buffer(const uint8_t* rgb_buffer, size_t buffer_len) {
#if CONFIG_DISPLAY_ENABLED
    if (buffer_len != CONFIG_MATRIX_WIDTH * CONFIG_MATRIX_HEIGHT * 3) {
        return;
    }

    dma_display.draw_pixels(0, 0, CONFIG_MATRIX_WIDTH, CONFIG_MATRIX_HEIGHT,
        rgb_buffer, Hub75PixelFormat::RGB888);
#endif
}

void display_clear() {
#if CONFIG_DISPLAY_ENABLED
    dma_display.clear();
#endif
}

void display_set_brightness(uint8_t brightness) {
#if CONFIG_DISPLAY_ENABLED
    dma_display.set_brightness(brightness);
#else
    ESP_LOGW(TAG, "Display is not enabled, cannot set brightness");
#endif
}

size_t display_get_buffer_size() {
    return CONFIG_MATRIX_WIDTH * CONFIG_MATRIX_HEIGHT * 3;
}

void display_get_dimensions(int* width, int* height) {
    if (width) *width = CONFIG_MATRIX_WIDTH;
    if (height) *height = CONFIG_MATRIX_HEIGHT;
}

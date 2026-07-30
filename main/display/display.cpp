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
#include <nvs_flash.h>
#include <nvs.h>

#include <cstring>
#include <cstdio>
#include <cstdlib>

#include "sockets.h"
#include "scheduler.h"
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
        .output_clock_speed = Hub75ClockSpeed::HZ_20M,
        .gpio_drive_strength = 1,
    };

    Hub75Driver dma_display(display_cfg);

    // ---- Panel hardware tuning, persisted in NVS (survives factory reset) ----
    constexpr const char* HW_NVS_NAMESPACE = "disp_hw";
    constexpr const char* HW_KEY_COLOR_ORDER = "color_order";  // u8: 0=rgb, 1=bgr (R/B swapped panel)
    constexpr const char* HW_KEY_BIT_DEPTH = "bit_depth";      // u8: 4-12, 0=compile default
    constexpr const char* HW_KEY_CLK_MHZ = "clk_mhz";          // u8: 8|10|16|20|32, 0=default (20)

    // Panel color order: false = normal panel (default), true = R/B swapped.
    bool g_panel_bgr = false;

    // Buffer color order handed to draw_pixels. On a normal panel the webp
    // RGBA paths use BGR (matches the driver's byte layout for RGB888_32) and
    // the packed-RGB path uses RGB; a swapped panel flips both.
    inline Hub75ColorOrder rgba_draw_order() {
        return g_panel_bgr ? Hub75ColorOrder::RGB : Hub75ColorOrder::BGR;
    }
    inline Hub75ColorOrder rgb_draw_order() {
        return g_panel_bgr ? Hub75ColorOrder::BGR : Hub75ColorOrder::RGB;
    }

    bool clock_speed_from_mhz(int mhz, Hub75ClockSpeed* out) {
        switch (mhz) {
        case 8:  *out = Hub75ClockSpeed::HZ_8M;  return true;
        case 10: *out = Hub75ClockSpeed::HZ_10M; return true;
        case 16: *out = Hub75ClockSpeed::HZ_16M; return true;
        case 20: *out = Hub75ClockSpeed::HZ_20M; return true;
        case 32: *out = Hub75ClockSpeed::HZ_32M; return true;
        default: return false;
        }
    }

    void hw_settings_load() {
        // display_init runs before kd_common_init, so bring NVS up ourselves;
        // nvs_flash_init is idempotent. On failure just use defaults.
        esp_err_t err = nvs_flash_init();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "NVS init: %s, using default panel settings", esp_err_to_name(err));
            return;
        }

        nvs_handle_t h;
        if (nvs_open(HW_NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return;

        uint8_t v = 0;
        if (nvs_get_u8(h, HW_KEY_COLOR_ORDER, &v) == ESP_OK) {
            g_panel_bgr = (v != 0);
        }
        if (nvs_get_u8(h, HW_KEY_BIT_DEPTH, &v) == ESP_OK && v >= 4 && v <= 12) {
            display_cfg.bit_depth = v;
        }
        if (nvs_get_u8(h, HW_KEY_CLK_MHZ, &v) == ESP_OK) {
            Hub75ClockSpeed speed;
            if (clock_speed_from_mhz(v, &speed)) {
                display_cfg.output_clock_speed = speed;
            }
        }
        nvs_close(h);

        ESP_LOGI(TAG, "Panel settings: color_order=%s bit_depth=%u clk=%luMHz",
                 g_panel_bgr ? "bgr" : "rgb",
                 display_cfg.bit_depth ? display_cfg.bit_depth : HUB75_BIT_DEPTH,
                 (unsigned long)((uint32_t)display_cfg.output_clock_speed / 1000000));
    }

    bool hw_setting_save(const char* key, uint8_t value) {
        nvs_handle_t h;
        if (nvs_open(HW_NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return false;
        bool ok = (nvs_set_u8(h, key, value) == ESP_OK) && (nvs_commit(h) == ESP_OK);
        nvs_close(h);
        if (!ok) ESP_LOGE(TAG, "Failed to save %s to NVS", key);
        return ok;
    }

    // Tear down the HUB75 driver and re-create it from the current display_cfg.
    // Quiesces the render pipeline first so nothing draws into the DMA engine
    // mid-teardown; restores the scheduler unless something else (quiet hours)
    // had already paused it.
    bool display_reinit() {
#if CONFIG_DISPLAY_ENABLED
        bool was_paused = scheduler_is_paused();
        scheduler_pause();

        // Wait for the webp player task to finish any in-flight frame.
        for (int i = 0; i < 100 && webp_player_is_playing(); i++) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        vTaskDelay(pdMS_TO_TICKS(20));

        display_cfg.brightness = dma_display.get_brightness();
        dma_display.end();
        dma_display.set_config(display_cfg);
        bool ok = dma_display.begin();
        if (ok) {
            dma_display.clear();
        } else {
            ESP_LOGE(TAG, "Display re-init failed");
        }

        if (!was_paused) {
            scheduler_resume();
        }
        return ok;
#else
        return false;
#endif
    }

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

#ifdef CONFIG_KD_COMMON_CONSOLE_ENABLE

    int cmd_set_color_order(int argc, char** argv) {
        if (argc < 2) {
            printf("color_order: %s\nusage: set_color_order <rgb|bgr>\n",
                g_panel_bgr ? "bgr" : "rgb");
            return 0;
        }

        bool bgr;
        if (strcmp(argv[1], "rgb") == 0) bgr = false;
        else if (strcmp(argv[1], "bgr") == 0) bgr = true;
        else {
            printf("invalid color order '%s' (rgb|bgr)\n", argv[1]);
            return 1;
        }

        g_panel_bgr = bgr;
        hw_setting_save(HW_KEY_COLOR_ORDER, bgr ? 1 : 0);
        printf("color_order = %s (applied to subsequent frames)\n", bgr ? "bgr" : "rgb");
        return 0;
    }

    int cmd_set_bit_depth(int argc, char** argv) {
        if (argc < 2) {
            printf("bit_depth: %u (0 = compile default %u)\nusage: set_bit_depth <4-12>\n",
                display_cfg.bit_depth, HUB75_BIT_DEPTH);
            return 0;
        }

        int depth = atoi(argv[1]);
        if (depth < 4 || depth > 12) {
            printf("bit depth must be 4-12\n");
            return 1;
        }

        display_cfg.bit_depth = static_cast<uint8_t>(depth);
        hw_setting_save(HW_KEY_BIT_DEPTH, static_cast<uint8_t>(depth));
        printf("bit_depth = %d, re-initializing display...\n", depth);
        return display_reinit() ? 0 : 1;
    }

    int cmd_set_clock_speed(int argc, char** argv) {
        if (argc < 2) {
            printf("clock_speed: %luMHz\nusage: set_clock_speed <8|10|16|20|32>\n",
                (unsigned long)((uint32_t)display_cfg.output_clock_speed / 1000000));
            return 0;
        }

        int mhz = atoi(argv[1]);
        Hub75ClockSpeed speed;
        if (!clock_speed_from_mhz(mhz, &speed)) {
            printf("clock speed must be 8, 10, 16, 20 or 32 (MHz)\n");
            return 1;
        }

        display_cfg.output_clock_speed = speed;
        hw_setting_save(HW_KEY_CLK_MHZ, static_cast<uint8_t>(mhz));
        printf("clock_speed = %dMHz, re-initializing display...\n", mhz);
        return display_reinit() ? 0 : 1;
    }

#endif  // CONFIG_KD_COMMON_CONSOLE_ENABLE

}  // namespace

void display_init() {
#if CONFIG_DISPLAY_ENABLED
    hw_settings_load();
    dma_display.set_config(display_cfg);
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
#ifdef CONFIG_KD_COMMON_CONSOLE_ENABLE
    kd_console_register_cmd("set_color_order",
        "Panel color order (rgb|bgr), stored to NVS, applies live", cmd_set_color_order);
    kd_console_register_cmd("set_bit_depth",
        "Panel color bit depth 4-12 (0 arg prints current), stored to NVS, re-inits display", cmd_set_bit_depth);
    kd_console_register_cmd("set_clock_speed",
        "HUB75 clock speed in MHz (8|10|16|20|32), stored to NVS, re-inits display", cmd_set_clock_speed);
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
        rgba_frame, Hub75PixelFormat::RGB888_32, rgba_draw_order());
#endif
}

void display_render_rgba_span(const uint8_t* rgba_span, int x, int y, int width) {
#if CONFIG_DISPLAY_ENABLED
    if (!rgba_span || width <= 0) return;
    if (x < 0 || y < 0 || x >= CONFIG_MATRIX_WIDTH || y >= CONFIG_MATRIX_HEIGHT) return;
    if (x + width > CONFIG_MATRIX_WIDTH) width = CONFIG_MATRIX_WIDTH - x;

    dma_display.draw_pixels(static_cast<uint16_t>(x), static_cast<uint16_t>(y),
        static_cast<uint16_t>(width), 1,
        rgba_span, Hub75PixelFormat::RGB888_32, rgba_draw_order());
#endif
}

void display_render_rgb_buffer(const uint8_t* rgb_buffer, size_t buffer_len) {
#if CONFIG_DISPLAY_ENABLED
    if (buffer_len != CONFIG_MATRIX_WIDTH * CONFIG_MATRIX_HEIGHT * 3) {
        return;
    }

    dma_display.draw_pixels(0, 0, CONFIG_MATRIX_WIDTH, CONFIG_MATRIX_HEIGHT,
        rgb_buffer, Hub75PixelFormat::RGB888, rgb_draw_order());
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

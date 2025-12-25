#include "display.h"
#include "pinout.h"

#include "freertos/task.h"
#include "freertos/queue.h"

#include "esp_log.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "wifi_provisioning/manager.h"
#include "protocomm_ble.h"

#include "webp/demux.h"
#include "qrcode.h"
#include "kd_common.h"
#include "sprites.h"

static const char* TAG = "display";

MatrixPanel_I2S_DMA* dma_display = nullptr;

TaskHandle_t xDecoderTask = NULL;
SemaphoreHandle_t xWebPSemaphore = NULL;

WebPData webp_data = {
    .bytes = NULL,
    .size = 0
};

WebPAnimDecoder* dec = NULL;
WebPAnimInfo anim_info;

DisplayStatusBar_t current_status_bar = {
    .enabled = false,
    .r = 0,
    .g = 0,
    .b = 0
};

void IRAM_ATTR destroy_decoder() {
    if (dec != NULL) {
        if (xSemaphoreTake(xWebPSemaphore, portMAX_DELAY) == pdTRUE) {
            WebPAnimDecoderDelete(dec);
            dec = NULL;
        }
        xSemaphoreGive(xWebPSemaphore);
    }
}

esp_err_t IRAM_ATTR start_decoder() {
    destroy_decoder();

    if (webp_data.bytes == NULL || webp_data.size == 0) {
        ESP_LOGE(TAG, "no sprite data");
        return ESP_ERR_INVALID_ARG;
    }

    if (xSemaphoreTake(xWebPSemaphore, portMAX_DELAY) == pdTRUE) {
        dec = WebPAnimDecoderNew(&webp_data, NULL);
        if (dec == NULL) {
            ESP_LOGE(TAG, "failed to create WebP decoder");
            xSemaphoreGive(xWebPSemaphore);
            return ESP_FAIL;
        }

        if (!WebPAnimDecoderGetInfo(dec, &anim_info)) {
            ESP_LOGE(TAG, "failed to get WebP animation info");
            WebPAnimDecoderDelete(dec);
            dec = NULL;
            xSemaphoreGive(xWebPSemaphore);
            return ESP_FAIL;
        }

        xSemaphoreGive(xWebPSemaphore);
    }
    else {
        ESP_LOGE(TAG, "failed to take semaphore for decoder start");
        return ESP_ERR_TIMEOUT;
    }

    xTaskNotify(xDecoderTask, WebPTaskNotification_t::WEBP_START, eSetValueWithOverwrite);
    return ESP_OK;
}

void IRAM_ATTR decoder_task(void* pvParameter) {
    WebPTaskNotification_t notification;
    bool active = false;

    TickType_t anim_start_tick = 0;
    uint8_t* frame_buffer = NULL;
    int last_timestamp = 0;

    while (1) {
        //wait for notification, does not run if we're currently displaying a sprite on the display
        if (!active && xTaskNotifyWait(0, ULONG_MAX, (uint32_t*)&notification, portMAX_DELAY) == pdTRUE) {
            if (notification == WebPTaskNotification_t::WEBP_START) {
                ESP_LOGI(TAG, "decoder notified to start");
                active = true;
            }
        }

        if (xSemaphoreTake(xWebPSemaphore, pdMS_TO_TICKS(100)) == pdTRUE) {
            //check if someone's destroyed our decoder
            if (dec == NULL) {
                xSemaphoreGive(xWebPSemaphore);
                active = false;
                continue;
            }

            // Quick operations only while holding semaphore
            bool has_more_frames = WebPAnimDecoderHasMoreFrames(dec);
            bool get_next_success = false;
            int timestamp;

            if (!has_more_frames) {
                WebPAnimDecoderReset(dec);
                // Reset timing variables here while holding lock
                anim_start_tick = 0;
                last_timestamp = 0;
            }

            if (anim_start_tick == 0) {
                anim_start_tick = xTaskGetTickCount();
                last_timestamp = 0;  // Ensure first frame starts from 0
            }

            get_next_success = WebPAnimDecoderGetNext(dec, &frame_buffer, &timestamp);
            xSemaphoreGive(xWebPSemaphore);  // Release ASAP

            if (!get_next_success) {
                // Handle error without holding semaphore
                ESP_LOGE(TAG, "error getting next frame, restarting decoder");
                destroy_decoder();  // This will handle semaphore internally

                // Restart with timeout to prevent infinite loops
                esp_err_t err = start_decoder();
                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "Failed to restart decoder, stopping");
                    active = false;
                }
                continue;
            }

            //display the frame
            if (frame_buffer == NULL) {
                ESP_LOGE(TAG, "frame buffer is null");
                active = false;
                continue;
            }

#if DISPLAY_ENABLED
            int px = 0;
            for (int y = 0; y < CONFIG_MATRIX_HEIGHT; y++) {
                for (int x = 0; x < CONFIG_MATRIX_WIDTH; x++) {
                    dma_display->drawPixelRGB888(x, y, frame_buffer[px * 4], frame_buffer[px * 4 + 1], frame_buffer[px * 4 + 2]);
                    px++;
                }
            }

            if (current_status_bar.enabled) {
                dma_display->drawFastHLine(0, 0, CONFIG_MATRIX_WIDTH, current_status_bar.r, current_status_bar.g, current_status_bar.b);
            }
#endif

            //wait to display frame if necessary
            int delay = timestamp - last_timestamp;
            last_timestamp = timestamp;

            // If there's only 1 frame, ensure a 1-second delay as fallback to prevent task starvation
            if (anim_info.frame_count == 1 && delay <= 0) {
                delay = 1000; // 1 second in milliseconds
            }

            if (delay > 0) {
                xTaskDelayUntil(&anim_start_tick, pdMS_TO_TICKS(delay));
            }
        }
    }
}

// Simple 5x7 monospaced font for digits 1-6
static const uint8_t font_5x7[][7] = {
    {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}, // 0
    {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E}, // 1
    {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F}, // 2
    {0x0E, 0x11, 0x01, 0x0E, 0x01, 0x11, 0x0E}, // 3
    {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}, // 4
    {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E}, // 5
    {0x0E, 0x10, 0x10, 0x1E, 0x11, 0x11, 0x0E}, // 6
};

void draw_char_scaled(uint8_t* buffer, int buf_width, int buf_height, char c, int x, int y, int scale, uint8_t r, uint8_t g, uint8_t b) {
    if (c < '0' || c > '6') {
        return; // Only support digits 0-6
    }

    int char_index = c - '0';
    const uint8_t* char_data = font_5x7[char_index];

    for (int row = 0; row < 7; row++) {
        uint8_t row_data = char_data[row];
        for (int col = 0; col < 5; col++) {
            if (row_data & (1 << (4 - col))) {
                // Draw scaled pixel
                for (int sy = 0; sy < scale; sy++) {
                    for (int sx = 0; sx < scale; sx++) {
                        int px = x + col * scale + sx;
                        int py = y + row * scale + sy;
                        if (px >= 0 && px < buf_width && py >= 0 && py < buf_height) {
                            size_t idx = ((size_t)py * (size_t)buf_width + (size_t)px) * 3;
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

void prov_display_pop_code() {
    char* pop_token = kd_common_provisioning_get_pop_token();
    if (pop_token == NULL) {
        ESP_LOGE(TAG, "POP token is NULL");
        return;
    }

    int w, h;
    get_display_dimensions(&w, &h);

    uint8_t* display_buffer = (uint8_t*)heap_caps_calloc(get_display_buffer_size(), sizeof(uint8_t), MALLOC_CAP_SPIRAM);
    if (display_buffer == NULL) {
        ESP_LOGE(TAG, "malloc failed: display buffer");
        return;
    }
    memset(display_buffer, 0, get_display_buffer_size());

    // Calculate optimal scale and positioning for 6 digits on 64x32 display with padding
    // Scale 1: 6 chars * 5px + 5 gaps * 2px = 40px width, leaving 12px padding on each side
    // Height: 7px tall, centered in 32px height with 12-13px padding top/bottom
    int token_len = strlen(pop_token);
    int scale = 1; // 1x scale provides good padding
    int char_width = 5 * scale;
    int char_height = 7 * scale;
    int spacing = 2; // 2px gap between characters for readability

    int total_width = token_len * char_width + (token_len - 1) * spacing;
    int x_start = (w - total_width) / 2;
    int y_start = (h - char_height) / 2;

    // Draw each character
    for (int i = 0; i < token_len; i++) {
        int x = x_start + i * (char_width + spacing);
        draw_char_scaled(display_buffer, w, h, pop_token[i], x, y_start, scale, 255, 255, 255);
    }

    display_raw_buffer(display_buffer, get_display_buffer_size());
    free(display_buffer);

    ESP_LOGI(TAG, "Displaying POP code: %s", pop_token);
}

void wifi_prov_connected(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    prov_display_pop_code();
}

void wifi_prov_disconnected(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    // If BLE disconnects while provisioning is still active and we're not connected to WiFi,
    // show the setup sprite again
    if (!kd_common_is_wifi_connected()) {
        ESP_LOGI(TAG, "BLE disconnected during provisioning, showing setup sprite");
        show_fs_sprite("setup");
    }
}

void wifi_prov_started(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    show_fs_sprite("setup");
}

void wifi_prov_ended(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    wifi_config_t wifi_cfg;
    if (esp_wifi_get_config(WIFI_IF_STA, &wifi_cfg) != ESP_OK) {
        return;
    }

    if (strlen((const char*)wifi_cfg.sta.ssid) != 0) {
        show_fs_sprite("connecting_wifi");
    }
}

void provisioning_event_handler2(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
        case WIFI_EVENT_STA_START: {
            wifi_config_t wifi_cfg;
            esp_wifi_get_config(WIFI_IF_STA, &wifi_cfg);

            if (strlen((const char*)wifi_cfg.sta.ssid) != 0) {
                show_fs_sprite("connecting_wifi");
                break;
            }
            break;
        }

        }
    }
}

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
    dma_display = new MatrixPanel_I2S_DMA(mxconfig);

    dma_display->begin();
    dma_display->setBrightness(32);
    dma_display->fillScreenRGB888(0, 0, 0);
#endif

    xWebPSemaphore = xSemaphoreCreateMutex();

    xTaskCreatePinnedToCore(decoder_task, "decoder", 4096, NULL, 10, &xDecoderTask, 1);

    //Display QR code once connected to endpoint device
    esp_event_handler_register(PROTOCOMM_TRANSPORT_BLE_EVENT, PROTOCOMM_TRANSPORT_BLE_CONNECTED, &wifi_prov_connected, NULL);
    esp_event_handler_register(PROTOCOMM_TRANSPORT_BLE_EVENT, PROTOCOMM_TRANSPORT_BLE_DISCONNECTED, &wifi_prov_disconnected, NULL);
    esp_event_handler_register(WIFI_PROV_EVENT, WIFI_PROV_START, &wifi_prov_started, NULL);
    esp_event_handler_register(WIFI_PROV_EVENT, WIFI_PROV_END, &wifi_prov_ended, NULL);
    esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_START, &provisioning_event_handler2, NULL);

    show_fs_sprite("boot");
    vTaskDelay(pdMS_TO_TICKS(1200));
}

esp_err_t IRAM_ATTR display_sprite(uint8_t* p_sprite_buf, size_t sprite_buf_len) {
    if (p_sprite_buf == NULL || sprite_buf_len == 0) {
        ESP_LOGE(TAG, "invalid sprite buffer");
        return ESP_ERR_INVALID_ARG;
    }

    if (p_sprite_buf == webp_data.bytes && sprite_buf_len == webp_data.size) {
        return ESP_OK;
    }

    destroy_decoder();

    webp_data.bytes = p_sprite_buf;
    webp_data.size = sprite_buf_len;

    // Start decoder with limited retries
    esp_err_t err;
    int retry_count = 0;
    const int max_retries = 3;

    do {
        err = start_decoder();
        if (err != ESP_OK) {
            retry_count++;
            ESP_LOGW(TAG, "Failed to start decoder, retry %d/%d", retry_count, max_retries);
            vTaskDelay(pdMS_TO_TICKS(200));
        }
    } while (err != ESP_OK && retry_count < max_retries);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start decoder after %d retries", max_retries);
        return err;
    }

    return ESP_OK;
}

//Display a raw framebuffer on the screen. Stops the WebP decoder.
//Buffer is expected to be in RGB format. Buffer is freed after display.
void display_raw_buffer(uint8_t* p_raw_buf, size_t raw_buf_len) {
    destroy_decoder();

    webp_data.bytes = NULL;
    webp_data.size = 0;

#if DISPLAY_ENABLED
    dma_display->fillScreen(0);

    if (raw_buf_len != CONFIG_MATRIX_WIDTH * CONFIG_MATRIX_HEIGHT * 3) {
        return;
    }

    for (int y = 0; y < CONFIG_MATRIX_HEIGHT; y++) {
        for (int x = 0; x < CONFIG_MATRIX_WIDTH; x++) {
            int px = (y * CONFIG_MATRIX_WIDTH + x) * 3;
            dma_display->drawPixelRGB888(x, y, p_raw_buf[px], p_raw_buf[px + 1], p_raw_buf[px + 2]);
        }
    }
#endif
}

void display_clear() {
#if DISPLAY_ENABLED
    if (dma_display != nullptr) {
        destroy_decoder();
        dma_display->fillScreenRGB888(0, 0, 0);
    }
#endif
}

void display_set_brightness(uint8_t brightness) {
#if DISPLAY_ENABLED
    if (dma_display != nullptr) {
        dma_display->setBrightness(brightness);
    }
#else
    ESP_LOGW(TAG, "Display is not enabled, cannot set brightness");
#endif
}

void display_clear_status_bar() {
    current_status_bar.enabled = false;
}

void display_set_status_bar(uint8_t r, uint8_t g, uint8_t b) {
    current_status_bar.enabled = true;
    current_status_bar.r = r;
    current_status_bar.g = g;
    current_status_bar.b = b;
}

size_t get_display_buffer_size() {
    return CONFIG_MATRIX_WIDTH * CONFIG_MATRIX_HEIGHT * 3;
}

void get_display_dimensions(int* w, int* h) {
    *w = CONFIG_MATRIX_WIDTH;
    *h = CONFIG_MATRIX_HEIGHT;
}

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

void destroy_decoder() {
    if (dec != NULL) {
        if (xSemaphoreTake(xWebPSemaphore, portMAX_DELAY) == pdTRUE) {
            WebPAnimDecoderDelete(dec);
            dec = NULL;
        }
        xSemaphoreGive(xWebPSemaphore);
    }
}

void start_decoder() {
    destroy_decoder();

    if (webp_data.bytes == NULL || webp_data.size == 0) {
        ESP_LOGE(TAG, "no sprite data");
        return;
    }

    dec = WebPAnimDecoderNew(&webp_data, NULL);
    WebPAnimDecoderGetInfo(dec, &anim_info);

    xTaskNotify(xDecoderTask, WebPTaskNotification_t::WEBP_START, eSetValueWithOverwrite);
}

void decoder_task(void* pvParameter) {
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

        if (xSemaphoreTake(xWebPSemaphore, portMAX_DELAY) == pdTRUE) {
            //check if someone's destroyed our decoder
            if (dec == NULL) {
                active = false;
                continue;
            }

            if (!WebPAnimDecoderHasMoreFrames(dec)) {
                WebPAnimDecoderReset(dec);
                anim_start_tick = 0;
            }

            if (anim_start_tick == 0) {
                anim_start_tick = xTaskGetTickCount();
            }

            int timestamp;

            if (!WebPAnimDecoderGetNext(dec, &frame_buffer, &timestamp)) {
                ESP_LOGE(TAG, "error getting next frame");
                WebPAnimDecoderReset(dec);
                anim_start_tick = 0;
                xSemaphoreGive(xWebPSemaphore);
                continue;
            }

            xSemaphoreGive(xWebPSemaphore);

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
            if (delay > 0) {
                xTaskDelayUntil(&anim_start_tick, pdMS_TO_TICKS(delay));
            }
        }
    }
}

uint8_t* qr_display_buffer = NULL;
void prov_display_qr_helper(esp_qrcode_handle_t qrcode) {
    //We want to center the QR code on the display
    int qr_size = esp_qrcode_get_size(qrcode);
    int w, h = 0;
    get_display_dimensions(&w, &h);

    int x_offset = (w - qr_size) / 2;
    int y_offset = (h - qr_size) / 2;

    for (int y = 0; y < qr_size; y++) {
        for (int x = 0; x < qr_size; x++) {
            if (esp_qrcode_get_module(qrcode, x, y)) {
                qr_display_buffer[((y + y_offset) * w + (x + x_offset)) * 3] = 255;
                qr_display_buffer[((y + y_offset) * w + (x + x_offset)) * 3 + 1] = 255;
                qr_display_buffer[((y + y_offset) * w + (x + x_offset)) * 3 + 2] = 255;
            }
        }
    }

    display_raw_buffer(qr_display_buffer, get_display_buffer_size());
}

void prov_display_qr() {
    esp_qrcode_config_t qr_config = {
        .display_func = prov_display_qr_helper,
        .max_qrcode_version = 40,
        .qrcode_ecc_level = ESP_QRCODE_ECC_MED,
    };

    qr_display_buffer = (uint8_t*)heap_caps_calloc(get_display_buffer_size(), sizeof(uint8_t), MALLOC_CAP_SPIRAM);
    if (qr_display_buffer == NULL) {
        ESP_LOGE(TAG, "malloc failed: display buffer");
        return;
    }
    memset(qr_display_buffer, 0, get_display_buffer_size());

    esp_err_t err = esp_qrcode_generate(&qr_config, kd_common_get_provisioning_qr_payload());
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "QR code generation failed");
    }
}

void wifi_prov_connected(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    prov_display_qr();
}

void wifi_prov_started(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    show_fs_sprite("/fs/ble_prov.webp");
}

void provisioning_event_handler2(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    static int wifiConnectionAttempts = 0;
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
        case WIFI_EVENT_STA_START: {
            wifi_config_t wifi_cfg;
            esp_wifi_get_config(WIFI_IF_STA, &wifi_cfg);

            if (strlen((const char*)wifi_cfg.sta.ssid) != 0) {
                show_fs_sprite("/fs/connect_wifi.webp");
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

    xWebPSemaphore = xSemaphoreCreateBinary();
    xSemaphoreGive(xWebPSemaphore);

    xTaskCreatePinnedToCore(decoder_task, "decoder", 4096, NULL, 3, &xDecoderTask, 1);

    //Display QR code once connected to endpoint device
    esp_event_handler_register(PROTOCOMM_TRANSPORT_BLE_EVENT, PROTOCOMM_TRANSPORT_BLE_CONNECTED, &wifi_prov_connected, NULL);
    esp_event_handler_register(WIFI_PROV_EVENT, WIFI_PROV_START, &wifi_prov_started, NULL);
    esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_START, &provisioning_event_handler2, NULL);

    show_fs_sprite("/fs/boot.webp");
    vTaskDelay(pdMS_TO_TICKS(1200));
}

esp_err_t display_sprite(uint8_t* p_sprite_buf, size_t sprite_buf_len) {
    destroy_decoder();

    webp_data.bytes = p_sprite_buf;
    webp_data.size = sprite_buf_len;

    start_decoder();

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
        free(p_raw_buf);
        return;
    }

    for (int y = 0; y < CONFIG_MATRIX_HEIGHT; y++) {
        for (int x = 0; x < CONFIG_MATRIX_WIDTH; x++) {
            int px = (y * CONFIG_MATRIX_WIDTH + x) * 3;
            dma_display->drawPixelRGB888(x, y, p_raw_buf[px], p_raw_buf[px + 1], p_raw_buf[px + 2]);
        }
    }
#endif

    free(p_raw_buf);
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

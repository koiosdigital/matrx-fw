#include "display.h"

#include "freertos/task.h"
#include "freertos/queue.h"

#include "esp_log.h"

#include "webp/demux.h"

#include "nvs_flash.h"

MatrixPanel_I2S_DMA* dma_display = nullptr;
static const char* TAG = "display";

uint16_t width = MATRIX_WIDTH;
uint16_t height = MATRIX_HEIGHT;

struct DisplayStatusBar_t {
    bool enabled = false;
    uint8_t r = 0;
    uint8_t g = 0;
    uint8_t b = 0;
} current_status_bar;

typedef enum WebPTaskNotification_t {
    WEBP_STOP = 1,
    WEBP_START = 2,
} WebPTaskNotification_t;

TaskHandle_t xWebPTask = nullptr;

WebPData webp_data = {
    .bytes = nullptr,
    .size = 0
};

void webp_task(void* pvParameter) {
    WebPAnimDecoder* dec = nullptr;

    TickType_t animation_start_tick = 0;
    TickType_t next_frame_tick = 0;
    unsigned long current_frame_index = 0;
    bool animation_started = false;

    WebPTaskNotification_t notification;

    while (1) {
        TickType_t ticks_to_wait = animation_started ? pdMS_TO_TICKS(8) : portMAX_DELAY;

        if (xTaskNotifyWait(0, ULONG_MAX, (uint32_t*)&notification, ticks_to_wait) == pdTRUE) {
            if (notification == WebPTaskNotification_t::WEBP_START) {
                if (dec != nullptr) {
                    ESP_LOGD(TAG, "destroy decoder");
                    WebPAnimDecoderDelete(dec);
                    dec = nullptr;
                }

                if (webp_data.bytes == nullptr || webp_data.size == 0) {
                    ESP_LOGE(TAG, "no sprite data");
                    continue;
                }

                dec = WebPAnimDecoderNew(&webp_data, NULL);
                current_frame_index = -1;
                next_frame_tick = 0;
            }
            else if (notification == WebPTaskNotification_t::WEBP_STOP) {
                if (dec != nullptr) {
                    ESP_LOGD(TAG, "destroy decoder");
                    WebPAnimDecoderDelete(dec);
                    dec = nullptr;
                }
            }
        }

        //handle frame stepping
        if (dec != nullptr) {
            if (pdTICKS_TO_MS(xTaskGetTickCount()) - animation_start_tick >= next_frame_tick) {
                if (current_frame_index == 0) {
                    ESP_LOGD(TAG, "start animation");
                    animation_start_tick = xTaskGetTickCount();
                }

                if (WebPAnimDecoderHasMoreFrames(dec)) {
                    uint8_t* frame_buffer;
                    int timestamp;
                    if (WebPAnimDecoderGetNext(dec, &frame_buffer, &timestamp)) {
                        ESP_LOGV(TAG, "frame %lu", current_frame_index);
                        next_frame_tick = animation_start_tick + pdMS_TO_TICKS(timestamp);

                        int px = 0;
                        for (int y = 0; y < height; y++) {
                            for (int x = 0; x < width; x++) {
                                if (y = 0 && current_status_bar.enabled) {
                                    dma_display->drawPixelRGB888(x, 0, current_status_bar.r, current_status_bar.g, current_status_bar.b);
                                }
                                else {
                                    dma_display->drawPixelRGB888(x, y, frame_buffer[px * 4], frame_buffer[px * 4 + 1], frame_buffer[px * 4 + 2]);
                                }
                                px++;
                            }
                        }

                        current_frame_index++;

                        if (!WebPAnimDecoderHasMoreFrames(dec)) {
                            ESP_LOGD(TAG, "animation end");
                            current_frame_index = 0;
                            WebPAnimDecoderReset(dec);
                        }
                    }
                    else {
                        ESP_LOGE(TAG, "frame decode failed");
                        display_fill_rgb(0, 0, 0);
                        xTaskNotify(xWebPTask, WebPTaskNotification_t::WEBP_STOP, eSetValueWithOverwrite);
                    }
                }
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

    //open nvs
    nvs_handle handle;
    esp_err_t error = nvs_open(DISPLAY_CONFIG_NAMESPACE, NVS_READWRITE, &handle);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "nvs open failed");
        return;
    }

    error = nvs_get_u16(handle, "width", &width);
    if (error != ESP_OK) {
        if (error == ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "nvs width not found, set default");
            nvs_set_u16(handle, "width", width);
        }
        else {
            ESP_LOGE(TAG, "nvs get width failed");
            return;
        }
    }

    error = nvs_get_u16(handle, "height", &height);
    if (error != ESP_OK) {
        if (error == ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "nvs height not found, set default");
            nvs_set_u16(handle, "height", height);
        }
        else {
            ESP_LOGE(TAG, "nvs get height failed");
            return;
        }
    }

    nvs_commit(handle);
    nvs_close(handle);

    HUB75_I2S_CFG mxconfig(width, height, 1, pins);

    dma_display = new MatrixPanel_I2S_DMA(mxconfig);

    dma_display->begin();
    dma_display->setBrightness(32);
    dma_display->fillScreen(0);

    xTaskCreatePinnedToCore(webp_task, "webp", 2048, NULL, 5, &xWebPTask, 0);
}

void display_fill_rgb(uint8_t r, uint8_t g, uint8_t b) {
    dma_display->fillScreenRGB888(r, g, b);
}

esp_err_t display_sprite(uint8_t* p_sprite_buf, size_t sprite_buf_len) {
    xTaskNotify(xWebPTask, WebPTaskNotification_t::WEBP_STOP, eSetValueWithOverwrite);

    webp_data.bytes = p_sprite_buf;
    webp_data.size = sprite_buf_len;

    xTaskNotify(xWebPTask, WebPTaskNotification_t::WEBP_START, eSetValueWithOverwrite);

    return ESP_OK;
}

void display_clear_status_bar() {
    current_status_bar.enabled = false;

    for (int x = 0; x < width; x++) {
        dma_display->drawPixelRGB888(x, 0, 0, 0, 0);
    }
}

void display_set_status_bar(uint8_t r, uint8_t g, uint8_t b) {
    current_status_bar.enabled = true;
    current_status_bar.r = r;
    current_status_bar.g = g;
    current_status_bar.b = b;

    for (int x = 0; x < width; x++) {
        dma_display->drawPixelRGB888(x, 0, r, g, b);
    }
}
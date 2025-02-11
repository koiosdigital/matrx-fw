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

TaskHandle_t xDecoderTask = nullptr;
SemaphoreHandle_t xWebPSemaphore = nullptr;

WebPData webp_data = {
    .bytes = nullptr,
    .size = 0
};

WebPAnimDecoder* dec = nullptr;
WebPAnimInfo anim_info;

void decoder_task(void* pvParameter) {
    WebPTaskNotification_t notification;
    bool active = false;

    TickType_t anim_start_tick = 0;
    TickType_t next_frame_tick = 0;
    uint8_t* frame_buffer = nullptr;

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
            if (dec == nullptr) {
                active = false;
                continue;
            }

            if (!WebPAnimDecoderHasMoreFrames(dec)) {
                WebPAnimDecoderReset(dec);
                anim_start_tick = 0;
            }

            int timestamp;

            if (!WebPAnimDecoderGetNext(dec, &frame_buffer, &timestamp)) {
                ESP_LOGE(TAG, "error getting next frame");
                active = false;
                continue;
            }

            if (anim_start_tick == 0) {
                anim_start_tick = xTaskGetTickCount();
            }

            next_frame_tick = anim_start_tick + pdMS_TO_TICKS(timestamp);
            xSemaphoreGive(xWebPSemaphore);
        }

        //wait until next frame tick has elapsed
        if (xTaskGetTickCount() < next_frame_tick) {
            vTaskDelayUntil(&next_frame_tick, pdMS_TO_TICKS(1));
        }

        //display the frame
        if (frame_buffer == nullptr) {
            ESP_LOGE(TAG, "frame buffer is null");
            active = false;
            continue;
        }

        int px = 0;
        for (int y = 0; y < height; y++) {
            for (int x = 0; x < width; x++) {
                dma_display->drawPixelRGB888(x, y, frame_buffer[px * 4], frame_buffer[px * 4 + 1], frame_buffer[px * 4 + 2]);
                px++;
            }
        }

        if (current_status_bar.enabled) {
            dma_display->drawFastHLine(0, 0, width, current_status_bar.r, current_status_bar.g, current_status_bar.b);
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

    xWebPSemaphore = xSemaphoreCreateBinary();

    xTaskCreatePinnedToCore(decoder_task, "decoder", 1024, NULL, 2, &xDecoderTask, 0);
}

void start_decoder() {
    destroy_decoder();

    if (webp_data.bytes == nullptr || webp_data.size == 0) {
        ESP_LOGE(TAG, "no sprite data");
        return;
    }

    dec = WebPAnimDecoderNew(&webp_data, NULL);
    WebPAnimDecoderGetInfo(dec, &anim_info);
    xTaskNotify(xDecoderTask, WebPTaskNotification_t::WEBP_START, eSetValueWithOverwrite);
}

void destroy_decoder() {
    if (dec != nullptr) {
        if (xSemaphoreTake(xWebPSemaphore, portMAX_DELAY) == pdTRUE) {
            ESP_LOGD(TAG, "destroy decoder");
            WebPAnimDecoderDelete(dec);
            dec = nullptr;
        }
        xSemaphoreGive(xWebPSemaphore);
    }
}

esp_err_t display_sprite(uint8_t* p_sprite_buf, size_t sprite_buf_len) {
    destroy_decoder();

    webp_data.bytes = p_sprite_buf;
    webp_data.size = sprite_buf_len;

    start_decoder();

    return ESP_OK;
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
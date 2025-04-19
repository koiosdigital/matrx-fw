#ifndef DISPLAY_H
#define DISPLAY_H

#include "ESP32-HUB75-MatrixPanel-I2S-DMA.h"

typedef struct DisplayStatusBar_t {
    bool enabled = false;
    uint8_t r = 0;
    uint8_t g = 0;
    uint8_t b = 0;
} DisplayStatusBar_t;

typedef enum WebPTaskNotification_t {
    WEBP_START = 1,
} WebPTaskNotification_t;

void display_init();
esp_err_t display_sprite(uint8_t* p_sprite_buf, size_t sprite_buf_len);
void display_raw_buffer(uint8_t* p_raw_buf, size_t raw_buf_len);
size_t get_display_buffer_size();
void get_display_dimensions(int* w, int* h);
void display_clear_status_bar();
void display_set_status_bar(uint8_t r, uint8_t g, uint8_t b);

#endif
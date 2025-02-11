#ifndef DISPLAY_H
#define DISPLAY_H

#include "ESP32-HUB75-MatrixPanel-I2S-DMA.h"

#ifdef HW_TIDBYT_V1
#include "pinouts/tidbyt_v1.h"
#elif HW_TIDBYT_V2
#include "pinouts/tidbyt_v2.h"
#else
#include "pinouts/matrx_v8.h"
#endif

#define DISPLAY_CONFIG_NAMESPACE "dispconf"

void display_init();
void display_fill_rgb(uint8_t r, uint8_t g, uint8_t b);
esp_err_t display_sprite(uint8_t* p_sprite_buf, size_t sprite_buf_len);

void display_clear_status_bar();
void display_set_status_bar(uint8_t r, uint8_t g, uint8_t b);

#endif
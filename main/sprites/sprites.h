#ifndef SPRITES_H
#define SPRITES_H

#include <stdint.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

typedef struct RAMSprite_t {
    uint8_t* data;
    size_t len;
    SemaphoreHandle_t mutex;
} RAMSprite_t;

RAMSprite_t* sprite_allocate();
void sprite_update_data(RAMSprite_t* sprite, const uint8_t* data, size_t len);
void sprite_free(RAMSprite_t* sprite);
void sprites_cleanup(); // Clean up global sprite buffers

// Thread-safe accessor functions
bool sprite_get_data_copy(RAMSprite_t* sprite, uint8_t** data_copy, size_t* len);
size_t sprite_get_length(RAMSprite_t* sprite);

void show_fs_sprite(const char* filename);
void show_sprite(RAMSprite_t* sprite);
void list_static_images(); // List available static images

#endif
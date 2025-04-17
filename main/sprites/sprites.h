#ifndef SPRITES_H
#define SPRITES_H

#include <stdint.h>
#include <stdlib.h>

typedef struct RAMSprite_t {
    uint8_t* data;
    size_t len;
} RAMSprite_t;

void sprites_init();

RAMSprite_t* sprite_allocate();
void sprite_update_data(RAMSprite_t* sprite, uint8_t* data, size_t len);
void sprite_free(RAMSprite_t* sprite);

void show_fs_sprite(const char* filename);
void show_sprite(RAMSprite_t* sprite);

#endif
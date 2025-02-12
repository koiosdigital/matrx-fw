#ifndef SPRITES_H
#define SPRITES_H

#include <stdint.h>
#include <stdlib.h>

typedef struct RAMSprite_t {
    uint16_t id;
    uint8_t* data;
    size_t len;
    RAMSprite_t* next;
} RAMSprite_t;

void sprites_init();
void sprites_update(uint16_t id, uint8_t* data, size_t len);
RAMSprite_t* sprites_get(uint16_t id);

void show_fs_sprite(const char* filename);
void show_ram_sprite(uint16_t id);

#endif
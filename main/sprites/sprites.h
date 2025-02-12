#ifndef SPRITES_H
#define SPRITES_H

#include <stdint.h>
#include <stdlib.h>

typedef struct RAMSprite_t {
    char* uuid;
    uint8_t* data;
    size_t len;
    RAMSprite_t* next;
} RAMSprite_t;

void sprites_init();
void sprites_update(const char* uuid, uint8_t* data, size_t len);
void delete_sprite(const char* uuid);

RAMSprite_t* sprites_get(const char* uuid);
RAMSprite_t* sprites_get_head();

void show_fs_sprite(const char* filename);
void show_ram_sprite(const char* uuid);

#endif
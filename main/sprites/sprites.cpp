#include "sprites.h"

#include "esp_log.h"
#include <esp_littlefs.h>

#include "display.h"

RAMSprite_t* sprites_head;
uint8_t* fs_sprite_buf = NULL;

static const char* TAG = "sprites";

void sprites_init() {
    //initialize the linked list
    sprites_head = NULL;

    //register the littlefs partition
    esp_vfs_littlefs_conf_t conf = {
        .base_path = "/fs",
        .partition_label = "fs",
        .format_if_mount_failed = true,
        .dont_mount = false,
    };

    ESP_ERROR_CHECK(esp_vfs_littlefs_register(&conf));
}

void sprites_update(uint32_t id, const char* uuid, uint8_t* data, size_t len) {
    RAMSprite_t* current = sprites_head;
    RAMSprite_t* prev = NULL;

    while (current != NULL) {
        if (strcmp(current->uuid, uuid) == 0) {
            //update the sprite
            free(current->data);
            current->data = (uint8_t*)heap_caps_malloc(len, MALLOC_CAP_SPIRAM);

            current->len = len;
            memcpy(current->data, data, len);

            return;
        }

        prev = current;
        current = current->next;
    }

    //create a new sprite
    RAMSprite_t* new_sprite = (RAMSprite_t*)malloc(sizeof(RAMSprite_t));

    new_sprite->uuid = (char*)malloc(strlen(uuid) + 1);
    strcpy(new_sprite->uuid, uuid);

    new_sprite->data = (uint8_t*)heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
    new_sprite->len = len;
    memcpy(new_sprite->data, data, len);

    new_sprite->next = NULL;

    if (prev == NULL) {
        sprites_head = new_sprite;
        return;
    }

    prev->next = new_sprite;
}

RAMSprite_t* sprites_get(const char* uuid) {
    //find the sprite with the given uuid
    RAMSprite_t* current = sprites_head;
    while (current != NULL) {
        if (strcmp(current->uuid, uuid) == 0) {
            return current;
        }

        current = current->next;
    }

    return NULL;
}

RAMSprite_t* sprites_get_head() {
    return sprites_head;
}

void delete_sprite(const char* uuid) {
    RAMSprite_t* current = sprites_head;
    RAMSprite_t* prev = NULL;

    while (current != NULL) {
        if (strcmp(current->uuid, uuid) == 0) {
            if (prev == NULL) {
                sprites_head = current->next;
            }
            else {
                prev->next = current->next;
            }

            free(current->uuid);
            free(current->data);
            free(current);

            return;
        }

        prev = current;
        current = current->next;
    }
}

void show_fs_sprite(const char* filename) {
    //open the file
    FILE* f = fopen(filename, "r");
    if (f == NULL) {
        ESP_LOGE(TAG, "failed to open file");
        return;
    }

    //get the file size
    fseek(f, 0, SEEK_END);
    size_t len = ftell(f);
    fseek(f, 0, SEEK_SET);

    free(fs_sprite_buf);
    fs_sprite_buf = NULL;

    //read the file
    fs_sprite_buf = (uint8_t*)heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
    if (fs_sprite_buf == NULL) {
        ESP_LOGE(TAG, "malloc failed");
        fclose(f);
        return;
    }

    fread(fs_sprite_buf, 1, len, f);
    fclose(f);

    display_sprite(fs_sprite_buf, len);
}

void show_ram_sprite(const char* uuid) {
    RAMSprite_t* sprite = sprites_get(uuid);
    if (sprite == NULL) {
        ESP_LOGE(TAG, "sprite not found");
        return;
    }

    display_sprite(sprite->data, sprite->len);

    free(fs_sprite_buf);
    fs_sprite_buf = NULL;
}
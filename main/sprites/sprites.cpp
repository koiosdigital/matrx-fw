#include "sprites.h"

#include "esp_log.h"
#include <esp_littlefs.h>

#include "display.h"

RAMSprite_t* sprites_head;
uint8_t* fs_sprite_buf = nullptr;

static const char* TAG = "sprites";

void sprites_init() {
    //initialize the linked list
    sprites_head = nullptr;

    //register the littlefs partition
    esp_vfs_littlefs_conf_t conf = {
        .base_path = "/fs",
        .partition_label = "fs",
        .format_if_mount_failed = true,
        .dont_mount = false,
    };

    ESP_ERROR_CHECK(esp_vfs_littlefs_register(&conf));
}

void sprites_update(uint16_t id, uint8_t* data, size_t len) {
    RAMSprite_t* current = sprites_head;
    RAMSprite_t* prev = nullptr;

    while (current != nullptr) {
        if (current->id == id) {
            //update the sprite
            free(current->data);
            current->data = data;
            current->len = len;
            return;
        }

        prev = current;
        current = current->next;
    }

    //create a new sprite
    RAMSprite_t* new_sprite = (RAMSprite_t*)malloc(sizeof(RAMSprite_t));
    new_sprite->id = id;
    new_sprite->data = data;
    new_sprite->len = len;
    new_sprite->next = nullptr;

    if (prev == nullptr) {
        sprites_head = new_sprite;
    }
    else {
        prev->next = new_sprite;
    }
}

RAMSprite_t* sprites_get(uint16_t id) {
    //find the sprite with the given id
    RAMSprite_t* current = sprites_head;
    while (current != nullptr) {
        if (current->id == id) {
            return current;
        }

        current = current->next;
    }

    return nullptr;
}

void show_fs_sprite(const char* filename) {
    //open the file
    FILE* f = fopen(filename, "r");
    if (f == nullptr) {
        ESP_LOGE(TAG, "failed to open file");
        return;
    }

    //get the file size
    fseek(f, 0, SEEK_END);
    size_t len = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (fs_sprite_buf != nullptr) {
        free(fs_sprite_buf);
        fs_sprite_buf = nullptr;
    }

    //read the file
    fs_sprite_buf = (uint8_t*)heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
    if (fs_sprite_buf == nullptr) {
        ESP_LOGE(TAG, "malloc failed");
        fclose(f);
        return;
    }

    fread(fs_sprite_buf, 1, len, f);
    fclose(f);

    display_sprite(fs_sprite_buf, len);
}

void show_ram_sprite(uint16_t id) {
    RAMSprite_t* sprite = sprites_get(id);
    if (sprite == nullptr) {
        ESP_LOGE(TAG, "sprite not found");
        return;
    }

    display_sprite(sprite->data, sprite->len);

    if (fs_sprite_buf != nullptr) {
        free(fs_sprite_buf);
        fs_sprite_buf = nullptr;
    }
}
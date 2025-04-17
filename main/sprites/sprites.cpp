#include "sprites.h"

#include "esp_log.h"
#include <esp_littlefs.h>

#include "display.h"

uint8_t* fs_sprite_buf = NULL;

static const char* TAG = "sprites";

void sprites_init() {
    //register the littlefs partition
    esp_vfs_littlefs_conf_t conf = {
        .base_path = "/fs",
        .partition_label = "fs",
        .format_if_mount_failed = true,
        .dont_mount = false,
    };

    ESP_ERROR_CHECK(esp_vfs_littlefs_register(&conf));

    ESP_LOGI(TAG, "fs registered");
}

void sprite_free(RAMSprite_t* sprite) {
    free(sprite->data);
    free(sprite);
}

RAMSprite_t* sprite_allocate() {
    RAMSprite_t* sprite = (RAMSprite_t*)malloc(sizeof(RAMSprite_t));
    if (sprite == NULL) {
        ESP_LOGE(TAG, "malloc failed: new sprite");
        return NULL;
    }

    sprite->data = NULL;
    sprite->len = 0;

    return sprite;
}

void sprite_update_data(RAMSprite_t* sprite, uint8_t* data, size_t len) {
    free(sprite->data);
    sprite->data = (uint8_t*)heap_caps_calloc(len, sizeof(uint8_t), MALLOC_CAP_SPIRAM);
    if (sprite->data == NULL) {
        ESP_LOGE(TAG, "malloc failed: update sprite data");
        return;
    }

    memcpy(sprite->data, data, len);
    sprite->len = len;

    free(data);
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
    fs_sprite_buf = (uint8_t*)heap_caps_calloc(len, sizeof(uint8_t), MALLOC_CAP_SPIRAM);
    if (fs_sprite_buf == NULL) {
        ESP_LOGE(TAG, "malloc failed: fs_sprite_buf");
        fclose(f);
        return;
    }

    fread(fs_sprite_buf, 1, len, f);
    fclose(f);

    ESP_LOGI(TAG, "read %d bytes from %s", len, filename);

    display_sprite(fs_sprite_buf, len);
}

void show_sprite(RAMSprite_t* sprite) {
    display_sprite(sprite->data, sprite->len);

    free(fs_sprite_buf);
    fs_sprite_buf = NULL;
}
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
    if (sprite != NULL) {
        free(sprite->data);
        free(sprite);
    }
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
    if (sprite == NULL) {
        ESP_LOGE(TAG, "invalid sprite pointer");
        free(data); // Still free the input data
        return;
    }

    if (data == NULL || len == 0) {
        ESP_LOGE(TAG, "invalid sprite data");
        free(sprite->data);
        sprite->data = NULL;
        sprite->len = 0;
        free(data); // Still free the input data
        return;
    }

    free(sprite->data);
    sprite->data = (uint8_t*)heap_caps_calloc(len, sizeof(uint8_t), MALLOC_CAP_SPIRAM);
    if (sprite->data == NULL) {
        ESP_LOGE(TAG, "malloc failed: update sprite data");
        sprite->len = 0;
        free(data); // Still free the input data on allocation failure
        return;
    }

    memcpy(sprite->data, data, len);
    sprite->len = len;
}

void show_fs_sprite(const char* filename) {
    if (filename == NULL) {
        ESP_LOGE(TAG, "invalid filename");
        return;
    }

    //open the file
    FILE* f = fopen(filename, "r");
    if (f == NULL) {
        ESP_LOGE(TAG, "failed to open file: %s", filename);
        return;
    }

    //get the file size
    fseek(f, 0, SEEK_END);
    size_t len = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (len == 0) {
        ESP_LOGE(TAG, "file is empty: %s", filename);
        fclose(f);
        return;
    }

    free(fs_sprite_buf);
    fs_sprite_buf = NULL;

    //read the file
    fs_sprite_buf = (uint8_t*)heap_caps_calloc(len, sizeof(uint8_t), MALLOC_CAP_SPIRAM);
    if (fs_sprite_buf == NULL) {
        ESP_LOGE(TAG, "malloc failed: fs_sprite_buf");
        fclose(f);
        return;
    }

    size_t bytes_read = fread(fs_sprite_buf, 1, len, f);
    fclose(f);

    if (bytes_read != len) {
        ESP_LOGE(TAG, "failed to read complete file: %s (read %d of %d bytes)", filename, bytes_read, len);
        free(fs_sprite_buf);
        fs_sprite_buf = NULL;
        return;
    }

    ESP_LOGI(TAG, "read %d bytes from %s", len, filename);

    display_sprite(fs_sprite_buf, len);
}

void show_sprite(RAMSprite_t* sprite) {
    if (sprite == NULL || sprite->data == NULL || sprite->len == 0) {
        ESP_LOGE(TAG, "invalid sprite data");
        return;
    }

    display_sprite(sprite->data, sprite->len);

    // Only free fs_sprite_buf if we're not using it for this display
    // RAM sprites don't use fs_sprite_buf, so it's safe to free here
    free(fs_sprite_buf);
    fs_sprite_buf = NULL;
}

void sprites_cleanup() {
    free(fs_sprite_buf);
    fs_sprite_buf = NULL;
}
#include "sprites.h"

#include "esp_log.h"
#include <esp_littlefs.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

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
        // Take mutex before freeing to ensure no operations are in progress
        if (sprite->mutex != NULL) {
            xSemaphoreTake(sprite->mutex, portMAX_DELAY);
            // Delete the mutex
            vSemaphoreDelete(sprite->mutex);
            sprite->mutex = NULL;
        }

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

    // Create mutex for thread safety
    sprite->mutex = xSemaphoreCreateMutex();
    if (sprite->mutex == NULL) {
        ESP_LOGE(TAG, "failed to create sprite mutex");
        free(sprite);
        return NULL;
    }

    return sprite;
}

void sprite_update_data(RAMSprite_t* sprite, uint8_t* data, size_t len) {
    if (sprite == NULL) {
        ESP_LOGE(TAG, "invalid sprite pointer");
        free(data); // Still free the input data
        return;
    }

    if (sprite->mutex == NULL) {
        ESP_LOGE(TAG, "sprite mutex is null");
        free(data); // Still free the input data
        return;
    }

    // Take mutex to ensure exclusive access during data update
    if (xSemaphoreTake(sprite->mutex, portMAX_DELAY) != pdTRUE) {
        ESP_LOGE(TAG, "failed to take sprite mutex");
        free(data); // Still free the input data
        return;
    }

    if (data == NULL || len == 0) {
        ESP_LOGE(TAG, "invalid sprite data");
        free(sprite->data);
        sprite->data = NULL;
        sprite->len = 0;
        free(data); // Still free the input data
        xSemaphoreGive(sprite->mutex);
        return;
    }

    free(sprite->data);
    sprite->data = (uint8_t*)heap_caps_calloc(len, sizeof(uint8_t), MALLOC_CAP_SPIRAM);
    if (sprite->data == NULL) {
        ESP_LOGE(TAG, "malloc failed: update sprite data");
        sprite->len = 0;
        free(data); // Still free the input data on allocation failure
        xSemaphoreGive(sprite->mutex);
        return;
    }

    memcpy(sprite->data, data, len);
    sprite->len = len;

    // Release mutex after data update is complete
    xSemaphoreGive(sprite->mutex);
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
    if (sprite == NULL) {
        ESP_LOGE(TAG, "invalid sprite pointer");
        return;
    }

    if (sprite->mutex == NULL) {
        ESP_LOGE(TAG, "sprite mutex is null");
        return;
    }

    // Take mutex to ensure sprite data doesn't change during display
    // This will block until any update_data operation is complete
    if (xSemaphoreTake(sprite->mutex, portMAX_DELAY) != pdTRUE) {
        ESP_LOGE(TAG, "failed to take sprite mutex for display");
        return;
    }

    // Check data validity while holding the mutex
    if (sprite->data == NULL || sprite->len == 0) {
        ESP_LOGE(TAG, "invalid sprite data");
        xSemaphoreGive(sprite->mutex);
        return;
    }

    // Free any existing buffer
    free(fs_sprite_buf);
    fs_sprite_buf = NULL;

    // Create a buffer copy of the sprite data to prevent issues if the original data changes
    fs_sprite_buf = (uint8_t*)heap_caps_calloc(sprite->len, sizeof(uint8_t), MALLOC_CAP_SPIRAM);
    if (fs_sprite_buf == NULL) {
        ESP_LOGE(TAG, "malloc failed: fs_sprite_buf for RAM sprite");
        xSemaphoreGive(sprite->mutex);
        return;
    }

    memcpy(fs_sprite_buf, sprite->data, sprite->len);
    size_t sprite_len = sprite->len;

    // Release mutex after copying data
    xSemaphoreGive(sprite->mutex);

    // Display the copied data (no need to hold mutex during display)
    display_sprite(fs_sprite_buf, sprite_len);
}

void sprites_cleanup() {
    free(fs_sprite_buf);
    fs_sprite_buf = NULL;
}

// Thread-safe function to get a copy of sprite data
bool sprite_get_data_copy(RAMSprite_t* sprite, uint8_t** data_copy, size_t* len) {
    if (sprite == NULL || data_copy == NULL || len == NULL) {
        ESP_LOGE(TAG, "invalid parameters for sprite_get_data_copy");
        return false;
    }

    if (sprite->mutex == NULL) {
        ESP_LOGE(TAG, "sprite mutex is null");
        return false;
    }

    // Take mutex to ensure consistent data read
    if (xSemaphoreTake(sprite->mutex, portMAX_DELAY) != pdTRUE) {
        ESP_LOGE(TAG, "failed to take sprite mutex for data copy");
        return false;
    }

    if (sprite->data == NULL || sprite->len == 0) {
        *data_copy = NULL;
        *len = 0;
        xSemaphoreGive(sprite->mutex);
        return true; // Valid state, just empty
    }

    // Allocate copy buffer
    *data_copy = (uint8_t*)heap_caps_malloc(sprite->len, MALLOC_CAP_SPIRAM);
    if (*data_copy == NULL) {
        ESP_LOGE(TAG, "failed to allocate memory for sprite data copy");
        *len = 0;
        xSemaphoreGive(sprite->mutex);
        return false;
    }

    // Copy data
    memcpy(*data_copy, sprite->data, sprite->len);
    *len = sprite->len;

    xSemaphoreGive(sprite->mutex);
    return true;
}

// Thread-safe function to get sprite length
size_t sprite_get_length(RAMSprite_t* sprite) {
    if (sprite == NULL || sprite->mutex == NULL) {
        ESP_LOGE(TAG, "invalid sprite or mutex for sprite_get_length");
        return 0;
    }

    // Take mutex to ensure consistent read
    if (xSemaphoreTake(sprite->mutex, portMAX_DELAY) != pdTRUE) {
        ESP_LOGE(TAG, "failed to take sprite mutex for length read");
        return 0;
    }

    size_t len = sprite->len;
    xSemaphoreGive(sprite->mutex);

    return len;
}
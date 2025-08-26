#include "config.h"

#include <string.h>
#include <esp_log.h>
#include <nvs_flash.h>
#include <nvs.h>
#include <esp_event.h>

#include "display.h"
#include "daughterboard.h"

static const char* TAG = "config";

// Current system configuration (cached in memory)
static system_config_t current_config = {
    .screen_enabled = DEFAULT_SCREEN_ENABLED,
    .screen_brightness = DEFAULT_SCREEN_BRIGHTNESS,
    .auto_brightness_enabled = DEFAULT_AUTO_BRIGHTNESS
};

// Mutex for thread-safe config access
static SemaphoreHandle_t config_mutex = NULL;

// Forward declarations
static esp_err_t load_config_from_nvs(void);
static esp_err_t save_config_to_nvs(void);
static void light_sensor_event_handler(void* handler_args, esp_event_base_t base,
    int32_t event_id, void* event_data);
static void apply_display_settings(void);

esp_err_t config_init(void) {
    esp_err_t ret;

    // Create mutex for thread-safe access
    config_mutex = xSemaphoreCreateMutex();
    if (config_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create config mutex");
        return ESP_ERR_NO_MEM;
    }

    // Load configuration from NVS
    ret = load_config_from_nvs();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to load config from NVS, using defaults: %s", esp_err_to_name(ret));
        // Save default config to NVS
        save_config_to_nvs();
    }

    // Register event handler for light sensor readings
    ret = esp_event_handler_register(DAUGHTERBOARD_EVENTS, DAUGHTERBOARD_EVENT_LIGHT_READING,
        light_sensor_event_handler, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register light sensor event handler: %s", esp_err_to_name(ret));
        return ret;
    }

    // Apply initial display settings
    apply_display_settings();

    ESP_LOGI(TAG, "Config module initialized");
    ESP_LOGI(TAG, "Screen enabled: %s, Brightness: %d, Auto brightness: %s",
        current_config.screen_enabled ? "true" : "false",
        current_config.screen_brightness,
        current_config.auto_brightness_enabled ? "true" : "false");

    return ESP_OK;
}

system_config_t config_get_system_config(void) {
    system_config_t config;

    if (xSemaphoreTake(config_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        config = current_config;
        xSemaphoreGive(config_mutex);
    }
    else {
        ESP_LOGW(TAG, "Failed to take config mutex");
        // Return default config if mutex failed
        config.screen_enabled = DEFAULT_SCREEN_ENABLED;
        config.screen_brightness = DEFAULT_SCREEN_BRIGHTNESS;
        config.auto_brightness_enabled = DEFAULT_AUTO_BRIGHTNESS;
    }

    return config;
}

esp_err_t config_set_system_config(const system_config_t* config) {
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (xSemaphoreTake(config_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to take config mutex");
        return ESP_ERR_TIMEOUT;
    }

    // Update configuration
    current_config = *config;

    // Save to NVS
    esp_err_t ret = save_config_to_nvs();

    xSemaphoreGive(config_mutex);

    if (ret == ESP_OK) {
        // Apply display settings
        apply_display_settings();
        ESP_LOGI(TAG, "System config updated");
    }

    return ret;
}

esp_err_t config_update_system_config(const system_config_t* config, bool update_screen_enabled,
    bool update_brightness, bool update_auto_brightness) {
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (xSemaphoreTake(config_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to take config mutex");
        return ESP_ERR_TIMEOUT;
    }

    // Update only specified fields
    if (update_screen_enabled) {
        current_config.screen_enabled = config->screen_enabled;
    }
    if (update_brightness) {
        current_config.screen_brightness = config->screen_brightness;
    }
    if (update_auto_brightness) {
        current_config.auto_brightness_enabled = config->auto_brightness_enabled;
    }

    // Save to NVS
    esp_err_t ret = save_config_to_nvs();

    xSemaphoreGive(config_mutex);

    if (ret == ESP_OK) {
        // Apply display settings
        apply_display_settings();
        ESP_LOGI(TAG, "System config updated (partial)");
    }

    return ret;
}

bool config_get_screen_enabled(void) {
    bool enabled = DEFAULT_SCREEN_ENABLED;

    if (xSemaphoreTake(config_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        enabled = current_config.screen_enabled;
        xSemaphoreGive(config_mutex);
    }

    return enabled;
}

esp_err_t config_set_screen_enabled(bool enabled) {
    if (xSemaphoreTake(config_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    current_config.screen_enabled = enabled;
    esp_err_t ret = save_config_to_nvs();

    xSemaphoreGive(config_mutex);

    if (ret == ESP_OK) {
        apply_display_settings();
    }

    return ret;
}

uint8_t config_get_screen_brightness(void) {
    uint8_t brightness = DEFAULT_SCREEN_BRIGHTNESS;

    if (xSemaphoreTake(config_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        brightness = current_config.screen_brightness;
        xSemaphoreGive(config_mutex);
    }

    return brightness;
}

esp_err_t config_set_screen_brightness(uint8_t brightness) {
    if (xSemaphoreTake(config_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    current_config.screen_brightness = brightness;
    esp_err_t ret = save_config_to_nvs();

    xSemaphoreGive(config_mutex);

    if (ret == ESP_OK) {
        apply_display_settings();
    }

    return ret;
}

bool config_get_auto_brightness_enabled(void) {
    bool enabled = DEFAULT_AUTO_BRIGHTNESS;

    if (xSemaphoreTake(config_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        enabled = current_config.auto_brightness_enabled;
        xSemaphoreGive(config_mutex);
    }

    return enabled;
}

esp_err_t config_set_auto_brightness_enabled(bool enabled) {
    if (xSemaphoreTake(config_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    current_config.auto_brightness_enabled = enabled;
    esp_err_t ret = save_config_to_nvs();

    xSemaphoreGive(config_mutex);

    if (ret == ESP_OK) {
        apply_display_settings();
    }

    return ret;
}

// Private helper functions

static esp_err_t load_config_from_nvs(void) {
    nvs_handle_t nvs_handle;
    esp_err_t ret;

    ret = nvs_open(NVS_CONFIG_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (ret != ESP_OK) {
        return ret;
    }

    size_t required_size;
    uint8_t temp_uint8;

    // Load screen enabled state
    required_size = sizeof(uint8_t);
    ret = nvs_get_blob(nvs_handle, NVS_CONFIG_SCREEN_ENABLE, &temp_uint8, &required_size);
    if (ret == ESP_OK) {
        current_config.screen_enabled = (temp_uint8 != 0);
    }

    // Load screen brightness
    required_size = sizeof(uint8_t);
    ret = nvs_get_blob(nvs_handle, NVS_CONFIG_SCREEN_BRIGHTNESS, &current_config.screen_brightness, &required_size);
    if (ret != ESP_OK) {
        current_config.screen_brightness = DEFAULT_SCREEN_BRIGHTNESS;
    }

    // Load auto brightness enabled state
    required_size = sizeof(uint8_t);
    ret = nvs_get_blob(nvs_handle, NVS_CONFIG_AUTO_BRIGHTNESS, &temp_uint8, &required_size);
    if (ret == ESP_OK) {
        current_config.auto_brightness_enabled = (temp_uint8 != 0);
    }

    nvs_close(nvs_handle);
    return ESP_OK;
}

static esp_err_t save_config_to_nvs(void) {
    nvs_handle_t nvs_handle;
    esp_err_t ret;

    ret = nvs_open(NVS_CONFIG_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Error opening NVS handle: %s", esp_err_to_name(ret));
        return ret;
    }

    // Save screen enabled state
    uint8_t temp_uint8 = current_config.screen_enabled ? 1 : 0;
    ret = nvs_set_blob(nvs_handle, NVS_CONFIG_SCREEN_ENABLE, &temp_uint8, sizeof(uint8_t));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Error saving screen enable state: %s", esp_err_to_name(ret));
        goto cleanup;
    }

    // Save screen brightness
    ret = nvs_set_blob(nvs_handle, NVS_CONFIG_SCREEN_BRIGHTNESS, &current_config.screen_brightness, sizeof(uint8_t));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Error saving screen brightness: %s", esp_err_to_name(ret));
        goto cleanup;
    }

    // Save auto brightness enabled state
    temp_uint8 = current_config.auto_brightness_enabled ? 1 : 0;
    ret = nvs_set_blob(nvs_handle, NVS_CONFIG_AUTO_BRIGHTNESS, &temp_uint8, sizeof(uint8_t));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Error saving auto brightness enable state: %s", esp_err_to_name(ret));
        goto cleanup;
    }

    // Commit changes
    ret = nvs_commit(nvs_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Error committing to NVS: %s", esp_err_to_name(ret));
    }

cleanup:
    nvs_close(nvs_handle);
    return ret;
}

static void light_sensor_event_handler(void* handler_args, esp_event_base_t base,
    int32_t event_id, void* event_data) {
    if (event_id != DAUGHTERBOARD_EVENT_LIGHT_READING) {
        return;
    }

    light_reading_t* reading = (light_reading_t*)event_data;
    if (reading == NULL) {
        return;
    }

    bool auto_brightness_enabled = config_get_auto_brightness_enabled();
    if (!auto_brightness_enabled) {
        return;
    }

    ESP_LOGD(TAG, "Light sensor reading: %u lux", reading->lux);

    // Determine if screen should be enabled based on light level
    bool should_enable_screen = (reading->lux > AUTO_BRIGHTNESS_LUX_THRESHOLD);
    bool current_screen_enabled = config_get_screen_enabled();

    if (should_enable_screen != current_screen_enabled) {
        ESP_LOGI(TAG, "Auto brightness: %s screen due to light level %u lux",
            should_enable_screen ? "enabling" : "disabling", reading->lux);

        config_set_screen_enabled(should_enable_screen);
    }
}

static void apply_display_settings(void) {
    // Get current config values
    bool screen_enabled = config_get_screen_enabled();
    uint8_t brightness = config_get_screen_brightness();

    if (screen_enabled) {
        // Set brightness to configured value
        display_set_brightness(brightness);
        ESP_LOGD(TAG, "Applied display settings: enabled, brightness=%d", brightness);
    }
    else {
        // Set brightness to 0 to turn off screen
        display_set_brightness(0);
        ESP_LOGD(TAG, "Applied display settings: disabled");
    }
}

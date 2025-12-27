#include "config.h"

#include <cstring>

#include <esp_log.h>
#include <nvs_flash.h>
#include <nvs.h>
#include <esp_event.h>

#include "display.h"
#include "daughterboard.h"
#include "raii_utils.hpp"
#include <nvs_handle.hpp>

static const char* TAG = "config";

namespace {

// Encapsulated configuration state
struct ConfigState {
    system_config_t config = {
        .screen_enabled = DEFAULT_SCREEN_ENABLED,
        .screen_brightness = DEFAULT_SCREEN_BRIGHTNESS,
        .auto_brightness_enabled = DEFAULT_AUTO_BRIGHTNESS,
        .screen_off_lux = DEFAULT_SCREEN_OFF_LUX
    };
    SemaphoreHandle_t mutex = nullptr;

    bool init_mutex() {
        mutex = xSemaphoreCreateMutex();
        return mutex != nullptr;
    }

    // Get config with lock
    system_config_t get() {
        raii::MutexGuard lock(mutex, pdMS_TO_TICKS(100));
        if (lock) {
            return config;
        }
        ESP_LOGW(TAG, "Failed to take config mutex");
        return {DEFAULT_SCREEN_ENABLED, DEFAULT_SCREEN_BRIGHTNESS,
                DEFAULT_AUTO_BRIGHTNESS, DEFAULT_SCREEN_OFF_LUX};
    }

    // Get single field with lock
    template<typename T>
    T get_field(T system_config_t::*field, T default_val) {
        raii::MutexGuard lock(mutex, pdMS_TO_TICKS(100));
        return lock ? config.*field : default_val;
    }

    // Set single field with lock, returns true if successful
    template<typename T>
    bool set_field(T system_config_t::*field, T value) {
        raii::MutexGuard lock(mutex, pdMS_TO_TICKS(100));
        if (!lock) return false;
        config.*field = value;
        return true;
    }
};

ConfigState state;

// NVS operations
esp_err_t load_from_nvs() {
    kd::NvsHandle nvs(NVS_CONFIG_NAMESPACE, NVS_READONLY);
    if (!nvs) {
        return nvs.open_error();
    }

    size_t size;
    uint8_t temp_u8;

    // Load screen enabled
    size = sizeof(uint8_t);
    if (nvs.get_blob(NVS_CONFIG_SCREEN_ENABLE, &temp_u8, &size) == ESP_OK) {
        state.config.screen_enabled = (temp_u8 != 0);
    }

    // Load screen brightness
    size = sizeof(uint8_t);
    if (nvs.get_blob(NVS_CONFIG_SCREEN_BRIGHTNESS, &state.config.screen_brightness, &size) != ESP_OK) {
        state.config.screen_brightness = DEFAULT_SCREEN_BRIGHTNESS;
    }

    // Load auto brightness
    size = sizeof(uint8_t);
    if (nvs.get_blob(NVS_CONFIG_AUTO_BRIGHTNESS, &temp_u8, &size) == ESP_OK) {
        state.config.auto_brightness_enabled = (temp_u8 != 0);
    }

    // Load screen off lux
    size = sizeof(uint16_t);
    if (nvs.get_blob(NVS_CONFIG_SCREEN_OFF_LUX, &state.config.screen_off_lux, &size) != ESP_OK) {
        state.config.screen_off_lux = DEFAULT_SCREEN_OFF_LUX;
    }

    return ESP_OK;
}

esp_err_t save_to_nvs() {
    kd::NvsHandle nvs(NVS_CONFIG_NAMESPACE, NVS_READWRITE);
    if (!nvs) {
        ESP_LOGE(TAG, "Error opening NVS: %s", esp_err_to_name(nvs.open_error()));
        return nvs.open_error();
    }

    uint8_t temp_u8 = state.config.screen_enabled ? 1 : 0;
    esp_err_t err = nvs.set_blob(NVS_CONFIG_SCREEN_ENABLE, &temp_u8, sizeof(uint8_t));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error saving screen enable: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs.set_blob(NVS_CONFIG_SCREEN_BRIGHTNESS, &state.config.screen_brightness, sizeof(uint8_t));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error saving brightness: %s", esp_err_to_name(err));
        return err;
    }

    temp_u8 = state.config.auto_brightness_enabled ? 1 : 0;
    err = nvs.set_blob(NVS_CONFIG_AUTO_BRIGHTNESS, &temp_u8, sizeof(uint8_t));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error saving auto brightness: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs.set_blob(NVS_CONFIG_SCREEN_OFF_LUX, &state.config.screen_off_lux, sizeof(uint16_t));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error saving screen off lux: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs.commit();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error committing NVS: %s", esp_err_to_name(err));
    }
    return err;
}

// Apply display settings - caller must NOT hold the config mutex
void apply_display_settings() {
    bool screen_enabled = config_get_screen_enabled();
    uint8_t brightness = config_get_screen_brightness();

    if (screen_enabled) {
        display_set_brightness(brightness);
        ESP_LOGV(TAG, "Applied display settings: enabled, brightness=%d", brightness);
    } else {
        display_set_brightness(0);
        ESP_LOGV(TAG, "Applied display settings: disabled");
    }
}

// Apply display settings using already-fetched values (safe to call while holding mutex)
void apply_display_settings_unlocked(bool screen_enabled, uint8_t brightness) {
    if (screen_enabled) {
        display_set_brightness(brightness);
        ESP_LOGV(TAG, "Applied display settings: enabled, brightness=%d", brightness);
    } else {
        display_set_brightness(0);
        ESP_LOGV(TAG, "Applied display settings: disabled");
    }
}

void light_sensor_event_handler(void*, esp_event_base_t, int32_t event_id, void* event_data) {
    if (event_id != DAUGHTERBOARD_EVENT_LIGHT_READING) {
        return;
    }

    auto* reading = static_cast<light_reading_t*>(event_data);
    if (reading == nullptr) {
        return;
    }

    if (!config_get_auto_brightness_enabled()) {
        return;
    }

    ESP_LOGV(TAG, "Light sensor reading: %u lux", reading->lux);

    uint16_t screen_off_lux = config_get_screen_off_lux();
    bool should_enable = (reading->lux > screen_off_lux);
    bool currently_enabled = config_get_screen_enabled();

    if (should_enable != currently_enabled) {
        ESP_LOGI(TAG, "Auto brightness: %s screen (lux=%u)",
                 should_enable ? "enabling" : "disabling", reading->lux);
        config_set_screen_enabled(should_enable);
    }
}

}  // namespace

//MARK: Public API

esp_err_t config_init() {
    if (!state.init_mutex()) {
        ESP_LOGE(TAG, "Failed to create config mutex");
        return ESP_ERR_NO_MEM;
    }

    esp_err_t ret = load_from_nvs();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to load config, using defaults: %s", esp_err_to_name(ret));
        save_to_nvs();
    }

    ret = esp_event_handler_register(DAUGHTERBOARD_EVENTS, DAUGHTERBOARD_EVENT_LIGHT_READING,
                                     light_sensor_event_handler, nullptr);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register light sensor handler: %s", esp_err_to_name(ret));
        return ret;
    }

    apply_display_settings();

    ESP_LOGD(TAG, "Config initialized: enabled=%s, brightness=%d, auto=%s, off_lux=%u",
             state.config.screen_enabled ? "true" : "false",
             state.config.screen_brightness,
             state.config.auto_brightness_enabled ? "true" : "false",
             state.config.screen_off_lux);

    return ESP_OK;
}

system_config_t config_get_system_config() {
    return state.get();
}

esp_err_t config_set_system_config(const system_config_t* config) {
    if (config == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    bool screen_enabled;
    uint8_t brightness;
    esp_err_t ret;

    {
        raii::MutexGuard lock(state.mutex, pdMS_TO_TICKS(100));
        if (!lock) {
            ESP_LOGE(TAG, "Failed to take config mutex");
            return ESP_ERR_TIMEOUT;
        }

        state.config = *config;
        screen_enabled = state.config.screen_enabled;
        brightness = state.config.screen_brightness;
        ret = save_to_nvs();
    }  // mutex released here

    if (ret == ESP_OK) {
        apply_display_settings_unlocked(screen_enabled, brightness);
        ESP_LOGD(TAG, "System config updated");
    }

    return ret;
}

esp_err_t config_update_system_config(const system_config_t* config, bool update_screen_enabled,
                                       bool update_brightness, bool update_auto_brightness,
                                       bool update_screen_off_lux) {
    if (config == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    bool screen_enabled;
    uint8_t brightness;
    esp_err_t ret;

    {
        raii::MutexGuard lock(state.mutex, pdMS_TO_TICKS(100));
        if (!lock) {
            ESP_LOGE(TAG, "Failed to take config mutex");
            return ESP_ERR_TIMEOUT;
        }

        if (update_screen_enabled) {
            state.config.screen_enabled = config->screen_enabled;
        }
        if (update_brightness) {
            state.config.screen_brightness = config->screen_brightness;
        }
        if (update_auto_brightness) {
            state.config.auto_brightness_enabled = config->auto_brightness_enabled;
        }
        if (update_screen_off_lux) {
            state.config.screen_off_lux = config->screen_off_lux;
        }

        screen_enabled = state.config.screen_enabled;
        brightness = state.config.screen_brightness;
        ret = save_to_nvs();
    }  // mutex released here

    if (ret == ESP_OK) {
        apply_display_settings_unlocked(screen_enabled, brightness);
        ESP_LOGD(TAG, "System config updated (partial)");
    }

    return ret;
}

//MARK: Individual Getters

uint16_t config_get_screen_off_lux() {
    return state.get_field(&system_config_t::screen_off_lux, DEFAULT_SCREEN_OFF_LUX);
}

bool config_get_screen_enabled() {
    return state.get_field(&system_config_t::screen_enabled, DEFAULT_SCREEN_ENABLED);
}

uint8_t config_get_screen_brightness() {
    return state.get_field(&system_config_t::screen_brightness, DEFAULT_SCREEN_BRIGHTNESS);
}

bool config_get_auto_brightness_enabled() {
    return state.get_field(&system_config_t::auto_brightness_enabled, DEFAULT_AUTO_BRIGHTNESS);
}

//MARK: Individual Setters

esp_err_t config_set_screen_off_lux(uint16_t value) {
    if (!state.set_field(&system_config_t::screen_off_lux, value)) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t ret = save_to_nvs();
    if (ret == ESP_OK) {
        apply_display_settings();
    }
    return ret;
}

esp_err_t config_set_screen_enabled(bool value) {
    if (!state.set_field(&system_config_t::screen_enabled, value)) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t ret = save_to_nvs();
    if (ret == ESP_OK) {
        apply_display_settings();
    }
    return ret;
}

esp_err_t config_set_screen_brightness(uint8_t value) {
    if (!state.set_field(&system_config_t::screen_brightness, value)) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t ret = save_to_nvs();
    if (ret == ESP_OK) {
        apply_display_settings();
    }
    return ret;
}

esp_err_t config_set_auto_brightness_enabled(bool value) {
    if (!state.set_field(&system_config_t::auto_brightness_enabled, value)) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t ret = save_to_nvs();
    if (ret == ESP_OK) {
        apply_display_settings();
    }
    return ret;
}

//MARK: Reset

esp_err_t config_reset_to_defaults() {
    esp_err_t ret;

    {
        raii::MutexGuard lock(state.mutex, pdMS_TO_TICKS(100));
        if (!lock) {
            return ESP_ERR_TIMEOUT;
        }

        state.config = {
            .screen_enabled = DEFAULT_SCREEN_ENABLED,
            .screen_brightness = DEFAULT_SCREEN_BRIGHTNESS,
            .auto_brightness_enabled = DEFAULT_AUTO_BRIGHTNESS,
            .screen_off_lux = DEFAULT_SCREEN_OFF_LUX
        };

        // Erase all config from NVS
        kd::NvsHandle nvs(NVS_CONFIG_NAMESPACE, NVS_READWRITE);
        if (nvs) {
            nvs_erase_all(nvs.get());
            nvs.commit();
        }

        ret = save_to_nvs();
    }  // mutex released here

    if (ret == ESP_OK) {
        apply_display_settings_unlocked(DEFAULT_SCREEN_ENABLED, DEFAULT_SCREEN_BRIGHTNESS);
        ESP_LOGD(TAG, "Config reset to defaults");
    }

    return ret;
}

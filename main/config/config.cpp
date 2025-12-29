// System configuration - display settings with NVS persistence
#include "config.h"

#include <cstring>
#include <esp_log.h>
#include <nvs_flash.h>
#include <nvs.h>
#include <esp_system.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "display.h"
#include "apps.h"

static const char* TAG = "config";

namespace {

constexpr const char* NVS_NAMESPACE = "sys_cfg";
constexpr const char* NVS_KEY = "cfg";

constexpr system_config_t DEFAULT_CONFIG = {
    .screen_enabled = true,
    .screen_brightness = 255,
    .auto_brightness_enabled = false,
    .screen_off_lux = 1
};

system_config_t g_config = DEFAULT_CONFIG;
SemaphoreHandle_t g_mutex = nullptr;

// RAII mutex guard
class MutexGuard {
public:
    explicit MutexGuard(SemaphoreHandle_t m) : mutex_(m), locked_(false) {
        if (mutex_) {
            locked_ = (xSemaphoreTake(mutex_, pdMS_TO_TICKS(100)) == pdTRUE);
        }
    }
    ~MutexGuard() {
        if (locked_ && mutex_) {
            xSemaphoreGive(mutex_);
        }
    }
    explicit operator bool() const { return locked_; }
    MutexGuard(const MutexGuard&) = delete;
    MutexGuard& operator=(const MutexGuard&) = delete;
private:
    SemaphoreHandle_t mutex_;
    bool locked_;
};

esp_err_t load_from_nvs() {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) return err;

    size_t size = sizeof(system_config_t);
    err = nvs_get_blob(handle, NVS_KEY, &g_config, &size);
    nvs_close(handle);

    if (err != ESP_OK || size != sizeof(system_config_t)) {
        g_config = DEFAULT_CONFIG;
        return ESP_ERR_NOT_FOUND;
    }
    return ESP_OK;
}

esp_err_t save_to_nvs() {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS open failed: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_set_blob(handle, NVS_KEY, &g_config, sizeof(system_config_t));
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS save failed: %s", esp_err_to_name(err));
    }
    return err;
}

void apply_to_display() {
    if (g_config.screen_enabled) {
        display_set_brightness(g_config.screen_brightness);
    } else {
        display_set_brightness(0);
    }
}

}  // namespace

esp_err_t config_init() {
    g_mutex = xSemaphoreCreateMutex();
    if (!g_mutex) {
        ESP_LOGE(TAG, "Failed to create mutex");
        return ESP_ERR_NO_MEM;
    }

    esp_err_t ret = load_from_nvs();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Using defaults (NVS load: %s)", esp_err_to_name(ret));
        save_to_nvs();
    }

    apply_to_display();

    ESP_LOGI(TAG, "Config: enabled=%d brightness=%d auto=%d lux=%u",
        g_config.screen_enabled, g_config.screen_brightness,
        g_config.auto_brightness_enabled, g_config.screen_off_lux);

    return ESP_OK;
}

system_config_t config_get() {
    MutexGuard lock(g_mutex);
    return lock ? g_config : DEFAULT_CONFIG;
}

esp_err_t config_set(const system_config_t* config) {
    if (!config) return ESP_ERR_INVALID_ARG;

    MutexGuard lock(g_mutex);
    if (!lock) return ESP_ERR_TIMEOUT;

    g_config = *config;
    esp_err_t ret = save_to_nvs();
    if (ret == ESP_OK) {
        apply_to_display();
    }
    return ret;
}

void perform_factory_reset(const char* reason) {
    ESP_LOGI(TAG, "Factory reset%s%s", reason ? ": " : "", reason ? reason : "");

    show_fs_sprite("factory_reset_hold");

    // Erase WiFi
    if (esp_wifi_restore() != ESP_OK) {
        nvs_handle_t h;
        if (nvs_open("nvs.net80211", NVS_READWRITE, &h) == ESP_OK) {
            nvs_erase_all(h);
            nvs_commit(h);
            nvs_close(h);
        }
    }

    // Erase config
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    }

    show_fs_sprite("factory_reset_success");
    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_restart();
}

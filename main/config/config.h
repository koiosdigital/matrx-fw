// System configuration - display settings with NVS persistence
#pragma once

#include <esp_err.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool screen_enabled;
    uint8_t screen_brightness;
    bool auto_brightness_enabled;
    uint16_t screen_off_lux;
} system_config_t;

// Initialize config module (loads from NVS)
esp_err_t config_init(void);

// Get current config (thread-safe copy)
system_config_t config_get(void);

// Update config (saves to NVS, applies to display)
esp_err_t config_set(const system_config_t* config);

// Factory reset - erases WiFi and config, restarts device
void perform_factory_reset(const char* reason);

#ifdef __cplusplus
}
#endif

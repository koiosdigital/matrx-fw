#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NVS_CONFIG_NAMESPACE "system_config"

#define NVS_CONFIG_SCREEN_ENABLE "screen_enable"
#define NVS_CONFIG_SCREEN_BRIGHTNESS "screen_bright"
#define NVS_CONFIG_AUTO_BRIGHTNESS "auto_bright"

    // System configuration structure
    typedef struct {
        bool screen_enabled;           // Screen enable state
        uint8_t screen_brightness;     // Manual screen brightness (0-255)
        bool auto_brightness_enabled;  // Auto brightness enable state
    } system_config_t;

    // Default configuration values
#define DEFAULT_SCREEN_ENABLED      true
#define DEFAULT_SCREEN_BRIGHTNESS   128
#define DEFAULT_AUTO_BRIGHTNESS     false

// Auto brightness thresholds
#define AUTO_BRIGHTNESS_LUX_THRESHOLD   1  // lux threshold for screen on/off

/**
 * Initialize the config module
 */
    esp_err_t config_init(void);

    /**
     * Get current system configuration
     */
    system_config_t config_get_system_config(void);

    /**
     * Set system configuration (all fields updated atomically)
     */
    esp_err_t config_set_system_config(const system_config_t* config);

    /**
     * Update specific system configuration fields (only provided fields are updated)
     */
    esp_err_t config_update_system_config(const system_config_t* config, bool update_screen_enabled,
        bool update_brightness, bool update_auto_brightness);

    /**
     * Get screen enabled state
     */
    bool config_get_screen_enabled(void);

    /**
     * Set screen enabled state
     */
    esp_err_t config_set_screen_enabled(bool enabled);

    /**
     * Get screen brightness
     */
    uint8_t config_get_screen_brightness(void);

    /**
     * Set screen brightness
     */
    esp_err_t config_set_screen_brightness(uint8_t brightness);

    /**
     * Get auto brightness enabled state
     */
    bool config_get_auto_brightness_enabled(void);

    /**
     * Set auto brightness enabled state
     */
    esp_err_t config_set_auto_brightness_enabled(bool enabled);

#ifdef __cplusplus
}
#endif

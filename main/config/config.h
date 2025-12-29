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
#define NVS_CONFIG_SCREEN_OFF_LUX "screen_off_lux"

    // System configuration structure
    typedef struct {
        bool screen_enabled;           // Screen enable state
        uint8_t screen_brightness;     // Manual screen brightness (0-255)
        bool auto_brightness_enabled;  // Auto brightness enable state
        uint16_t screen_off_lux;       // Auto brightness: screen off when ambient light <= this lux value
    } system_config_t;

    // Default configuration values
#define DEFAULT_SCREEN_ENABLED      true
#define DEFAULT_SCREEN_BRIGHTNESS   static_cast<uint8_t>(255)
#define DEFAULT_AUTO_BRIGHTNESS     false
#define DEFAULT_SCREEN_OFF_LUX      static_cast<uint16_t>(1)

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
        bool update_brightness, bool update_auto_brightness, bool update_screen_off_lux);

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

    /**
     * Get auto brightness screen-off lux threshold
     */
    uint16_t config_get_screen_off_lux(void);

    /**
     * Set auto brightness screen-off lux threshold
     */
    esp_err_t config_set_screen_off_lux(uint16_t lux);

    /**
     * Reset configuration to default values
     */
    esp_err_t config_reset_to_defaults(void);

    /**
     * Perform a full factory reset.
     * Erases WiFi credentials, config NVS, and restarts the device.
     * This function does not return.
     *
     * @param reason Optional reason string for logging (can be NULL)
     */
    void perform_factory_reset(const char* reason);

#ifdef __cplusplus
}
#endif

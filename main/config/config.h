#pragma once

#include <esp_err.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MATRX_MAX_QUIET_WINDOWS 7

    typedef struct {
        uint8_t day_mask;    // bit0 = Sunday .. bit6 = Saturday
        uint8_t start_hour;  // 0-23
        uint8_t start_min;   // 0-59
        uint8_t end_hour;    // 0-23
        uint8_t end_min;     // 0-59
        bool    enabled;
    } quiet_window_t;

    typedef struct {
        bool screen_enabled;
        uint8_t screen_brightness;
        bool auto_brightness_enabled;
        uint16_t screen_off_lux;
        quiet_window_t quiet_windows[MATRX_MAX_QUIET_WINDOWS];
        uint8_t quiet_window_count;
    } system_config_t;

    esp_err_t config_init(void);
    system_config_t config_get(void);
    esp_err_t config_set(const system_config_t* config);
    void perform_factory_reset(const char* reason);
    bool config_is_quiet_now(void);
    void config_set_ambient_screen_off(bool off);

#ifdef __cplusplus
}
#endif

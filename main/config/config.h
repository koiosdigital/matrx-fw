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

esp_err_t config_init(void);
system_config_t config_get(void);
esp_err_t config_set(const system_config_t* config);
void perform_factory_reset(const char* reason);

#ifdef __cplusplus
}
#endif

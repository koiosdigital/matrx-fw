// Auto brightness - adjusts display brightness based on ambient light
#pragma once

#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

// Initialize auto brightness module
// Registers event handler for light readings from daughterboard
void auto_brightness_init(void);

#ifdef __cplusplus
}
#endif

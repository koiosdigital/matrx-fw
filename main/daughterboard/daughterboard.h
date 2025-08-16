#pragma once

#include <stdint.h>
#include "esp_event.h"

#ifdef __cplusplus
extern "C" {
#endif

    // GPIO pin definitions
#define DAUGHTERBOARD_BUTTON_A_GPIO     GPIO_NUM_5
#define DAUGHTERBOARD_BUTTON_B_GPIO     GPIO_NUM_6
#define DAUGHTERBOARD_BUTTON_C_GPIO     GPIO_NUM_7

// I2C pins for VEML6030 (assuming shared I2C bus)
#define DAUGHTERBOARD_I2C_SDA_GPIO      GPIO_NUM_2
#define DAUGHTERBOARD_I2C_SCL_GPIO      GPIO_NUM_1
#define DAUGHTERBOARD_I2C_PORT          I2C_NUM_1
#define DAUGHTERBOARD_I2C_FREQ_HZ       10000

// VEML6030 I2C address
#define VEML6030_I2C_ADDR               0x48

// Event declarations
    ESP_EVENT_DECLARE_BASE(DAUGHTERBOARD_EVENTS);

    typedef enum {
        DAUGHTERBOARD_EVENT_BUTTON_A_PRESSED,
        DAUGHTERBOARD_EVENT_BUTTON_B_PRESSED,
        DAUGHTERBOARD_EVENT_BUTTON_C_PRESSED,
        DAUGHTERBOARD_EVENT_LIGHT_READING,
    } daughterboard_event_t;

    // Light sensor reading data structure
    typedef struct {
        uint16_t lux;           // Light level in lux
        uint32_t timestamp;     // Timestamp when reading was taken
    } light_reading_t;

    // Button event data structure
    typedef struct {
        uint8_t button_id;      // 0=A, 1=B, 2=C
        uint32_t timestamp;     // Timestamp when button was pressed
    } button_event_t;

    // Function declarations
    esp_err_t daughterboard_init(void);
    esp_err_t daughterboard_deinit(void);
    esp_err_t daughterboard_get_light_reading(uint16_t* lux);
    bool daughterboard_is_button_pressed(uint8_t button_id);

#ifdef __cplusplus
}
#endif

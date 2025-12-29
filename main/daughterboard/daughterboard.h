#pragma once

#include <stdint.h>
#include <driver/gpio.h>
#include <driver/i2c.h>
#include <esp_event.h>

#ifdef __cplusplus
extern "C" {
#endif

// Button GPIOs
#define DAUGHTERBOARD_BUTTON_A_GPIO  GPIO_NUM_5
#define DAUGHTERBOARD_BUTTON_B_GPIO  GPIO_NUM_6
#define DAUGHTERBOARD_BUTTON_C_GPIO  GPIO_NUM_7

// I2C config
#define DAUGHTERBOARD_I2C_SDA_GPIO   GPIO_NUM_2
#define DAUGHTERBOARD_I2C_SCL_GPIO   GPIO_NUM_1
#define DAUGHTERBOARD_I2C_PORT       I2C_NUM_1
#define DAUGHTERBOARD_I2C_FREQ_HZ    100000
#define VEML6030_I2C_ADDR            0x48

// Events
ESP_EVENT_DECLARE_BASE(DAUGHTERBOARD_EVENTS);

typedef enum {
    DAUGHTERBOARD_EVENT_BUTTON_A_PRESSED,
    DAUGHTERBOARD_EVENT_BUTTON_B_PRESSED,
    DAUGHTERBOARD_EVENT_BUTTON_C_PRESSED,
    DAUGHTERBOARD_EVENT_LIGHT_READING,  // event data is uint16_t lux
} daughterboard_event_t;

// Init I2C, light sensor, buttons. Call from main task.
esp_err_t daughterboard_init(void);

// Get last lux reading (updated every second)
uint16_t daughterboard_get_lux(void);

// Check if button currently pressed (0=A, 1=B, 2=C)
bool daughterboard_is_button_pressed(uint8_t id);

#ifdef __cplusplus
}
#endif

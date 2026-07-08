#pragma once

#include <stdint.h>
#include <driver/gpio.h>
#include <driver/i2c_master.h>
#include <esp_event.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DAUGHTERBOARD_BUTTON_A_GPIO  GPIO_NUM_5
#define DAUGHTERBOARD_BUTTON_B_GPIO  GPIO_NUM_6
#define DAUGHTERBOARD_BUTTON_C_GPIO  GPIO_NUM_7

#define DAUGHTERBOARD_I2C_SDA_GPIO   GPIO_NUM_2
#define DAUGHTERBOARD_I2C_SCL_GPIO   GPIO_NUM_1
#define DAUGHTERBOARD_I2C_PORT       I2C_NUM_1
#define DAUGHTERBOARD_I2C_FREQ_HZ    100000
#define VEML6030_I2C_ADDR            0x48

    ESP_EVENT_DECLARE_BASE(DAUGHTERBOARD_EVENTS);

    typedef enum {
        DAUGHTERBOARD_EVENT_BUTTON_A_PRESSED,
        DAUGHTERBOARD_EVENT_BUTTON_B_PRESSED,
        DAUGHTERBOARD_EVENT_BUTTON_C_PRESSED,
        DAUGHTERBOARD_EVENT_LIGHT_READING,
    } daughterboard_event_t;

    typedef struct {
        uint16_t raw;
    } light_reading_t;

    esp_err_t daughterboard_init(void);
    uint16_t daughterboard_get_lux(void);
    bool daughterboard_is_button_pressed(uint8_t id);
    esp_err_t daughterboard_set_veml_config(uint16_t config);

#ifdef __cplusplus
}
#endif

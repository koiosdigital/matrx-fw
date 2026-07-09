#include "daughterboard.h"

#include <cmath>
#include <esp_log.h>
#include <esp_event.h>
#include <esp_timer.h>
#include <driver/gpio.h>
#include <driver/i2c_master.h>

#include "config.h"
#include "display.h"

static const char* TAG = "daughterboard";

ESP_EVENT_DEFINE_BASE(DAUGHTERBOARD_EVENTS);

namespace {

    constexpr uint8_t VEML6030_REG_ALS_CONF = 0x00;
    constexpr uint8_t VEML6030_REG_PSM = 0x03;
    constexpr uint8_t VEML6030_REG_ALS = 0x04;

    constexpr uint16_t GAIN_2 = 0x0800;
    constexpr uint16_t GAIN_1 = 0x0000;
    constexpr uint16_t GAIN_1_4 = 0x1800;
    constexpr uint16_t GAIN_1_8 = 0x1000;

    constexpr uint16_t IT_100MS = 0x0000;

    constexpr float RESOLUTION_GAIN_2 = 0.0288f;
    constexpr float RESOLUTION_GAIN_1 = 0.0576f;
    constexpr float RESOLUTION_GAIN_1_4 = 0.2304f;
    constexpr float RESOLUTION_GAIN_1_8 = 0.4608f;

    constexpr uint16_t HIGH_THRESHOLD = 50000;
    constexpr uint16_t LOW_THRESHOLD = 1000;

    constexpr float HYSTERESIS_LUX = 2.0f;
    constexpr float SMOOTHING_FACTOR = 0.3f;

    constexpr gpio_num_t BUTTON_GPIOS[] = {
        DAUGHTERBOARD_BUTTON_A_GPIO,
        DAUGHTERBOARD_BUTTON_B_GPIO,
        DAUGHTERBOARD_BUTTON_C_GPIO
    };
    constexpr size_t NUM_BUTTONS = 3;

    esp_timer_handle_t light_timer = nullptr;
    uint64_t button_last_isr[NUM_BUTTONS] = {};
    uint16_t last_lux = 0;
    i2c_master_dev_handle_t veml_dev = nullptr;

    struct AutoBrightnessState {
        uint16_t current_gain = GAIN_2;
        float smoothed_lux = 0.0f;
        bool screen_is_off = false;
        bool initialized = false;
    };
    AutoBrightnessState ab_state;

    esp_err_t veml_write(uint8_t reg, uint16_t val) {
        uint8_t data[3] = { reg, (uint8_t)(val & 0xFF), (uint8_t)(val >> 8) };
        return i2c_master_transmit(veml_dev, data, sizeof(data), 100);
    }

    esp_err_t veml_read(uint8_t reg, uint16_t* val) {
        uint8_t data[2] = {};
        esp_err_t ret = i2c_master_transmit_receive(veml_dev, &reg, 1, data, sizeof(data), 100);
        if (ret == ESP_OK) *val = (data[1] << 8) | data[0];
        return ret;
    }

    float get_resolution(uint16_t gain) {
        switch (gain) {
        case GAIN_2:   return RESOLUTION_GAIN_2;
        case GAIN_1:   return RESOLUTION_GAIN_1;
        case GAIN_1_4: return RESOLUTION_GAIN_1_4;
        case GAIN_1_8: return RESOLUTION_GAIN_1_8;
        default:       return RESOLUTION_GAIN_2;
        }
    }

    uint16_t decrease_gain(uint16_t current) {
        switch (current) {
        case GAIN_2:   return GAIN_1;
        case GAIN_1:   return GAIN_1_4;
        case GAIN_1_4: return GAIN_1_8;
        case GAIN_1_8: return GAIN_1_8;
        default:       return GAIN_1_8;
        }
    }

    uint16_t increase_gain(uint16_t current) {
        switch (current) {
        case GAIN_1_8: return GAIN_1_4;
        case GAIN_1_4: return GAIN_1;
        case GAIN_1:   return GAIN_2;
        case GAIN_2:   return GAIN_2;
        default:       return GAIN_2;
        }
    }

    bool adjust_gain_if_needed(uint16_t raw) {
        uint16_t new_gain = ab_state.current_gain;

        if (raw > HIGH_THRESHOLD) {
            new_gain = decrease_gain(ab_state.current_gain);
        }
        else if (raw < LOW_THRESHOLD && ab_state.smoothed_lux < 100.0f) {
            new_gain = increase_gain(ab_state.current_gain);
        }

        if (new_gain != ab_state.current_gain) {
            ab_state.current_gain = new_gain;
            veml_write(VEML6030_REG_ALS_CONF, new_gain | IT_100MS);
            return true;
        }

        return false;
    }

    void process_auto_brightness(uint16_t raw) {
        system_config_t config = config_get();

        if (!config.auto_brightness_enabled) return;

        if (adjust_gain_if_needed(raw)) return;

        float lux = raw * get_resolution(ab_state.current_gain);

        if (!ab_state.initialized) {
            ab_state.smoothed_lux = lux;
            ab_state.initialized = true;
        } else {
            ab_state.smoothed_lux = ab_state.smoothed_lux * (1.0f - SMOOTHING_FACTOR)
                + lux * SMOOTHING_FACTOR;
        }

        float screen_off_threshold = static_cast<float>(config.screen_off_lux);
        float screen_on_threshold = screen_off_threshold + HYSTERESIS_LUX;

        if (ab_state.screen_is_off) {
            if (ab_state.smoothed_lux >= screen_on_threshold) {
                ab_state.screen_is_off = false;
                config_set_ambient_screen_off(false);
            } else {
                return;
            }
        } else {
            if (ab_state.smoothed_lux < screen_off_threshold) {
                ab_state.screen_is_off = true;
                config_set_ambient_screen_off(true);
                return;
            }
        }
    }

    void light_timer_cb(void*) {
        uint16_t raw = 0;
        if (veml_read(VEML6030_REG_ALS, &raw) == ESP_OK) {
            last_lux = (uint16_t)(raw * get_resolution(ab_state.current_gain) + 0.5f);

            process_auto_brightness(raw);

            light_reading_t reading = { .raw = raw };
            esp_event_post(DAUGHTERBOARD_EVENTS, DAUGHTERBOARD_EVENT_LIGHT_READING,
                &reading, sizeof(reading), 0);
        }
    }

    void IRAM_ATTR button_isr(void* arg) {
        size_t id = (size_t)arg;
        if (id >= NUM_BUTTONS) return;

        uint64_t now = esp_timer_get_time();
        if (now - button_last_isr[id] < 50000) return;
        button_last_isr[id] = now;

        int32_t event = DAUGHTERBOARD_EVENT_BUTTON_A_PRESSED + id;
        BaseType_t woken = pdFALSE;
        esp_event_isr_post(DAUGHTERBOARD_EVENTS, event, nullptr, 0, &woken);
        if (woken) portYIELD_FROM_ISR();
    }

}  // namespace

esp_err_t daughterboard_init() {
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = DAUGHTERBOARD_I2C_PORT,
        .sda_io_num = DAUGHTERBOARD_I2C_SDA_GPIO,
        .scl_io_num = DAUGHTERBOARD_I2C_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .intr_priority = 0,
        .trans_queue_depth = 0,
        .flags = {
            .enable_internal_pullup = false,
            .allow_pd = false,
        },
    };
    i2c_master_bus_handle_t bus = nullptr;
    esp_err_t ret = i2c_new_master_bus(&bus_cfg, &bus);
    if (ret != ESP_OK) return ret;

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = VEML6030_I2C_ADDR,
        .scl_speed_hz = DAUGHTERBOARD_I2C_FREQ_HZ,
        .scl_wait_us = 0,
        .flags = {
            .disable_ack_check = false,
        },
    };
    ret = i2c_master_bus_add_device(bus, &dev_cfg, &veml_dev);
    if (ret != ESP_OK) return ret;

    veml_write(VEML6030_REG_ALS_CONF, 0x0001);
    veml_write(VEML6030_REG_PSM, 0x0000);
    veml_write(VEML6030_REG_ALS_CONF, GAIN_2 | IT_100MS);
    ab_state.current_gain = GAIN_2;

    gpio_config_t btn_cfg = {
        .pin_bit_mask = (1ULL << BUTTON_GPIOS[0]) | (1ULL << BUTTON_GPIOS[1]) | (1ULL << BUTTON_GPIOS[2]),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_NEGEDGE,
    };
    gpio_config(&btn_cfg);
    gpio_install_isr_service(0);

    for (size_t i = 0; i < NUM_BUTTONS; i++) {
        gpio_isr_handler_add(BUTTON_GPIOS[i], button_isr, (void*)i);
    }

    esp_timer_create_args_t timer_args = {
        .callback = light_timer_cb,
        .arg = nullptr,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "light",
        .skip_unhandled_events = true,
    };
    esp_timer_create(&timer_args, &light_timer);
    esp_timer_start_periodic(light_timer, 1000000);

    return ESP_OK;
}

uint16_t daughterboard_get_lux() {
    return last_lux;
}

bool daughterboard_is_button_pressed(uint8_t id) {
    if (id >= NUM_BUTTONS) return false;
    return !gpio_get_level(BUTTON_GPIOS[id]);
}

esp_err_t daughterboard_set_veml_config(uint16_t config) {
    return veml_write(VEML6030_REG_ALS_CONF, config);
}

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

    // VEML6030 registers
    constexpr uint8_t VEML6030_REG_ALS_CONF = 0x00;
    constexpr uint8_t VEML6030_REG_PSM = 0x03;
    constexpr uint8_t VEML6030_REG_ALS = 0x04;

    // VEML6030 gain constants (ALS_CONF register bits 12:11)
    constexpr uint16_t GAIN_2 = 0x0800;    // 2x gain, highest sensitivity
    constexpr uint16_t GAIN_1 = 0x0000;    // 1x gain
    constexpr uint16_t GAIN_1_4 = 0x1800;  // 1/4 gain
    constexpr uint16_t GAIN_1_8 = 0x1000;  // 1/8 gain, lowest sensitivity

    // Integration time (100ms, bits 9:6 = 0000)
    constexpr uint16_t IT_100MS = 0x0000;

    // Resolution (lux per raw count) for each gain at 100ms IT
    constexpr float RESOLUTION_GAIN_2 = 0.0288f;
    constexpr float RESOLUTION_GAIN_1 = 0.0576f;
    constexpr float RESOLUTION_GAIN_1_4 = 0.2304f;
    constexpr float RESOLUTION_GAIN_1_8 = 0.4608f;

    // Auto-range thresholds
    constexpr uint16_t HIGH_THRESHOLD = 50000;  // ~80% of 16-bit max, decrease gain
    constexpr uint16_t LOW_THRESHOLD = 1000;    // Low signal, increase gain

    // Brightness mapping constants
    constexpr uint8_t MIN_BRIGHTNESS = 8;
    constexpr uint8_t MAX_BRIGHTNESS = 255;
    constexpr float HYSTERESIS_LUX = 2.0f;
    constexpr float SMOOTHING_FACTOR = 0.3f;

    // Button GPIOs
    constexpr gpio_num_t BUTTON_GPIOS[] = {
        DAUGHTERBOARD_BUTTON_A_GPIO,
        DAUGHTERBOARD_BUTTON_B_GPIO,
        DAUGHTERBOARD_BUTTON_C_GPIO
    };
    constexpr size_t NUM_BUTTONS = 3;

    // State
    esp_timer_handle_t light_timer = nullptr;
    uint64_t button_last_isr[NUM_BUTTONS] = {};
    uint16_t last_lux = 0;
    i2c_master_dev_handle_t veml_dev = nullptr;

    // Auto brightness state
    struct AutoBrightnessState {
        uint16_t current_gain = GAIN_2;
        float smoothed_lux = 0.0f;
        bool screen_is_off = false;
        bool initialized = false;
    };
    AutoBrightnessState ab_state;

    // I2C helpers
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
            if (new_gain != ab_state.current_gain) {
                ESP_LOGI(TAG, "Decreasing gain: raw=%u > %u", raw, HIGH_THRESHOLD);
            }
        }
        else if (raw < LOW_THRESHOLD && ab_state.smoothed_lux < 100.0f) {
            new_gain = increase_gain(ab_state.current_gain);
            if (new_gain != ab_state.current_gain) {
                ESP_LOGI(TAG, "Increasing gain: raw=%u < %u, lux=%.1f",
                    raw, LOW_THRESHOLD, ab_state.smoothed_lux);
            }
        }

        if (new_gain != ab_state.current_gain) {
            ab_state.current_gain = new_gain;
            veml_write(VEML6030_REG_ALS_CONF, new_gain | IT_100MS);
            return true;
        }

        return false;
    }

    uint8_t lux_to_brightness(float lux) {
        if (lux <= 1.0f) return MIN_BRIGHTNESS;
        if (lux >= 1000.0f) return MAX_BRIGHTNESS;

        float normalized = std::log10(lux) / 3.0f;
        float brightness = MIN_BRIGHTNESS + (MAX_BRIGHTNESS - MIN_BRIGHTNESS) * normalized;

        if (brightness < MIN_BRIGHTNESS) brightness = MIN_BRIGHTNESS;
        if (brightness > MAX_BRIGHTNESS) brightness = MAX_BRIGHTNESS;

        return static_cast<uint8_t>(brightness + 0.5f);
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
                ESP_LOGI(TAG, "Screen on: lux=%.1f >= %.1f",
                    ab_state.smoothed_lux, screen_on_threshold);
            } else {
                return;
            }
        } else {
            if (ab_state.smoothed_lux < screen_off_threshold) {
                ab_state.screen_is_off = true;
                ESP_LOGI(TAG, "Screen off: lux=%.1f < %.1f",
                    ab_state.smoothed_lux, screen_off_threshold);
                return;
            }
        }

        uint8_t brightness = lux_to_brightness(ab_state.smoothed_lux);

        ESP_LOGD(TAG, "lux=%.1f (raw=%u, gain=0x%04x) -> brightness=%u",
            ab_state.smoothed_lux, raw, ab_state.current_gain, brightness);
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
        if (now - button_last_isr[id] < 50000) return;  // 50ms debounce
        button_last_isr[id] = now;

        int32_t event = DAUGHTERBOARD_EVENT_BUTTON_A_PRESSED + id;
        BaseType_t woken = pdFALSE;
        esp_event_isr_post(DAUGHTERBOARD_EVENTS, event, nullptr, 0, &woken);
        if (woken) portYIELD_FROM_ISR();
    }

}  // namespace

esp_err_t daughterboard_init() {
    // Init I2C bus
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

    // Add VEML6030 device
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

    // Init VEML6030 with gain 2x (highest sensitivity)
    veml_write(VEML6030_REG_ALS_CONF, 0x0001);  // Shutdown
    veml_write(VEML6030_REG_PSM, 0x0000);
    veml_write(VEML6030_REG_ALS_CONF, GAIN_2 | IT_100MS);  // Active
    ab_state.current_gain = GAIN_2;

    // Init buttons
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

    // Start light sensor timer (1 second interval)
    esp_timer_create_args_t timer_args = {
        .callback = light_timer_cb,
        .arg = nullptr,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "light",
        .skip_unhandled_events = true,
    };
    esp_timer_create(&timer_args, &light_timer);
    esp_timer_start_periodic(light_timer, 1000000);  // 1 second

    ESP_LOGI(TAG, "Daughterboard initialized");
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

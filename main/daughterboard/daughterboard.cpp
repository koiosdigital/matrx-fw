#include "daughterboard.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <esp_log.h>
#include <esp_event.h>
#include <esp_timer.h>
#include <driver/gpio.h>
#include <driver/i2c.h>

#include "display.h"

static const char* TAG = "daughterboard";

ESP_EVENT_DEFINE_BASE(DAUGHTERBOARD_EVENTS);

namespace {

// VEML6030 register definitions
constexpr uint8_t VEML6030_REG_ALS_CONF = 0x00;
constexpr uint8_t VEML6030_REG_PSM      = 0x03;
constexpr uint8_t VEML6030_REG_ALS      = 0x04;

// VEML6030 configuration values
constexpr uint16_t VEML6030_ALS_SD_SHUTDOWN = 0x0001;
constexpr uint16_t VEML6030_IT_100MS        = 0x0000;
constexpr uint16_t VEML6030_PERS_1          = 0x0000;
constexpr uint16_t VEML6030_GAIN_2          = 0x0800;
constexpr uint16_t VEML6030_INT_DISABLE     = 0x0000;
constexpr uint16_t VEML6030_CONFIG_ACTIVE   = VEML6030_IT_100MS | VEML6030_PERS_1 |
                                               VEML6030_INT_DISABLE | VEML6030_GAIN_2;

// Timing constants
constexpr uint64_t BUTTON_DEBOUNCE_US      = 50000;    // 50ms
constexpr uint32_t LIGHT_SENSOR_INTERVAL_MS = 5000;    // 5 seconds

// Lux conversion factor for gain 2x and 100ms integration
constexpr float LUX_RESOLUTION = 0.0288f;

// Button GPIO mapping
constexpr gpio_num_t BUTTON_GPIOS[] = {
    DAUGHTERBOARD_BUTTON_A_GPIO,
    DAUGHTERBOARD_BUTTON_B_GPIO,
    DAUGHTERBOARD_BUTTON_C_GPIO
};
constexpr size_t NUM_BUTTONS = 3;

// Encapsulated daughterboard state
struct DaughterboardState {
    TaskHandle_t light_sensor_task = nullptr;
    uint64_t button_last_isr_time[NUM_BUTTONS] = {};
};

DaughterboardState db;

// I2C helper functions
esp_err_t veml6030_write_reg(uint8_t reg, uint16_t value) {
    uint8_t data[3] = {
        reg,
        static_cast<uint8_t>(value & 0xFF),
        static_cast<uint8_t>((value >> 8) & 0xFF)
    };

    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (VEML6030_I2C_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write(cmd, data, 3, true);
    i2c_master_stop(cmd);

    esp_err_t ret = i2c_master_cmd_begin(DAUGHTERBOARD_I2C_PORT, cmd, pdMS_TO_TICKS(1000));
    i2c_cmd_link_delete(cmd);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C write failed for reg 0x%02X: %s", reg, esp_err_to_name(ret));
    }
    return ret;
}

esp_err_t veml6030_read_reg(uint8_t reg, uint16_t* value) {
    if (value == nullptr) return ESP_ERR_INVALID_ARG;

    uint8_t data[2] = {};

    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (VEML6030_I2C_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg, true);
    i2c_master_start(cmd);  // Repeated start
    i2c_master_write_byte(cmd, (VEML6030_I2C_ADDR << 1) | I2C_MASTER_READ, true);
    i2c_master_read_byte(cmd, &data[0], I2C_MASTER_ACK);
    i2c_master_read_byte(cmd, &data[1], I2C_MASTER_NACK);
    i2c_master_stop(cmd);

    esp_err_t ret = i2c_master_cmd_begin(DAUGHTERBOARD_I2C_PORT, cmd, pdMS_TO_TICKS(1000));
    i2c_cmd_link_delete(cmd);

    if (ret == ESP_OK) {
        *value = (data[1] << 8) | data[0];
    } else {
        ESP_LOGE(TAG, "I2C read failed for reg 0x%02X: %s", reg, esp_err_to_name(ret));
        *value = 0;
    }
    return ret;
}

void light_sensor_task_func(void*) {
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(LIGHT_SENSOR_INTERVAL_MS));

        uint16_t lux = 0;
        esp_err_t ret = daughterboard_get_light_reading(&lux);

        if (ret == ESP_OK) {
            light_reading_t reading = {
                .lux = lux,
                .timestamp = static_cast<uint32_t>(esp_timer_get_time() / 1000)
            };

            esp_event_post(DAUGHTERBOARD_EVENTS, DAUGHTERBOARD_EVENT_LIGHT_READING,
                           &reading, sizeof(reading), 0);

            ESP_LOGD(TAG, "Light reading: %u lux", lux);
        } else {
            ESP_LOGW(TAG, "Failed to read light sensor: %s", esp_err_to_name(ret));

            uint16_t raw_debug = 0;
            if (veml6030_read_reg(VEML6030_REG_ALS, &raw_debug) == ESP_OK) {
                ESP_LOGW(TAG, "Raw ALS register value: %u", raw_debug);
            }
        }
    }
}

void IRAM_ATTR button_isr_handler(void* arg) {
    auto button_id = reinterpret_cast<uintptr_t>(arg);
    if (button_id >= NUM_BUTTONS) return;

    uint64_t current_time = esp_timer_get_time();

    if (current_time - db.button_last_isr_time[button_id] < BUTTON_DEBOUNCE_US) {
        return;
    }
    db.button_last_isr_time[button_id] = current_time;

    constexpr daughterboard_event_t events[] = {
        DAUGHTERBOARD_EVENT_BUTTON_A_PRESSED,
        DAUGHTERBOARD_EVENT_BUTTON_B_PRESSED,
        DAUGHTERBOARD_EVENT_BUTTON_C_PRESSED
    };

    BaseType_t high_task_awoken = pdFALSE;
    esp_event_isr_post(DAUGHTERBOARD_EVENTS, events[button_id],
                       nullptr, 0, &high_task_awoken);

    if (high_task_awoken == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

esp_err_t init_i2c() {
    i2c_config_t conf = {};
    conf.mode = I2C_MODE_MASTER;
    conf.sda_io_num = DAUGHTERBOARD_I2C_SDA_GPIO;
    conf.scl_io_num = DAUGHTERBOARD_I2C_SCL_GPIO;
    conf.sda_pullup_en = GPIO_PULLUP_DISABLE;
    conf.scl_pullup_en = GPIO_PULLUP_DISABLE;
    conf.master.clk_speed = DAUGHTERBOARD_I2C_FREQ_HZ;
    conf.clk_flags = 0;

    esp_err_t ret = i2c_param_config(DAUGHTERBOARD_I2C_PORT, &conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C param config failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = i2c_driver_install(DAUGHTERBOARD_I2C_PORT, conf.mode, 0, 0, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C driver install failed: %s", esp_err_to_name(ret));
    }
    return ret;
}

esp_err_t init_veml6030() {
    esp_err_t ret = veml6030_write_reg(VEML6030_REG_ALS_CONF, VEML6030_ALS_SD_SHUTDOWN);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to shutdown VEML6030: %s", esp_err_to_name(ret));
        return ret;
    }

    vTaskDelay(pdMS_TO_TICKS(10));

    ret = veml6030_write_reg(VEML6030_REG_PSM, 0x0000);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure VEML6030 PSM: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = veml6030_write_reg(VEML6030_REG_ALS_CONF, VEML6030_CONFIG_ACTIVE);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure VEML6030 ALS: %s", esp_err_to_name(ret));
        return ret;
    }

    vTaskDelay(pdMS_TO_TICKS(120));  // Wait for first measurement
    return ESP_OK;
}

esp_err_t init_buttons() {
    gpio_config_t io_conf = {
        .pin_bit_mask = ((1ULL << DAUGHTERBOARD_BUTTON_A_GPIO) |
                        (1ULL << DAUGHTERBOARD_BUTTON_B_GPIO) |
                        (1ULL << DAUGHTERBOARD_BUTTON_C_GPIO)),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_NEGEDGE,
    };

    esp_err_t ret = gpio_config(&io_conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure button GPIOs: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = gpio_install_isr_service(0);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Failed to install GPIO ISR service: %s", esp_err_to_name(ret));
        return ret;
    }

    for (size_t i = 0; i < NUM_BUTTONS; i++) {
        ret = gpio_isr_handler_add(BUTTON_GPIOS[i], button_isr_handler,
                                    reinterpret_cast<void*>(i));
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to add ISR handler for button %zu: %s", i, esp_err_to_name(ret));
            return ret;
        }
        db.button_last_isr_time[i] = 0;
    }

    return ESP_OK;
}

}  // namespace

//MARK: Public API

esp_err_t daughterboard_init() {
    esp_err_t ret = init_i2c();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize I2C: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = init_veml6030();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize VEML6030: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = init_buttons();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize buttons: %s", esp_err_to_name(ret));
        return ret;
    }

    BaseType_t task_ret = xTaskCreate(light_sensor_task_func, "light_sensor",
                                       2048, nullptr, 5, &db.light_sensor_task);
    if (task_ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create light sensor task");
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t daughterboard_deinit() {
    if (db.light_sensor_task != nullptr) {
        vTaskDelete(db.light_sensor_task);
        db.light_sensor_task = nullptr;
    }

    for (size_t i = 0; i < NUM_BUTTONS; i++) {
        gpio_isr_handler_remove(BUTTON_GPIOS[i]);
    }

    i2c_driver_delete(DAUGHTERBOARD_I2C_PORT);
    return ESP_OK;
}

esp_err_t daughterboard_get_light_reading(uint16_t* lux) {
    if (lux == nullptr) return ESP_ERR_INVALID_ARG;

    uint16_t raw_value = 0;
    esp_err_t ret = veml6030_read_reg(VEML6030_REG_ALS, &raw_value);

    if (ret == ESP_OK) {
        float lux_float = static_cast<float>(raw_value) * LUX_RESOLUTION;
        *lux = static_cast<uint16_t>(lux_float + 0.5f);
    } else {
        ESP_LOGW(TAG, "Failed to read VEML6030 ALS register: %s", esp_err_to_name(ret));
        *lux = 0;
    }

    return ret;
}

bool daughterboard_is_button_pressed(uint8_t button_id) {
    if (button_id >= NUM_BUTTONS) return false;
    return !gpio_get_level(BUTTON_GPIOS[button_id]);
}

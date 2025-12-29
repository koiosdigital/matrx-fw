#include "daughterboard.h"

#include <esp_log.h>
#include <esp_event.h>
#include <esp_timer.h>
#include <driver/gpio.h>
#include <driver/i2c.h>

static const char* TAG = "daughterboard";

ESP_EVENT_DEFINE_BASE(DAUGHTERBOARD_EVENTS);

namespace {

// VEML6030 registers
constexpr uint8_t VEML6030_REG_ALS_CONF = 0x00;
constexpr uint8_t VEML6030_REG_PSM      = 0x03;
constexpr uint8_t VEML6030_REG_ALS      = 0x04;

// VEML6030 config: gain 2x, 100ms integration, no interrupt
constexpr uint16_t VEML6030_CONFIG = 0x0800;  // GAIN_2 | IT_100MS

// Lux conversion factor for gain 2x and 100ms integration
constexpr float LUX_RESOLUTION = 0.0288f;

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

// I2C helpers
esp_err_t veml_write(uint8_t reg, uint16_t val) {
    uint8_t data[3] = { reg, (uint8_t)(val & 0xFF), (uint8_t)(val >> 8) };
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (VEML6030_I2C_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write(cmd, data, 3, true);
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(DAUGHTERBOARD_I2C_PORT, cmd, pdMS_TO_TICKS(100));
    i2c_cmd_link_delete(cmd);
    return ret;
}

esp_err_t veml_read(uint8_t reg, uint16_t* val) {
    uint8_t data[2] = {};
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (VEML6030_I2C_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg, true);
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (VEML6030_I2C_ADDR << 1) | I2C_MASTER_READ, true);
    i2c_master_read(cmd, data, 2, I2C_MASTER_LAST_NACK);
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(DAUGHTERBOARD_I2C_PORT, cmd, pdMS_TO_TICKS(100));
    i2c_cmd_link_delete(cmd);
    if (ret == ESP_OK) *val = (data[1] << 8) | data[0];
    return ret;
}

void light_timer_cb(void*) {
    uint16_t raw = 0;
    if (veml_read(VEML6030_REG_ALS, &raw) == ESP_OK) {
        last_lux = (uint16_t)(raw * LUX_RESOLUTION + 0.5f);
        esp_event_post(DAUGHTERBOARD_EVENTS, DAUGHTERBOARD_EVENT_LIGHT_READING,
                       &last_lux, sizeof(last_lux), 0);
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
    // Init I2C
    i2c_config_t i2c_cfg = {};
    i2c_cfg.mode = I2C_MODE_MASTER;
    i2c_cfg.sda_io_num = DAUGHTERBOARD_I2C_SDA_GPIO;
    i2c_cfg.scl_io_num = DAUGHTERBOARD_I2C_SCL_GPIO;
    i2c_cfg.sda_pullup_en = GPIO_PULLUP_DISABLE;
    i2c_cfg.scl_pullup_en = GPIO_PULLUP_DISABLE;
    i2c_cfg.master.clk_speed = DAUGHTERBOARD_I2C_FREQ_HZ;

    esp_err_t ret = i2c_param_config(DAUGHTERBOARD_I2C_PORT, &i2c_cfg);
    if (ret != ESP_OK) return ret;

    ret = i2c_driver_install(DAUGHTERBOARD_I2C_PORT, I2C_MODE_MASTER, 0, 0, 0);
    if (ret != ESP_OK) return ret;

    // Init VEML6030
    veml_write(VEML6030_REG_ALS_CONF, 0x0001);  // Shutdown
    veml_write(VEML6030_REG_PSM, 0x0000);
    veml_write(VEML6030_REG_ALS_CONF, VEML6030_CONFIG);  // Active

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

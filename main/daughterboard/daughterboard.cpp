#include "daughterboard.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "esp_log.h"
#include "esp_event.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "driver/i2c.h"
#include "display.h"

static const char* TAG = "daughterboard";

// Event base definition
ESP_EVENT_DEFINE_BASE(DAUGHTERBOARD_EVENTS);

// ESP timer handle for light sensor
static esp_timer_handle_t light_sensor_timer_handle = NULL;

// VEML6030 register definitions
#define VEML6030_REG_ALS_CONF   0x00
#define VEML6030_REG_ALS_WH     0x01
#define VEML6030_REG_ALS_WL     0x02,
#define VEML6030_REG_PSM        0x03
#define VEML6030_REG_ALS        0x04
#define VEML6030_REG_WHITE      0x05
#define VEML6030_REG_ALS_INT    0x06
#define VEML6030_REG_ID         0x07

// Expected device ID value
#define VEML6030_DEVICE_ID      0x0081

// VEML6030 configuration values
// ALS_CONF register (0x00):
// Bit 15-14: Reserved, must be 0
// Bit 13: ALS_INT_EN (0 = disable interrupt)
// Bit 12-11: PSM (00 = PSM mode 1, 01 = PSM mode 2, 10 = PSM mode 3, 11 = PSM mode 4)
// Bit 10-9: Reserved, must be 0  
// Bit 8-6: ALS_PERS (000 = 1, 001 = 2, 010 = 4, 011 = 8)
// Bit 5-4: Reserved, must be 0
// Bit 3-2: ALS_IT (00 = 100ms, 01 = 200ms, 10 = 800ms, 11 = 50ms)
// Bit 1: Reserved, must be 0
// Bit 0: ALS_SD (0 = power on, 1 = shutdown)

#define VEML6030_ALS_SD_ENABLE      0x0000  // Bit 0 = 0 (power on)
#define VEML6030_ALS_SD_SHUTDOWN    0x0001  // Bit 0 = 1 (shutdown)

// Integration time settings (bits 3-2)
#define VEML6030_IT_25MS            0x000C  // 11 (actually 25ms)
#define VEML6030_IT_50MS            0x0008  // 10 (actually 50ms)  
#define VEML6030_IT_100MS           0x0000  // 00 (100ms)
#define VEML6030_IT_200MS           0x0004  // 01 (200ms)
#define VEML6030_IT_400MS           0x0008  // 10 (400ms)
#define VEML6030_IT_800MS           0x000C  // 11 (800ms)

// Persistence settings (bits 8-6)
#define VEML6030_PERS_1             0x0000  // 000
#define VEML6030_PERS_2             0x0040  // 001
#define VEML6030_PERS_4             0x0080  // 010  
#define VEML6030_PERS_8             0x00C0  // 011

// Gain settings (bits 12-11)
#define VEML6030_GAIN_1             0x0000  // 00 (1x gain)
#define VEML6030_GAIN_2             0x0800  // 01 (2x gain)
#define VEML6030_GAIN_1_4           0x1000  // 10 (1/8x gain)
#define VEML6030_GAIN_1_8           0x1800  // 11 (1/4x gain)

// Interrupt enable (bit 13)
#define VEML6030_INT_DISABLE        0x0000
#define VEML6030_INT_ENABLE         0x2000

// Configuration: Power on + 100ms integration time + interrupt disabled + persistence 1
#define VEML6030_CONFIG_ACTIVE      (VEML6030_ALS_SD_ENABLE | VEML6030_IT_100MS | VEML6030_PERS_1 | VEML6030_INT_DISABLE | VEML6030_GAIN_2)

// Button debounce timing
#define BUTTON_DEBOUNCE_US          50000  // 50ms in microseconds

// Light sensor timing
#define LIGHT_SENSOR_INTERVAL_US    5000000  // 5 seconds in microseconds

// Button debounce tracking
static uint64_t button_last_isr_time[3] = { 0 };

// I2C helper functions
static esp_err_t veml6030_write_reg(uint8_t reg, uint16_t value) {
    // VEML6030 uses little endian format
    uint8_t data[3] = {
        reg,
        (uint8_t)(value & 0xFF),        // LSB first
        (uint8_t)((value >> 8) & 0xFF)  // MSB second
    };

    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (VEML6030_I2C_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write(cmd, data, 3, true);
    i2c_master_stop(cmd);

    esp_err_t ret = i2c_master_cmd_begin(DAUGHTERBOARD_I2C_PORT, cmd, pdMS_TO_TICKS(1000));
    i2c_cmd_link_delete(cmd);

    if (ret == ESP_OK) {
        // Success - no logging needed
    }
    else {
        ESP_LOGE(TAG, "I2C write failed for reg 0x%02X: %s", reg, esp_err_to_name(ret));
    }

    return ret;
}

static esp_err_t veml6030_read_reg(uint8_t reg, uint16_t* value) {
    if (value == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret;
    uint8_t data[2] = { 0, 0 };

    // Use a single transaction with repeated start for proper I2C protocol
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();

    // Start condition
    i2c_master_start(cmd);

    // Write device address + write bit
    i2c_master_write_byte(cmd, (VEML6030_I2C_ADDR << 1) | I2C_MASTER_WRITE, true);

    // Write register address
    i2c_master_write_byte(cmd, reg, true);

    // Repeated start condition (no stop)
    i2c_master_start(cmd);

    // Write device address + read bit
    i2c_master_write_byte(cmd, (VEML6030_I2C_ADDR << 1) | I2C_MASTER_READ, true);

    // Read first byte with ACK
    i2c_master_read_byte(cmd, &data[0], I2C_MASTER_ACK);

    // Read second byte with NACK (last byte)
    i2c_master_read_byte(cmd, &data[1], I2C_MASTER_NACK);

    // Stop condition
    i2c_master_stop(cmd);

    ret = i2c_master_cmd_begin(DAUGHTERBOARD_I2C_PORT, cmd, pdMS_TO_TICKS(1000));
    i2c_cmd_link_delete(cmd);

    if (ret == ESP_OK) {
        // VEML6030 uses little endian format
        *value = (data[1] << 8) | data[0];
    }
    else {
        ESP_LOGE(TAG, "I2C read failed for reg 0x%02X: %s", reg, esp_err_to_name(ret));
        *value = 0;
    }

    return ret;
}

// Light sensor timer callback
static void light_sensor_timer_callback(void* arg) {
    uint16_t lux;
    esp_err_t ret = daughterboard_get_light_reading(&lux);

    if (ret == ESP_OK) {
        light_reading_t reading = {
            .lux = lux,
            .timestamp = (uint32_t)(esp_timer_get_time() / 1000)  // Convert to ms
        };

        esp_event_post(DAUGHTERBOARD_EVENTS,
            DAUGHTERBOARD_EVENT_LIGHT_READING,
            &reading,
            sizeof(reading),
            0);

        ESP_LOGI(TAG, "Light reading: %u lux", lux);
    }
    else {
        ESP_LOGW(TAG, "Failed to read light sensor: %s", esp_err_to_name(ret));

        // Try to read raw register for debugging
        uint16_t raw_debug;
        esp_err_t debug_ret = veml6030_read_reg(VEML6030_REG_ALS, &raw_debug);
        if (debug_ret == ESP_OK) {
            ESP_LOGW(TAG, "Raw ALS register value: %u", raw_debug);
        }
        else {
            ESP_LOGE(TAG, "Cannot even read raw ALS register: %s", esp_err_to_name(debug_ret));
        }
    }
}

// Button ISR handler
static void IRAM_ATTR button_isr_handler(void* arg) {
    ESP_EARLY_LOGI(TAG, "Button ISR triggered for GPIO %d", (int)arg);
    uint32_t button_id = (uint32_t)arg;
    uint64_t current_time = esp_timer_get_time();

    // Simple debounce check in ISR
    if (current_time - button_last_isr_time[button_id] < BUTTON_DEBOUNCE_US) {
        return;
    }
    button_last_isr_time[button_id] = current_time;

    daughterboard_event_t event_type;
    switch (button_id) {
    case 0: event_type = DAUGHTERBOARD_EVENT_BUTTON_A_PRESSED; break;
    case 1: event_type = DAUGHTERBOARD_EVENT_BUTTON_B_PRESSED; break;
    case 2: event_type = DAUGHTERBOARD_EVENT_BUTTON_C_PRESSED; break;
    default: return;
    }

    // Post event from ISR
    BaseType_t high_task_awoken = pdFALSE;
    esp_event_isr_post(DAUGHTERBOARD_EVENTS,
        event_type,
        NULL,
        0,
        &high_task_awoken);

    if (high_task_awoken == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

// Initialize I2C
static esp_err_t init_i2c(void) {
    i2c_config_t conf = {};
    conf.mode = I2C_MODE_MASTER;
    conf.sda_io_num = DAUGHTERBOARD_I2C_SDA_GPIO;
    conf.scl_io_num = DAUGHTERBOARD_I2C_SCL_GPIO;
    conf.sda_pullup_en = GPIO_PULLUP_DISABLE;  // External pullups
    conf.scl_pullup_en = GPIO_PULLUP_DISABLE;  // External pullups
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
        return ret;
    }
    return ESP_OK;
}

// Initialize VEML6030
static esp_err_t init_veml6030(void) {
    esp_err_t ret;

    // Ensure sensor is in shutdown mode
    ret = veml6030_write_reg(VEML6030_REG_ALS_CONF, VEML6030_ALS_SD_SHUTDOWN);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to shutdown VEML6030: %s", esp_err_to_name(ret));
        return ret;
    }

    // Wait a bit for shutdown
    vTaskDelay(pdMS_TO_TICKS(10));

    // Configure power saving mode (disable PSM for continuous operation)
    ret = veml6030_write_reg(VEML6030_REG_PSM, 0x0000);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure VEML6030 PSM: %s", esp_err_to_name(ret));
        return ret;
    }

    // Configure ALS settings and power on
    ret = veml6030_write_reg(VEML6030_REG_ALS_CONF, VEML6030_CONFIG_ACTIVE);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure VEML6030 ALS: %s", esp_err_to_name(ret));
        return ret;
    }

    // Wait for first measurement to complete (integration time + margin)
    vTaskDelay(pdMS_TO_TICKS(120));

    return ESP_OK;
}

// Initialize buttons with ISR
static esp_err_t init_buttons(void) {
    gpio_config_t io_conf = {
        .pin_bit_mask = ((1ULL << DAUGHTERBOARD_BUTTON_A_GPIO) |
                        (1ULL << DAUGHTERBOARD_BUTTON_B_GPIO) |
                        (1ULL << DAUGHTERBOARD_BUTTON_C_GPIO)),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_NEGEDGE,  // Trigger on falling edge (button press)
    };

    esp_err_t ret = gpio_config(&io_conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure button GPIOs: %s", esp_err_to_name(ret));
        return ret;
    }

    // Install GPIO ISR service
    ret = gpio_install_isr_service(0);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {  // ESP_ERR_INVALID_STATE means already installed
        ESP_LOGE(TAG, "Failed to install GPIO ISR service: %s", esp_err_to_name(ret));
        return ret;
    }

    // Add ISR handlers for each button
    gpio_num_t gpio_pins[] = { DAUGHTERBOARD_BUTTON_A_GPIO,
                              DAUGHTERBOARD_BUTTON_B_GPIO,
                              DAUGHTERBOARD_BUTTON_C_GPIO };

    for (int i = 0; i < 3; i++) {
        ret = gpio_isr_handler_add(gpio_pins[i], button_isr_handler, (void*)i);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to add ISR handler for button %d: %s", i, esp_err_to_name(ret));
            return ret;
        }
        button_last_isr_time[i] = 0;
    }

    return ESP_OK;
}

// Public API functions
esp_err_t daughterboard_init(void) {
    esp_err_t ret;

    // Initialize I2C
    ret = init_i2c();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize I2C: %s", esp_err_to_name(ret));
        return ret;
    }

    // Initialize VEML6030
    ret = init_veml6030();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize VEML6030: %s", esp_err_to_name(ret));
        return ret;
    }

    // Initialize buttons with ISR
    ret = init_buttons();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize buttons: %s", esp_err_to_name(ret));
        return ret;
    }

    // Create ESP timer for light sensor
    const esp_timer_create_args_t light_sensor_timer_args = {
        .callback = &light_sensor_timer_callback,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "light_sensor"
    };

    ret = esp_timer_create(&light_sensor_timer_args, &light_sensor_timer_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create light sensor timer: %s", esp_err_to_name(ret));
        return ret;
    }

    // Start the timer (auto-reload, 5 second interval)
    ret = esp_timer_start_periodic(light_sensor_timer_handle, LIGHT_SENSOR_INTERVAL_US);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start light sensor timer: %s", esp_err_to_name(ret));
        esp_timer_delete(light_sensor_timer_handle);
        light_sensor_timer_handle = NULL;
        return ret;
    }

    return ESP_OK;
}

esp_err_t daughterboard_deinit(void) {
    // Stop and delete timer
    if (light_sensor_timer_handle != NULL) {
        esp_timer_stop(light_sensor_timer_handle);
        esp_timer_delete(light_sensor_timer_handle);
        light_sensor_timer_handle = NULL;
    }

    // Remove ISR handlers
    gpio_num_t gpio_pins[] = { DAUGHTERBOARD_BUTTON_A_GPIO,
                              DAUGHTERBOARD_BUTTON_B_GPIO,
                              DAUGHTERBOARD_BUTTON_C_GPIO };

    for (int i = 0; i < 3; i++) {
        gpio_isr_handler_remove(gpio_pins[i]);
    }

    // Deinitialize I2C
    i2c_driver_delete(DAUGHTERBOARD_I2C_PORT);

    return ESP_OK;
}

esp_err_t daughterboard_get_light_reading(uint16_t* lux) {
    if (lux == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    uint16_t raw_value;
    esp_err_t ret = veml6030_read_reg(VEML6030_REG_ALS, &raw_value);

    if (ret == ESP_OK) {
        // Convert raw value to lux according to VEML6030 datasheet
        // For gain 1x and integration time 100ms:
        // Resolution = 0.0288 lux/count
        // But we need to use proper float conversion
        float lux_float = (float)raw_value * 0.0288f;
        *lux = (uint16_t)(lux_float + 0.5f);  // Round to nearest integer
    }
    else {
        ESP_LOGW(TAG, "Failed to read VEML6030 ALS register: %s", esp_err_to_name(ret));
        *lux = 0;
    }

    return ret;
}

bool daughterboard_is_button_pressed(uint8_t button_id) {
    if (button_id >= 3) {
        return false;
    }

    gpio_num_t gpio_pins[] = { DAUGHTERBOARD_BUTTON_A_GPIO,
                              DAUGHTERBOARD_BUTTON_B_GPIO,
                              DAUGHTERBOARD_BUTTON_C_GPIO };

    // Read current button state directly (active low)
    return !gpio_get_level(gpio_pins[button_id]);
}

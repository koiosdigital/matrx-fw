// Auto brightness - adjusts display brightness based on ambient light
#include "auto_brightness.h"

#include <cmath>
#include <esp_log.h>
#include <esp_event.h>

#include "config.h"
#include "display.h"
#include "daughterboard.h"

static const char* TAG = "auto_brightness";

namespace {

//------------------------------------------------------------------------------
// VEML6030 Gain Constants
//------------------------------------------------------------------------------

// ALS_CONF register gain bits (bits 12:11)
constexpr uint16_t GAIN_2   = 0x0800;  // 2x gain, highest sensitivity
constexpr uint16_t GAIN_1   = 0x0000;  // 1x gain
constexpr uint16_t GAIN_1_4 = 0x1800;  // 1/4 gain
constexpr uint16_t GAIN_1_8 = 0x1000;  // 1/8 gain, lowest sensitivity

// Integration time (100ms, bits 9:6 = 0000)
constexpr uint16_t IT_100MS = 0x0000;

// Resolution (lux per raw count) for each gain at 100ms IT
constexpr float RESOLUTION_GAIN_2   = 0.0288f;
constexpr float RESOLUTION_GAIN_1   = 0.0576f;
constexpr float RESOLUTION_GAIN_1_4 = 0.2304f;
constexpr float RESOLUTION_GAIN_1_8 = 0.4608f;

//------------------------------------------------------------------------------
// Auto-range Thresholds
//------------------------------------------------------------------------------

constexpr uint16_t HIGH_THRESHOLD = 50000;  // ~80% of 16-bit max, decrease gain
constexpr uint16_t LOW_THRESHOLD  = 1000;   // Low signal, increase gain

//------------------------------------------------------------------------------
// Brightness Mapping Constants
//------------------------------------------------------------------------------

constexpr uint8_t MIN_BRIGHTNESS = 8;    // Minimum visible brightness
constexpr uint8_t MAX_BRIGHTNESS = 255;  // Maximum brightness
constexpr float HYSTERESIS_LUX = 2.0f;   // Prevent oscillation at threshold
constexpr float SMOOTHING_FACTOR = 0.3f; // EMA alpha

//------------------------------------------------------------------------------
// State
//------------------------------------------------------------------------------

struct AutoBrightnessState {
    uint16_t current_gain = GAIN_2;
    float smoothed_lux = 0.0f;
    bool screen_is_off = false;
    bool initialized = false;
};

AutoBrightnessState state;

//------------------------------------------------------------------------------
// Gain Helpers
//------------------------------------------------------------------------------

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
        case GAIN_1_8: return GAIN_1_8;  // Already at minimum
        default:       return GAIN_1_8;
    }
}

uint16_t increase_gain(uint16_t current) {
    switch (current) {
        case GAIN_1_8: return GAIN_1_4;
        case GAIN_1_4: return GAIN_1;
        case GAIN_1:   return GAIN_2;
        case GAIN_2:   return GAIN_2;  // Already at maximum
        default:       return GAIN_2;
    }
}

bool adjust_gain_if_needed(uint16_t raw) {
    uint16_t new_gain = state.current_gain;

    if (raw > HIGH_THRESHOLD) {
        new_gain = decrease_gain(state.current_gain);
        if (new_gain != state.current_gain) {
            ESP_LOGI(TAG, "Decreasing gain: raw=%u > %u", raw, HIGH_THRESHOLD);
        }
    } else if (raw < LOW_THRESHOLD && state.smoothed_lux < 100.0f) {
        new_gain = increase_gain(state.current_gain);
        if (new_gain != state.current_gain) {
            ESP_LOGI(TAG, "Increasing gain: raw=%u < %u, lux=%.1f",
                     raw, LOW_THRESHOLD, state.smoothed_lux);
        }
    }

    if (new_gain != state.current_gain) {
        state.current_gain = new_gain;
        daughterboard_set_veml_config(new_gain | IT_100MS);
        return true;  // Gain changed, skip this reading
    }

    return false;
}

//------------------------------------------------------------------------------
// Brightness Mapping
//------------------------------------------------------------------------------

uint8_t lux_to_brightness(float lux) {
    if (lux <= 1.0f) {
        return MIN_BRIGHTNESS;
    }
    if (lux >= 1000.0f) {
        return MAX_BRIGHTNESS;
    }

    // Logarithmic interpolation: log(1)=0, log(1000)=3
    float normalized = std::log10(lux) / 3.0f;
    float brightness = MIN_BRIGHTNESS + (MAX_BRIGHTNESS - MIN_BRIGHTNESS) * normalized;

    if (brightness < MIN_BRIGHTNESS) brightness = MIN_BRIGHTNESS;
    if (brightness > MAX_BRIGHTNESS) brightness = MAX_BRIGHTNESS;

    return static_cast<uint8_t>(brightness + 0.5f);
}

//------------------------------------------------------------------------------
// Event Handler
//------------------------------------------------------------------------------

void on_light_reading(void*, esp_event_base_t, int32_t, void* event_data) {
    if (!event_data) return;

    auto* reading = static_cast<light_reading_t*>(event_data);
    uint16_t raw = reading->raw;

    system_config_t config = config_get();

    // If auto brightness disabled, do nothing
    if (!config.auto_brightness_enabled) {
        return;
    }

    // Check if gain adjustment needed (skip reading if changed)
    if (adjust_gain_if_needed(raw)) {
        return;
    }

    // Convert raw to lux using current gain's resolution
    float lux = raw * get_resolution(state.current_gain);

    // Apply exponential moving average for smoothing
    if (!state.initialized) {
        state.smoothed_lux = lux;
        state.initialized = true;
    } else {
        state.smoothed_lux = state.smoothed_lux * (1.0f - SMOOTHING_FACTOR)
                           + lux * SMOOTHING_FACTOR;
    }

    // Screen off threshold with hysteresis
    float screen_off_threshold = static_cast<float>(config.screen_off_lux);
    float screen_on_threshold = screen_off_threshold + HYSTERESIS_LUX;

    if (state.screen_is_off) {
        // Screen is off, check if we should turn it on
        if (state.smoothed_lux >= screen_on_threshold) {
            state.screen_is_off = false;
            ESP_LOGI(TAG, "Screen on: lux=%.1f >= %.1f",
                     state.smoothed_lux, screen_on_threshold);
            // Fall through to set brightness
        } else {
            return;  // Stay off
        }
    } else {
        // Screen is on, check if we should turn it off
        if (state.smoothed_lux < screen_off_threshold) {
            state.screen_is_off = true;
            display_set_brightness(0);
            ESP_LOGI(TAG, "Screen off: lux=%.1f < %.1f",
                     state.smoothed_lux, screen_off_threshold);
            return;
        }
    }

    // Calculate and apply brightness
    uint8_t brightness = lux_to_brightness(state.smoothed_lux);
    display_set_brightness(brightness);

    ESP_LOGD(TAG, "lux=%.1f (raw=%u, gain=0x%04x) -> brightness=%u",
             state.smoothed_lux, raw, state.current_gain, brightness);
}

}  // namespace

void auto_brightness_init() {
    // Register event handler for light readings
    esp_event_handler_register(
        DAUGHTERBOARD_EVENTS,
        DAUGHTERBOARD_EVENT_LIGHT_READING,
        on_light_reading,
        nullptr
    );

    // Start with gain 2 (most sensitive)
    state.current_gain = GAIN_2;
    daughterboard_set_veml_config(GAIN_2 | IT_100MS);

    ESP_LOGI(TAG, "Auto brightness initialized");
}

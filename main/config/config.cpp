#include "config.h"

#include <cstring>
#include <ctime>
#include <esp_log.h>
#include <esp_timer.h>
#include <nvs_flash.h>
#include <nvs.h>
#include <esp_system.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <kd_common.h>

#include "raii_utils.hpp"
#include "display.h"
#include "apps.h"
#include "scheduler.h"
#include "messages.h"

static const char* TAG = "config";

namespace {

constexpr const char* NVS_NAMESPACE = "sys_cfg";
constexpr const char* NVS_KEY = "cfg";

constexpr system_config_t DEFAULT_CONFIG = {
    .screen_enabled = true,
    .screen_brightness = 128,
    .auto_brightness_enabled = false,
    .screen_off_lux = 1,
    .quiet_windows = {},
    .quiet_window_count = 0
};

system_config_t g_config = DEFAULT_CONFIG;
SemaphoreHandle_t g_mutex = nullptr;

// Display power state. The display is ON only when screen_enabled AND not in
// quiet hours AND not ambient-dark (auto-brightness). All three feed the single
// arbiter below; nothing else may call display_set_brightness.
bool g_quiet_active = false;   // inside an enabled quiet-hours window
bool g_ambient_off = false;    // daughterboard: lux below screen_off_lux
int16_t g_applied_brightness = -1;  // last value written to the panel (-1 = unknown)
esp_timer_handle_t g_quiet_timer = nullptr;

// How often the evaluator re-checks the wall clock against the quiet windows.
constexpr int64_t QUIET_EVAL_INTERVAL_US = 30 * 1000 * 1000;

constexpr TickType_t CONFIG_MUTEX_TIMEOUT = pdMS_TO_TICKS(100);

void erase_nvs_namespace(const char* ns) {
    nvs_handle_t h;
    if (nvs_open(ns, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    }
}

esp_err_t load_from_nvs() {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) return err;

    size_t size = sizeof(system_config_t);
    err = nvs_get_blob(handle, NVS_KEY, &g_config, &size);
    nvs_close(handle);

    if (err != ESP_OK || size != sizeof(system_config_t)) {
        g_config = DEFAULT_CONFIG;
        return ESP_ERR_NOT_FOUND;
    }
    return ESP_OK;
}

esp_err_t save_to_nvs() {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS open failed: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_set_blob(handle, NVS_KEY, &g_config, sizeof(system_config_t));
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS save failed: %s", esp_err_to_name(err));
    }
    return err;
}

// Single display-power arbiter. Computes whether the display should be on,
// writes brightness to the panel only when it actually changes, and returns
// whether the render pipeline should be paused. Caller must hold g_mutex.
bool apply_display_power_locked() {
    const bool ambient_off = g_ambient_off && g_config.auto_brightness_enabled;
    const bool on = g_config.screen_enabled && !g_quiet_active && !ambient_off;

    const uint8_t target = on ? g_config.screen_brightness : 0;
    if (g_applied_brightness != static_cast<int16_t>(target)) {
        display_set_brightness(target);
        g_applied_brightness = target;
        ESP_LOGI(TAG, "Display %s (brightness %u%s%s%s)", on ? "ON" : "OFF", target,
                 g_quiet_active ? ", quiet" : "",
                 ambient_off ? ", ambient" : "",
                 !g_config.screen_enabled ? ", disabled" : "");
    }
    return !on;
}

// Reconcile the panel brightness and render pipeline with the current state.
// Must NOT be called with g_mutex held (calls into the scheduler). The
// scheduler pause/resume calls are idempotent, so calling this repeatedly
// (30 s quiet tick, light-sensor ticks, config echoes) is cheap and also
// self-heals if an earlier pause/resume hit a mutex timeout.
void sync_display_power() {
    bool want_paused;
    {
        raii::MutexGuard lock(g_mutex, CONFIG_MUTEX_TIMEOUT);
        if (!lock) return;
        want_paused = apply_display_power_locked();
    }

    if (want_paused) {
        scheduler_pause();
    } else {
        scheduler_resume();
    }
}

// Evaluate the current wall clock against the configured quiet windows.
// Caller must hold g_mutex. Fails open (false) until the clock is NTP-synced.
bool compute_quiet_now_locked() {
    if (!kd_common_ntp_is_synced()) return false;

    time_t now = time(nullptr);
    struct tm lt;
    localtime_r(&now, &lt);
    if (lt.tm_year < 120) return false;  // year < 2020 => clock not yet valid

    const uint8_t dow = static_cast<uint8_t>(lt.tm_wday);  // 0=Sunday..6=Saturday
    const int now_min = lt.tm_hour * 60 + lt.tm_min;

    uint8_t n = g_config.quiet_window_count;
    if (n > MATRX_MAX_QUIET_WINDOWS) n = MATRX_MAX_QUIET_WINDOWS;

    for (uint8_t i = 0; i < n; i++) {
        const quiet_window_t& w = g_config.quiet_windows[i];
        if (!w.enabled) continue;

        const int start_min = w.start_hour * 60 + w.start_min;
        const int end_min = w.end_hour * 60 + w.end_min;
        if (start_min == end_min) continue;  // zero-length window is inert

        if (start_min < end_min) {
            // Same-day window.
            if ((w.day_mask & (1u << dow)) && now_min >= start_min && now_min < end_min) {
                return true;
            }
        } else {
            // Wraps past midnight: [start,24:00) on the start day, or
            // [00:00,end) counted against the previous day's bit.
            if ((w.day_mask & (1u << dow)) && now_min >= start_min) return true;
            const uint8_t prev_dow = (dow + 6) % 7;
            if ((w.day_mask & (1u << prev_dow)) && now_min < end_min) return true;
        }
    }
    return false;
}

// Recompute quiet state, reconcile display + pipeline, and on a quiet
// transition report the new is_quiet_now to the cloud. Must NOT be called with
// g_mutex held (it re-locks and calls out to scheduler/messages).
void evaluate_quiet() {
    bool changed;
    {
        raii::MutexGuard lock(g_mutex, CONFIG_MUTEX_TIMEOUT);
        if (!lock) return;

        bool quiet = compute_quiet_now_locked();
        changed = (quiet != g_quiet_active);
        g_quiet_active = quiet;
    }

    sync_display_power();

    if (changed) {
        msg_send_device_config();  // surface is_quiet_now to app/cloud
    }
}

void quiet_timer_callback(void*) {
    evaluate_quiet();
}

}  // namespace

esp_err_t config_init() {
    g_mutex = xSemaphoreCreateMutex();
    if (!g_mutex) {
        ESP_LOGE(TAG, "Failed to create mutex");
        return ESP_ERR_NO_MEM;
    }

    esp_err_t ret = load_from_nvs();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Using defaults (NVS load: %s)", esp_err_to_name(ret));
        save_to_nvs();
    }

    sync_display_power();

    esp_timer_create_args_t quiet_args = {
        .callback = quiet_timer_callback,
        .arg = nullptr,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "quiet_eval",
        .skip_unhandled_events = true,
    };
    if (esp_timer_create(&quiet_args, &g_quiet_timer) == ESP_OK) {
        esp_timer_start_periodic(g_quiet_timer, QUIET_EVAL_INTERVAL_US);
    }

    return ESP_OK;
}

system_config_t config_get() {
    raii::MutexGuard lock(g_mutex, CONFIG_MUTEX_TIMEOUT);
    return lock ? g_config : DEFAULT_CONFIG;
}

esp_err_t config_set(const system_config_t* config) {
    if (!config) return ESP_ERR_INVALID_ARG;

    esp_err_t ret = ESP_OK;
    {
        raii::MutexGuard lock(g_mutex, CONFIG_MUTEX_TIMEOUT);
        if (!lock) return ESP_ERR_TIMEOUT;

        // The cloud echoes back every config the device reports; skip the NVS
        // write (flash wear) when nothing actually changed.
        if (memcmp(&g_config, config, sizeof(*config)) != 0) {
            g_config = *config;
            ret = save_to_nvs();
        }
    }

    // Quiet windows / brightness / screen_enabled may have changed; re-evaluate
    // and reconcile immediately (outside the lock, since this calls into the
    // scheduler and message layer).
    evaluate_quiet();
    return ret;
}

bool config_is_quiet_now() {
    raii::MutexGuard lock(g_mutex, CONFIG_MUTEX_TIMEOUT);
    return lock ? g_quiet_active : false;
}

void config_set_ambient_screen_off(bool off) {
    {
        raii::MutexGuard lock(g_mutex, CONFIG_MUTEX_TIMEOUT);
        if (!lock) return;
        if (g_ambient_off == off) return;
        g_ambient_off = off;
    }
    sync_display_power();
}

void perform_factory_reset(const char* reason) {
    show_fs_sprite("factory_reset_hold");

    if (esp_wifi_restore() != ESP_OK) {
        erase_nvs_namespace("nvs.net80211");
    }

    erase_nvs_namespace(NVS_NAMESPACE);

    show_fs_sprite("factory_reset_success");
    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_restart();
}

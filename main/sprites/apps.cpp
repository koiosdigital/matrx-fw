// App Manager - unified schedule item and sprite data management
#include "apps.h"

#include <cstring>
#include <esp_log.h>
#include <esp_heap_caps.h>

#include "webp_player.h"
#include "static_files.h"
#include "matrx.pb-c.h"

static const char* TAG = "apps";

#define MAX_APPS 48

namespace {

    // Storage
    App_t* g_apps[MAX_APPS] = { nullptr };
    size_t g_app_count = 0;
    SemaphoreHandle_t g_apps_mutex = nullptr;

    // RAII mutex guard
    class MutexGuard {
    public:
        explicit MutexGuard(SemaphoreHandle_t m) : mutex_(m), locked_(false) {
            if (mutex_) {
                locked_ = (xSemaphoreTake(mutex_, portMAX_DELAY) == pdTRUE);
            }
        }
        ~MutexGuard() {
            if (locked_ && mutex_) {
                xSemaphoreGive(mutex_);
            }
        }
        explicit operator bool() const { return locked_; }

        MutexGuard(const MutexGuard&) = delete;
        MutexGuard& operator=(const MutexGuard&) = delete;

    private:
        SemaphoreHandle_t mutex_;
        bool locked_;
    };

    // Allocate in SPIRAM with zero-init
    uint8_t* alloc_spiram(size_t size) {
        return static_cast<uint8_t*>(heap_caps_calloc(size, 1, MALLOC_CAP_SPIRAM));
    }

    // Compare UUIDs
    bool uuid_equal(const uint8_t* a, const uint8_t* b) {
        return std::memcmp(a, b, 16) == 0;
    }

    // Find app index by UUID (must hold g_apps_mutex)
    int find_app_index_unlocked(const uint8_t* uuid) {
        for (size_t i = 0; i < g_app_count; i++) {
            if (g_apps[i] && uuid_equal(g_apps[i]->uuid, uuid)) {
                return static_cast<int>(i);
            }
        }
        return -1;
    }

    // Create a new app (must hold g_apps_mutex)
    App_t* create_app_unlocked(const uint8_t* uuid) {
        if (g_app_count >= MAX_APPS) {
            ESP_LOGE(TAG, "Max apps reached (%d)", MAX_APPS);
            return nullptr;
        }

        auto* app = static_cast<App_t*>(heap_caps_calloc(1, sizeof(App_t), MALLOC_CAP_SPIRAM));
        if (!app) {
            ESP_LOGE(TAG, "Failed to allocate app");
            return nullptr;
        }

        app->mutex = xSemaphoreCreateMutex();
        if (!app->mutex) {
            ESP_LOGE(TAG, "Failed to create app mutex");
            heap_caps_free(app);
            return nullptr;
        }

        std::memcpy(app->uuid, uuid, 16);
        g_apps[g_app_count++] = app;
        return app;
    }

    // Free an app (must hold g_apps_mutex, app mutex must NOT be held)
    void free_app_unlocked(App_t* app) {
        if (!app) return;

        if (app->mutex) {
            xSemaphoreTake(app->mutex, portMAX_DELAY);
            vSemaphoreDelete(app->mutex);
        }

        heap_caps_free(app->data);
        heap_caps_free(app);
    }

    // Remove app at index (must hold g_apps_mutex)
    void remove_app_at_unlocked(size_t index) {
        if (index >= g_app_count) return;

        App_t* app = g_apps[index];

        // Shift remaining apps down
        for (size_t i = index; i < g_app_count - 1; i++) {
            g_apps[i] = g_apps[i + 1];
        }
        g_apps[--g_app_count] = nullptr;

        free_app_unlocked(app);
    }

}  // namespace

void apps_init() {
    if (!g_apps_mutex) {
        g_apps_mutex = xSemaphoreCreateMutex();
    }
    ESP_LOGI(TAG, "Apps manager initialized");
}

void apps_cleanup() {
    MutexGuard lock(g_apps_mutex);
    if (!lock) return;

    for (size_t i = 0; i < g_app_count; i++) {
        free_app_unlocked(g_apps[i]);
        g_apps[i] = nullptr;
    }
    g_app_count = 0;

    ESP_LOGI(TAG, "Apps manager cleaned up");
}

void apps_sync_schedule(Kd__V1__ScheduleItem** items, size_t count) {
    MutexGuard lock(g_apps_mutex);
    if (!lock) return;

    // Mark all existing apps for potential removal
    bool keep[MAX_APPS] = { false };

    // Process incoming schedule items
    for (size_t i = 0; i < count; i++) {
        Kd__V1__ScheduleItem* item = items[i];
        if (!item || item->uuid.len != 16) continue;

        const uint8_t* uuid = item->uuid.data;
        int idx = find_app_index_unlocked(uuid);

        App_t* app;
        if (idx >= 0) {
            // Existing app - update metadata
            app = g_apps[idx];
            keep[idx] = true;
        }
        else {
            // New app - create it
            app = create_app_unlocked(uuid);
            if (!app) continue;
            keep[g_app_count - 1] = true;
        }

        // Update metadata (no lock needed - only this function modifies these)
        app->display_time = item->display_time;
        app->pinned = item->pinned;
        app->skipped = item->skipped;
    }

    // Remove apps not in the new schedule (iterate backwards to handle shifting)
    for (int i = static_cast<int>(g_app_count) - 1; i >= 0; i--) {
        if (!keep[i]) {
            ESP_LOGI(TAG, "Removing app at index %d", i);
            remove_app_at_unlocked(static_cast<size_t>(i));
        }
    }

    ESP_LOGI(TAG, "Schedule synced: %zu apps", g_app_count);
}

App_t* app_find(const uint8_t* uuid) {
    if (!uuid) return nullptr;

    MutexGuard lock(g_apps_mutex);
    if (!lock) return nullptr;

    int idx = find_app_index_unlocked(uuid);
    return (idx >= 0) ? g_apps[idx] : nullptr;
}

size_t apps_count() {
    MutexGuard lock(g_apps_mutex);
    return lock ? g_app_count : 0;
}

App_t* apps_get_by_index(size_t index) {
    MutexGuard lock(g_apps_mutex);
    if (!lock || index >= g_app_count) return nullptr;
    return g_apps[index];
}

void app_set_data(App_t* app, const uint8_t* data, size_t len) {
    if (!app || !app->mutex) return;

    MutexGuard lock(app->mutex);
    if (!lock) return;

    // Free old data
    heap_caps_free(app->data);
    app->data = nullptr;
    app->len = 0;

    if (!data || len == 0) return;

    // Allocate and copy new data
    app->data = alloc_spiram(len);
    if (!app->data) {
        ESP_LOGE(TAG, "Failed to allocate %zu bytes", len);
        return;
    }

    std::memcpy(app->data, data, len);
    app->len = len;
}

void app_clear_data(App_t* app) {
    if (!app || !app->mutex) return;

    MutexGuard lock(app->mutex);
    if (!lock) return;

    heap_caps_free(app->data);
    app->data = nullptr;
    app->len = 0;
}

bool app_is_displayable(App_t* app) {
    if (!app || !app->mutex) return false;

    MutexGuard lock(app->mutex);
    if (!lock) return false;

    return app->len > 0 && !app->skipped;
}

void app_show(App_t* app) {
    if (!app || !app_is_displayable(app)) {
        ESP_LOGD(TAG, "app_show: app not displayable");
        return;
    }

    // Use webp_player to display the app
    // The player will copy the data internally, so we don't need to manage a buffer here
    webp_player_play_app(app, app->display_time, true);
}

void show_fs_sprite(const char* name) {
    if (!name) return;

    // Use webp_player for embedded sprites
    // Embedded sprites loop forever until stopped or replaced
    webp_player_play_embedded(name, true);
}

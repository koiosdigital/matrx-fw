#include "apps.h"

#include <cstring>
#include <esp_log.h>
#include <esp_heap_caps.h>

#include "raii_utils.hpp"
#include "webp_player.h"
#include "static_files.h"
#include "matrx.pb-c.h"

static const char* TAG = "apps";

#define MAX_APPS 48

namespace {

    App_t* g_apps[MAX_APPS] = { nullptr };
    size_t g_app_count = 0;
    SemaphoreHandle_t g_apps_mutex = nullptr;

    uint8_t* alloc_spiram(size_t size) {
        return static_cast<uint8_t*>(heap_caps_calloc(size, 1, MALLOC_CAP_SPIRAM));
    }

    bool uuid_equal(const uint8_t* a, const uint8_t* b) {
        return std::memcmp(a, b, 16) == 0;
    }

    int find_app_index_unlocked(const uint8_t* uuid) {
        for (size_t i = 0; i < g_app_count; i++) {
            if (g_apps[i] && uuid_equal(g_apps[i]->uuid, uuid)) {
                return static_cast<int>(i);
            }
        }
        return -1;
    }

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
        app->displayable = true;
        g_apps[g_app_count++] = app;
        return app;
    }

    void free_app_unlocked(App_t* app) {
        if (!app) return;

        if (app->mutex) {
            xSemaphoreTake(app->mutex, portMAX_DELAY);
            vSemaphoreDelete(app->mutex);
        }

        heap_caps_free(app->data);
        heap_caps_free(app);
    }

    void remove_app_at_unlocked(size_t index) {
        if (index >= g_app_count) return;

        App_t* app = g_apps[index];

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
}

void apps_cleanup() {
    raii::MutexGuard lock(g_apps_mutex);
    if (!lock) return;

    for (size_t i = 0; i < g_app_count; i++) {
        free_app_unlocked(g_apps[i]);
        g_apps[i] = nullptr;
    }
    g_app_count = 0;
}

void apps_sync_schedule(Kd__V1__ScheduleItem** items, size_t count) {
    raii::MutexGuard lock(g_apps_mutex);
    if (!lock) return;

    bool keep[MAX_APPS] = { false };

    for (size_t i = 0; i < count; i++) {
        Kd__V1__ScheduleItem* item = items[i];
        if (!item || item->uuid.len != 16) continue;

        const uint8_t* uuid = item->uuid.data;
        int idx = find_app_index_unlocked(uuid);

        App_t* app;
        if (idx >= 0) {
            app = g_apps[idx];
            keep[idx] = true;
        }
        else {
            app = create_app_unlocked(uuid);
            if (!app) continue;
            keep[g_app_count - 1] = true;
        }

        app->display_time = item->display_time;
        app->pinned = item->pinned;
        app->skipped = item->skipped;
    }

    for (int i = static_cast<int>(g_app_count) - 1; i >= 0; i--) {
        if (!keep[i]) {
            remove_app_at_unlocked(static_cast<size_t>(i));
        }
    }
}

App_t* app_find(const uint8_t* uuid) {
    if (!uuid) return nullptr;

    raii::MutexGuard lock(g_apps_mutex);
    if (!lock) return nullptr;

    int idx = find_app_index_unlocked(uuid);
    return (idx >= 0) ? g_apps[idx] : nullptr;
}

size_t apps_count() {
    raii::MutexGuard lock(g_apps_mutex);
    return lock ? g_app_count : 0;
}

App_t* apps_get_by_index(size_t index) {
    raii::MutexGuard lock(g_apps_mutex);
    if (!lock || index >= g_app_count) return nullptr;
    return g_apps[index];
}

void app_set_data(App_t* app, const uint8_t* data, size_t len) {
    if (!app || !app->mutex) return;

    raii::MutexGuard lock(app->mutex);
    if (!lock) return;

    heap_caps_free(app->data);
    app->data = nullptr;
    app->len = 0;

    if (!data || len == 0) return;

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

    raii::MutexGuard lock(app->mutex);
    if (!lock) return;

    heap_caps_free(app->data);
    app->data = nullptr;
    app->len = 0;
    app->etag[0] = '\0';
}

void app_set_etag(App_t* app, const char* etag) {
    if (!app || !app->mutex) return;

    raii::MutexGuard lock(app->mutex);
    if (!lock) return;

    if (!etag) {
        app->etag[0] = '\0';
        return;
    }
    strlcpy(app->etag, etag, sizeof(app->etag));
}

bool app_copy_etag_if_has_data(App_t* app, char* out, size_t out_size) {
    if (out && out_size) out[0] = '\0';
    if (!app || !app->mutex || !out || out_size == 0) return false;

    raii::MutexGuard lock(app->mutex);
    if (!lock) return false;

    if (app->len == 0 || app->etag[0] == '\0') return false;
    strlcpy(out, app->etag, out_size);
    return true;
}

void app_set_displayable(App_t* app, bool displayable) {
    if (!app || !app->mutex) return;

    raii::MutexGuard lock(app->mutex);
    if (!lock) return;

    app->displayable = displayable;
}

bool app_has_data(App_t* app) {
    if (!app || !app->mutex) return false;

    raii::MutexGuard lock(app->mutex);
    if (!lock) return false;

    return app->len > 0;
}

bool app_is_qualified(App_t* app) {
    if (!app || !app->mutex) return false;

    raii::MutexGuard lock(app->mutex);
    if (!lock) return false;

    return app->len > 0 && app->displayable && !app->skipped;
}

void app_show(App_t* app) {
    if (!app || !app_is_qualified(app)) {
        return;
    }

    webp_player_play_app(app, app->display_time);
}

void show_fs_sprite(const char* name) {
    if (!name) return;

    webp_player_play_embedded(name);
}

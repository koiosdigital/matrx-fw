// App Manager - unified schedule item and sprite data management
#pragma once

#include <cstdint>
#include <cstddef>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#ifdef __cplusplus
extern "C" {
#endif

    // Forward declare protobuf type
    struct Kd__V1__ScheduleItem;

    // Single app instance
    typedef struct {
        uint8_t uuid[16];         // 16-byte binary UUID
        uint8_t sha256[32];       // 32-byte SHA-256 hash
        uint8_t* data;            // Sprite data buffer (SPIRAM)
        size_t len;               // Data length
        uint32_t display_time;    // Display duration in seconds (from schedule)
        bool pinned;              // Pinned state (from schedule)
        bool skipped;             // Skipped state (from schedule)
        SemaphoreHandle_t mutex;  // Per-app mutex
    } App_t;

    // Initialization
    void apps_init();
    void apps_cleanup();

    // Schedule sync - adds new apps, removes unlisted ones, updates metadata
    void apps_sync_schedule(Kd__V1__ScheduleItem** items, size_t count);

    // Lookup
    App_t* app_find(const uint8_t* uuid);
    size_t apps_count();
    App_t* apps_get_by_index(size_t index);

    // Data management
    void app_set_data(App_t* app, const uint8_t* data, size_t len);
    void app_clear_data(App_t* app);
    bool app_is_displayable(App_t* app);

    // Display
    void app_show(App_t* app);
    void show_fs_sprite(const char* name);

#ifdef __cplusplus
}
#endif

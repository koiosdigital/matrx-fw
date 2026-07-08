#pragma once

#include <cstdint>
#include <cstddef>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#ifdef __cplusplus
extern "C" {
#endif

    struct Kd__V1__ScheduleItem;

    #define APP_ETAG_MAX 96

    typedef struct {
        uint8_t uuid[16];
        char etag[APP_ETAG_MAX];
        uint8_t* data;
        size_t len;
        uint32_t display_time;
        bool pinned;
        bool skipped;
        bool displayable;
        SemaphoreHandle_t mutex;
    } App_t;

    void apps_init();
    void apps_cleanup();

    void apps_sync_schedule(Kd__V1__ScheduleItem** items, size_t count);

    App_t* app_find(const uint8_t* uuid);
    size_t apps_count();
    App_t* apps_get_by_index(size_t index);

    void app_set_data(App_t* app, const uint8_t* data, size_t len);
    void app_clear_data(App_t* app);
    void app_set_displayable(App_t* app, bool displayable);
    bool app_has_data(App_t* app);
    bool app_is_qualified(App_t* app);

    void app_set_etag(App_t* app, const char* etag);
    bool app_copy_etag_if_has_data(App_t* app, char* out, size_t out_size);

    void app_show(App_t* app);
    void show_fs_sprite(const char* name);

#ifdef __cplusplus
}
#endif

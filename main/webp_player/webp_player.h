#pragma once

#include <esp_event.h>
#include <esp_err.h>
#include <cstdint>
#include <cstddef>

#include "apps.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WEBP_PLAYER_RETRY_COUNT         3
#define WEBP_PLAYER_RETRY_DELAY_MS      200
#define WEBP_PLAYER_TASK_STACK_SIZE     4096
#define WEBP_PLAYER_TASK_PRIORITY       5
#define WEBP_PLAYER_TASK_CORE           1

    ESP_EVENT_DECLARE_BASE(WEBP_PLAYER_EVENTS);

    typedef enum {
        WEBP_PLAYER_EVT_PLAYING,
        WEBP_PLAYER_EVT_ERROR,
        WEBP_PLAYER_EVT_STOPPED,
    } webp_player_event_id_t;

    typedef enum {
        WEBP_SOURCE_RAM,
        WEBP_SOURCE_EMBEDDED,
    } webp_source_type_t;

    typedef struct {
        webp_source_type_t source_type;
        App_t* ram_app;
        const char* embedded_name;
        uint32_t duration_ms;
        uint32_t frame_count;
    } webp_player_playing_evt_t;

    typedef struct {
        webp_source_type_t source_type;
        App_t* ram_app;
        const char* embedded_name;
        int error_code;
    } webp_player_error_evt_t;

    esp_err_t webp_player_init(void);
    void webp_player_deinit(void);

    esp_err_t webp_player_play_app(App_t* app, uint32_t duration_ms);
    esp_err_t webp_player_play_embedded(const char* name);
    esp_err_t webp_player_stop(void);

    bool webp_player_is_playing(void);

#ifdef __cplusplus
}
#endif

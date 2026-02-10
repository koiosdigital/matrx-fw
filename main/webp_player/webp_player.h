// WebP Player - Lightweight event-driven animated WebP playback task
// Simplified state machine: IDLE <-> PLAYING
// Scheduler handles all timing (prepare timer, retry timer)
#pragma once

#include <esp_event.h>
#include <esp_err.h>
#include <cstdint>
#include <cstddef>

#include "apps.h"

#ifdef __cplusplus
extern "C" {
#endif

//------------------------------------------------------------------------------
// Configuration
//------------------------------------------------------------------------------

#define WEBP_PLAYER_RETRY_COUNT         3       // Decode error retries
#define WEBP_PLAYER_RETRY_DELAY_MS      200     // Delay between retries
#define WEBP_PLAYER_TASK_STACK_SIZE     4096
#define WEBP_PLAYER_TASK_PRIORITY       10
#define WEBP_PLAYER_TASK_CORE           1

//------------------------------------------------------------------------------
// Event Base and IDs
//------------------------------------------------------------------------------

ESP_EVENT_DECLARE_BASE(WEBP_PLAYER_EVENTS);

typedef enum {
    // Status events (player -> external) - posted to ESP event loop
    WEBP_PLAYER_EVT_PLAYING,        // Now playing content
    WEBP_PLAYER_EVT_ERROR,          // Decode error after retries
    WEBP_PLAYER_EVT_STOPPED,        // Playback stopped (duration expired or explicit stop)
} webp_player_event_id_t;

//------------------------------------------------------------------------------
// Source Type - RAM app vs Embedded sprite
//------------------------------------------------------------------------------

typedef enum {
    WEBP_SOURCE_RAM,        // App_t* from apps manager (SPIRAM, copied)
    WEBP_SOURCE_EMBEDDED,   // Embedded sprite from flash (direct pointer)
} webp_source_type_t;

//------------------------------------------------------------------------------
// Event Payload: PLAYING
//------------------------------------------------------------------------------

typedef struct {
    webp_source_type_t source_type;
    App_t* ram_app;                     // Valid if source_type == WEBP_SOURCE_RAM
    const char* embedded_name;          // Valid if source_type == WEBP_SOURCE_EMBEDDED
    uint32_t duration_ms;               // 0 if unlimited (embedded loops forever)
    uint32_t frame_count;               // Number of frames (1 = static image)
} webp_player_playing_evt_t;

//------------------------------------------------------------------------------
// Event Payload: ERROR
//------------------------------------------------------------------------------

typedef struct {
    webp_source_type_t source_type;
    App_t* ram_app;
    const char* embedded_name;
    int error_code;                     // ESP_ERR_* or custom code
} webp_player_error_evt_t;

//------------------------------------------------------------------------------
// Lifecycle
//------------------------------------------------------------------------------

/**
 * Initialize the WebP player task.
 * Must be called after display_init().
 * @return ESP_OK on success
 */
esp_err_t webp_player_init(void);

/**
 * Deinitialize the WebP player task.
 * Stops playback and frees resources.
 */
void webp_player_deinit(void);

//------------------------------------------------------------------------------
// Playback Control
//------------------------------------------------------------------------------

/**
 * Play a RAM app.
 * Copies app data to internal buffer (app data may change during playback).
 *
 * @param app         Pointer to App_t (data copied, app struct must remain valid for event payload)
 * @param duration_ms Display duration in ms (0 = loop forever)
 * @param immediate   If true, interrupts current playback immediately
 * @return ESP_OK on success
 */
esp_err_t webp_player_play_app(App_t* app, uint32_t duration_ms, bool immediate);

/**
 * Play an embedded sprite from flash.
 * Uses direct pointer to flash data (no copy).
 * Embedded sprites always loop forever until stopped or replaced.
 *
 * @param name        Name of embedded sprite (e.g., "boot", "connecting", "ready")
 * @param immediate   If true, interrupts current playback immediately
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if sprite doesn't exist
 */
esp_err_t webp_player_play_embedded(const char* name, bool immediate);

/**
 * Stop playback and go idle.
 * Emits WEBP_PLAYER_EVT_STOPPED.
 *
 * @return ESP_OK on success
 */
esp_err_t webp_player_stop(void);

//------------------------------------------------------------------------------
// Status Query
//------------------------------------------------------------------------------

/**
 * Check if currently playing.
 * @return true if in PLAYING state
 */
bool webp_player_is_playing(void);

#ifdef __cplusplus
}
#endif

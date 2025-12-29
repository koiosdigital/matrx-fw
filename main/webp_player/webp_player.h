// WebP Player - Event-driven animated WebP playback task
#pragma once

#include <esp_event.h>
#include <esp_err.h>
#include <cstdint>
#include <cstddef>

// App_t is defined in apps.h - include before extern "C"
#include "apps.h"

#ifdef __cplusplus
extern "C" {
#endif

//------------------------------------------------------------------------------
// Configuration
//------------------------------------------------------------------------------

#define WEBP_PLAYER_PREPARE_NEXT_MS     5000    // When to emit PREPARE_NEXT before duration ends
#define WEBP_PLAYER_NEED_NEXT_MS        1000    // Interval for NEED_NEXT when idle with invalid content
#define WEBP_PLAYER_RETRY_COUNT         3       // Decode error retries
#define WEBP_PLAYER_RETRY_DELAY_MS      200     // Delay between retries
#define WEBP_PLAYER_TASK_STACK_SIZE     4096
#define WEBP_PLAYER_TASK_PRIORITY       10
#define WEBP_PLAYER_TASK_CORE           1
#define WEBP_PLAYER_CMD_QUEUE_SIZE      8

//------------------------------------------------------------------------------
// Event Base and IDs
//------------------------------------------------------------------------------

ESP_EVENT_DECLARE_BASE(WEBP_PLAYER_EVENTS);

typedef enum {
    // Commands (external -> player) - delivered via internal queue
    WEBP_PLAYER_CMD_PLAY,           // Play an app immediately or queue
    WEBP_PLAYER_CMD_SET_NEXT,       // Queue next app (plays after current)
    WEBP_PLAYER_CMD_STOP,           // Stop playback, clear queue
    WEBP_PLAYER_CMD_PAUSE,          // Freeze current frame
    WEBP_PLAYER_CMD_RESUME,         // Resume playback

    // Status events (player -> external) - posted to ESP event loop
    WEBP_PLAYER_EVT_PLAYING,        // Now playing an app
    WEBP_PLAYER_EVT_ERROR,          // Decode error after retries
    WEBP_PLAYER_EVT_PREPARE_NEXT,   // Request next app (~5s before end)
    WEBP_PLAYER_EVT_STOPPED,        // Player has stopped
    WEBP_PLAYER_EVT_NEED_NEXT,      // No valid content, need displayable app (sent once/sec)
} webp_player_event_id_t;

//------------------------------------------------------------------------------
// Source Type - RAM app vs Embedded
//------------------------------------------------------------------------------

typedef enum {
    WEBP_SOURCE_RAM,        // App_t* from apps manager
    WEBP_SOURCE_EMBEDDED,   // Embedded sprite from flash
} webp_source_type_t;

//------------------------------------------------------------------------------
// Event Payload: PLAYING
//------------------------------------------------------------------------------

typedef struct {
    webp_source_type_t source_type;
    App_t* ram_app;                     // Valid if source_type == WEBP_SOURCE_RAM
    const char* embedded_name;          // Valid if source_type == WEBP_SOURCE_EMBEDDED
    uint32_t expected_duration_ms;      // 0 if unlimited
    uint32_t loop_duration_ms;          // Single animation loop duration
    uint32_t frame_count;               // Number of frames (1 = static)
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
// Event Payload: PREPARE_NEXT
//------------------------------------------------------------------------------

typedef struct {
    webp_source_type_t source_type;
    App_t* ram_app;
    const char* embedded_name;
    uint32_t remaining_ms;              // Approximate time remaining
} webp_player_prepare_next_evt_t;

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
 *
 * @param app         Pointer to App_t (must remain valid during playback)
 * @param duration_ms Display duration in ms (0 = loop forever)
 * @param immediate   If true, interrupts current playback
 * @return ESP_OK on success
 */
esp_err_t webp_player_play_app(App_t* app, uint32_t duration_ms, bool immediate);

/**
 * Play an embedded sprite from flash.
 * Embedded sprites always loop forever until stopped or replaced.
 *
 * @param name        Name of embedded sprite (e.g., "boot", "connecting")
 * @param immediate   If true, interrupts current playback
 * @return ESP_OK on success
 */
esp_err_t webp_player_play_embedded(const char* name, bool immediate);

/**
 * Queue the next RAM app to play after current finishes.
 *
 * @param app         Pointer to App_t
 * @param duration_ms Display duration in ms (0 = loop forever)
 * @return ESP_OK on success
 */
esp_err_t webp_player_set_next_app(App_t* app, uint32_t duration_ms);

/**
 * Stop playback and clear display.
 * Also clears any queued next app.
 *
 * @return ESP_OK on success
 */
esp_err_t webp_player_stop(void);

/**
 * Pause playback (freeze on current frame).
 *
 * @return ESP_OK on success
 */
esp_err_t webp_player_pause(void);

/**
 * Resume paused playback.
 *
 * @return ESP_OK on success
 */
esp_err_t webp_player_resume(void);

//------------------------------------------------------------------------------
// Status Query
//------------------------------------------------------------------------------

/**
 * Check if currently playing.
 * @return true if in PLAYING state
 */
bool webp_player_is_playing(void);

/**
 * Check if paused.
 * @return true if in PAUSED state
 */
bool webp_player_is_paused(void);

/**
 * Set display mode.
 * When enabled, NEED_NEXT events are emitted when no valid RAM app is loaded.
 * When disabled (boot/OTA/disconnected), embedded sprites play without NEED_NEXT.
 *
 * @param enabled true when sockets connected, false otherwise
 */
void webp_player_set_display_mode(bool enabled);

/**
 * Request next app.
 * Call this when the current app is not displayable (no data).
 * If in display mode, emits NEED_NEXT event immediately and starts periodic emission.
 * If not in display mode, does nothing.
 */
void webp_player_request_next(void);

#ifdef __cplusplus
}
#endif

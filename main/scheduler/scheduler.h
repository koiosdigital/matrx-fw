#ifndef SCHEDULER_H
#define SCHEDULER_H

#include <stdint.h>
#include <stdlib.h>

#include "sprites.h"
#include "kd/v1/matrx.pb-c.h"

#define MAX_SCHEDULE_ITEMS 255
#define UUID_SIZE_BYTES 16

// Render request timeout in ms
#define RENDER_TIMEOUT_MS 5000

// Render state for tracking requests
enum class RenderState : uint8_t {
    NeedsRender,      // No data, needs request
    RenderPending,    // Request sent, waiting
    HasData,          // Ready to display
};

typedef struct ScheduleFlags_t {
    unsigned pinned : 1;
    unsigned skipped_user : 1;
    unsigned skipped_server : 1;
    unsigned has_received_response : 1;  // Server has responded at least once
    unsigned display_time : 6;  // 0-63 seconds
} ScheduleFlags_t;

typedef struct ScheduleItem_t {
    uint8_t uuid[UUID_SIZE_BYTES] = { 0 };
    ScheduleFlags_t flags = { 0 };
    RAMSprite_t* sprite = nullptr;
    RenderState render_state = RenderState::NeedsRender;
    TickType_t render_request_tick = 0;  // When request was sent
} ScheduleItem_t;

// Initialize the scheduler
void scheduler_init();

// Check if we have a valid schedule
bool scheduler_has_schedule();

// Set schedule from server response
void scheduler_set_schedule(Kd__V1__MatrxSchedule* schedule_response);

// Clear all schedule data
void scheduler_clear();

// Start/stop the scheduler
void scheduler_start();
void scheduler_stop();

// Called when render data is received from server
// Handles both success (data) and failure (server_skipped)
void scheduler_handle_render_response(const uint8_t* uuid, const uint8_t* data, size_t len, bool server_error);

// Button handlers
void scheduler_handle_button_prev();   // A: unpin and go to previous
void scheduler_handle_button_pin();    // B: pin current item
void scheduler_handle_button_next();   // C: unpin and go to next

#endif

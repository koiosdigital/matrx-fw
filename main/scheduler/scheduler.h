// Scheduler - Event-driven app schedule management
#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Initialize scheduler (registers event handlers)
void scheduler_init(void);

// Start/stop scheduler
void scheduler_start(void);
void scheduler_stop(void);

// Check if scheduler has a valid schedule
bool scheduler_has_schedule(void);

// Called by handlers.cpp when schedule is received
void scheduler_on_schedule_received(void);

// Called by handlers.cpp when render response arrives
void scheduler_on_render_response(const uint8_t* uuid, bool success);

// Called by sockets.cpp when connection is lost
void scheduler_on_disconnect(void);

// Get current app UUID (for debugging/logging)
const uint8_t* scheduler_get_current_uuid(void);

#ifdef __cplusplus
}
#endif

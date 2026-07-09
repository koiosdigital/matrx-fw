#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void scheduler_init(void);

void scheduler_start(void);
void scheduler_stop(void);
void scheduler_deinit(void);

// Suspend the render pipeline (stop decoding/drawing, blank the framebuffer)
// and ignore schedule/timer/player events until resumed. Used by quiet hours.
void scheduler_pause(void);
// Leave the paused state and re-derive the correct playback from the schedule.
void scheduler_resume(void);

bool scheduler_has_schedule(void);

void scheduler_on_schedule_received(void);
void scheduler_on_render_response(const uint8_t* uuid, bool success, bool displayable);
void scheduler_on_pin_state_changed(const uint8_t* uuid, bool pinned);
void scheduler_on_connect(void);
void scheduler_on_disconnect(void);

const uint8_t* scheduler_get_current_uuid(void);

void scheduler_next(void);
void scheduler_prev(void);

#ifdef __cplusplus
}
#endif

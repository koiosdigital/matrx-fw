#ifndef SCHEDULER_H
#define SCHEDULER_H

#include <stdint.h>
#include <stdlib.h>

#include "sprites.h"
#include "kd/v1/matrx.pb-c.h"

#define MAX_SCHEDULE_ITEMS 255
#define UUID_SIZE_BYTES 16

#define PREPARE_TIME 3000

typedef struct ScheduleFlags_t {
    unsigned pinned : 1;
    unsigned skipped_user : 1;
    unsigned skipped_server : 1;
    unsigned display_time : 6;  // 0-63 seconds (max 1 minute)
} ScheduleFlags_t;

typedef struct ScheduleItem_t {
    uint8_t uuid[16] = { 0 };
    ScheduleFlags_t flags = { 0 };
    RAMSprite_t* sprite = NULL;
    ScheduleItem_t* next = NULL;
} ScheduleItem_t;

void scheduler_init();
bool scheduler_has_schedule();

ScheduleItem_t* find_schedule_item(uint8_t* schedule_item_uuid);
void scheduler_set_schedule(Kd__V1__MatrxSchedule* schedule_response);

void scheduler_skip_schedule_item(uint8_t* schedule_item_uuid);
void scheduler_skip_current_schedule_item();
void scheduler_pin_schedule_item(uint8_t* schedule_item_uuid);
void scheduler_pin_current_schedule_item();

void scheduler_goto_next_item();
void scheduler_goto_previous_item();

void scheduler_clear();
void scheduler_stop();
void scheduler_start();

// Called when render data is received for a schedule item
// If the item is currently being displayed, triggers a re-display
void scheduler_notify_render_complete(const uint8_t* uuid);

// Called to update sprite data for a schedule item with proper locking
// Returns true if item was found and updated, false otherwise
bool scheduler_update_sprite_data(const uint8_t* uuid, const uint8_t* data, size_t len, bool set_server_skipped);

#endif
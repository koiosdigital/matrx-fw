#ifndef SCHEDULER_H
#define SCHEDULER_H

#include <stdint.h>
#include <stdlib.h>

#include "sprites.h"
#include "matrx.pb-c.h"

#define MAX_SCHEDULE_ITEMS 255
#define UUID_SIZE_BYTES 16

#define PREPARE_TIME 3000

typedef struct ScheduleFlags_t {
    unsigned pinned : 1;
    unsigned skipped_user : 1;
    unsigned skipped_server : 1;
    unsigned display_time : 4;
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
void scheduler_set_schedule(Matrx__ScheduleResponse* schedule_response);

void scheduler_skip_schedule_item(uint8_t* schedule_item_uuid);
void scheduler_skip_current_schedule_item();
void scheduler_pin_schedule_item(uint8_t* schedule_item_uuid);
void scheduler_pin_current_schedule_item();

void scheduler_clear();
void scheduler_stop();
void scheduler_start();

#endif
#ifndef SCHEDULER_H
#define SCHEDULER_H

#include <stdint.h>
#include <stdlib.h>

typedef struct ScheduleItem_t {
    char* schedule_item_uuid; // 32 bytes, ties to sprite by uuid
    uint32_t schedule_position; // 0-based, where it should be in list.
    uint8_t display_time; // seconds
    bool pinned;
    bool skipped;
    ScheduleItem_t* next;
} ScheduleItem_t;

void scheduler_init();
void scheduler_pause();
void scheduler_resume();

void scheduler_clear();
void scheduler_set_schedule_item(char* schedule_item_uuid, uint32_t schedule_position, uint8_t display_time, bool pinned, bool skipped);
void scheduler_skip_schedule_item(char* schedule_item_uuid);
void scheduler_pin_schedule_item(char* schedule_item_uuid);
void scheduler_set_schedule(ScheduleItem_t* head);

bool scheduler_has_schedule();

#endif
#ifndef SOCKETS_H
#define SOCKETS_H

#include <stdint.h>
#include <stdlib.h>

#define SOCKETS_URI "wss://device-api-sec.koiosdigital.net/matrx"

void sockets_init();
void sockets_disconnect();
void sockets_connect();

void request_render(uint32_t sprite_id, uint8_t* schedule_item_uuid);
void upload_coredump(uint8_t* core_dump, size_t core_dump_len);
void request_schedule();
void attempt_coredump_upload();

#endif
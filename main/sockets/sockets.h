#ifndef SOCKETS_H
#define SOCKETS_H

#include <stdint.h>
#include <stdlib.h>

#define SOCKETS_URI "wss://device.api.koiosdigital.net"

void sockets_init();
void sockets_deinit();
void sockets_disconnect();
void sockets_connect();

void request_render(const uint8_t* schedule_item_uuid);
void upload_coredump(uint8_t* core_dump, size_t core_dump_len);
void request_schedule();
void attempt_coredump_upload();

// Publish the current device configuration (protobuf-c) to the server.
void sockets_send_device_config();

// Send currently displaying update to the server.
void sockets_send_currently_displaying(uint8_t* uuid);

// Send device info to the server.
void sockets_send_device_info();

// Send modify schedule item to the server (pin/skip changes).
void sockets_send_modify_schedule_item(const uint8_t* uuid, bool pinned, bool skipped);

#endif
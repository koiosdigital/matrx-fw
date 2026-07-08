#pragma once

#include <stddef.h>

void sockets_init();
void sockets_deinit();
bool sockets_is_connected();
void sockets_flush_outbox();
void sockets_on_schedule_received();

char* sockets_get_device_token_copy();
void sockets_request_token_refresh();

#define SOCKETS_URL "wss://vn-sec.koios.sh"

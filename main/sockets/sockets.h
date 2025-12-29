#ifndef SOCKETS_H
#define SOCKETS_H

#include <stdint.h>
#include <stdlib.h>

#define SOCKETS_URI "wss://device.api.koiosdigital.net"
#define UUID_SIZE_BYTES 16

// Initialize the sockets module (call once at startup)
void sockets_init();

// Deinitialize and clean up
void sockets_deinit();

// Check if connected to server
bool sockets_is_connected();

// Request schedule from server
void request_schedule();

// Send currently displaying update to the server
void sockets_send_currently_displaying(uint8_t* uuid);

// Send device info to the server
void sockets_send_device_info();

// Send device configuration to the server
void sockets_send_device_config();

// Send modify schedule item to the server (pin/skip changes)
void sockets_send_modify_schedule_item(const uint8_t* uuid, bool pinned, bool skipped);

// Called by scheduler to send render requests
void send_render_request_to_server(const uint8_t* uuid);

// Request certificate renewal from server (sends CSR, receives new cert)
void sockets_request_cert_renewal();

#endif

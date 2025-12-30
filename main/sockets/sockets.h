#pragma once

// Initialize the sockets module
void sockets_init();

// Deinitialize and clean up
void sockets_deinit();

// Check if connected to server
bool sockets_is_connected();

#define SOCKETS_URL "wss://device.api.koiosdigital.net"
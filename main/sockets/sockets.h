#ifndef SOCKETS_H
#define SOCKETS_H

#define SOCKETS_URI "wss://device-api-sec.koiosdigital.net/matrx"

void sockets_init();
void sockets_disconnect();
void sockets_connect();

#endif
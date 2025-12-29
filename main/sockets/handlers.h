// Message handlers for incoming WebSocket messages
#pragma once

#include <kd/v1/matrx.pb-c.h>

// Process a received protobuf message
void handle_message(Kd__V1__MatrxMessage* message);

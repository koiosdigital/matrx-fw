// Certificate renewal module
// Handles checking cert expiry and requesting renewal from server
// Currently disabled - calls are commented out in sockets.cpp

#pragma once

#include <stdint.h>
#include <esp_err.h>
#include <kd/v1/matrx.pb-c.h>

#ifdef __cplusplus
extern "C" {
#endif

// Initialize cert renewal module with cert data pointer
// cert and cert_len are pointers to the socket's cached cert
void cert_renewal_init(const char** cert, size_t* cert_len);

// Check if cert needs renewal (call periodically when connected)
// Returns true if renewal was requested
bool cert_renewal_check();

// Handle cert response from server
// On success, clears cached cert and returns true (caller should reconnect)
bool cert_renewal_handle_response(Kd__V1__CertResponse* response);

// Send renewal request to server (called internally, but exposed for flexibility)
void cert_renewal_send_request();

#ifdef __cplusplus
}
#endif

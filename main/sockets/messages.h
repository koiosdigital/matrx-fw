// Outbound message helpers
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <kd/v1/matrx.pb-c.h>
#include "apps.h"

// Initialize message system with outbox queue
void msg_init(QueueHandle_t outbox);

// Queue a protobuf message for sending
bool msg_queue(const Kd__V1__MatrxMessage* message);

// Send device info
void msg_send_device_info();

// Send device config
void msg_send_device_config();

// Send claim request if device needs claiming
void msg_send_claim_if_needed();

// Upload coredump if present
void msg_upload_coredump();

// Request app render from server
void msg_request_app_render(const App_t* app);

// Notify server of currently displaying app
void msg_send_currently_displaying(const App_t* app);

// Request schedule from server
void msg_send_schedule_request();

// Send current certificate to server for expiry check
void msg_send_cert_report();

// Send certificate renewal request with CSR
void msg_send_cert_renew_request(const char* csr, size_t csr_len);
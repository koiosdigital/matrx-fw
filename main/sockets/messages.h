#pragma once

#include <stdint.h>
#include <stddef.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <kd/v1/matrx.pb-c.h>
#include "apps.h"

void msg_init(QueueHandle_t outbox);
bool msg_queue(const Kd__V1__MatrxMessage* message);
void msg_send_device_info();
void msg_send_device_config();
void msg_send_claim_if_needed();
void msg_upload_coredump();
void msg_send_currently_displaying(const App_t* app);
void msg_send_schedule_request();

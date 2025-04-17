#ifndef PROVISIONING_H
#define PROVISIONING_H

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define DEVICE_NAME_PREFIX "MATRX"

#define PKI_PROVISIONING_ENDPOINT "https://device-api.koiosdigital.net/pki/v1/provision"

void provisioning_init();
void provisioning_task_init();
void start_provisioning();
void stop_provisioning();
void reset_provisioning();

char* get_provisioning_qr_payload();
char* get_provisioning_device_name();

#endif
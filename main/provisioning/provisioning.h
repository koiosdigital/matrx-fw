#ifndef PROVISIONING_H
#define PROVISIONING_H

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define DEVICE_NAME_PREFIX "MATRX"

#define PKI_PROVISIONING_ENDPOINT "https://pki-api.koiosdigital.net/sign"

void provisioning_init();
void start_provisioning();
void stop_provisioning();
void reset_provisioning();

char* get_provisioning_qr_payload();
char* get_provisioning_device_name();

typedef enum ProvisioningTaskNotification_t {
    STOP_PROVISIONING = 1,
    START_PROVISIONING = 2,
    RESET_PROVISIONING = 3,
    RESET_SM_ON_FAILURE = 4,
    PKI_PROV_ATTEMPT_ENROLL = 5,
    DISPLAY_PROV_QR = 6,
} ProvisioningTaskNotification_t;

#endif
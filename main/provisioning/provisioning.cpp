#include "provisioning.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <stdio.h>
#include <string.h>

#include <esp_event.h>
#include <esp_log.h>
#include <esp_wifi.h>
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include <esp_random.h>
#include <esp_mac.h>

#include <qrcode.h>
#include <wifi_provisioning/manager.h>
#include <wifi_provisioning/scheme_ble.h>

#include "certmgr.h"
#include "crypto.h"
#include "display.h"

static const char* TAG = "provisioning";

char provisioning_device_name[32];
char provisioning_qr_payload[64];
char provisioning_pop_token[9];

TaskHandle_t xProvisioningTask = NULL;
uint8_t* qr_display_buffer = NULL;

void prov_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    switch (event_id) {
    case WIFI_PROV_CRED_FAIL: {
        ESP_LOGE(TAG, "provisioning error");
        if (xProvisioningTask != NULL) {
            xTaskNotify(xProvisioningTask, ProvisioningTaskNotification_t::RESET_SM_ON_FAILURE, eSetValueWithOverwrite);
        }
        break;
    }
    case WIFI_PROV_END: {
        ESP_LOGI(TAG, "provisioning end");
        wifi_prov_mgr_deinit();
        if (xProvisioningTask != NULL) {
            xTaskNotify(xProvisioningTask, ProvisioningTaskNotification_t::STOP_PROVISIONING, eSetValueWithOverwrite);
        }
        break;
    }
    case WIFI_PROV_START: {
        ESP_LOGI(TAG, "provisioning started");
        if (xProvisioningTask != NULL) {
            xTaskNotify(xProvisioningTask, ProvisioningTaskNotification_t::DISPLAY_PROV_QR, eSetValueWithOverwrite);
        }

        //This line is parsed by the programming fixture
        ESP_LOGI(TAG, "PROG::%s::%s", provisioning_device_name, provisioning_pop_token);
        break;
    }
    default:
        break;
    }
}

char* get_provisioning_device_name() {
    return provisioning_device_name;
}

char* get_provisioning_qr_payload() {
    return provisioning_qr_payload;
}

void start_provisioning() {
    if (xProvisioningTask != NULL) {
        xTaskNotify(xProvisioningTask, ProvisioningTaskNotification_t::START_PROVISIONING, eSetValueWithOverwrite);
    }
}

void stop_provisioning() {
    if (xProvisioningTask != NULL) {
        xTaskNotify(xProvisioningTask, ProvisioningTaskNotification_t::STOP_PROVISIONING, eSetValueWithOverwrite);
    }
}

void reset_provisioning() {
    if (xProvisioningTask != NULL) {
        xTaskNotify(xProvisioningTask, ProvisioningTaskNotification_t::RESET_PROVISIONING, eSetValueWithOverwrite);
    }
}

void prov_display_qr_helper(esp_qrcode_handle_t qrcode) {
    //We want to center the QR code on the display
    int qr_size = esp_qrcode_get_size(qrcode);
    int w, h = 0;
    get_display_dimensions(&w, &h);

    int x_offset = (w - qr_size) / 2;
    int y_offset = (h - qr_size) / 2;

    for (int y = 0; y < qr_size; y++) {
        for (int x = 0; x < qr_size; x++) {
            if (esp_qrcode_get_module(qrcode, x, y)) {
                qr_display_buffer[((y + y_offset) * w + (x + x_offset)) * 3] = 255;
                qr_display_buffer[((y + y_offset) * w + (x + x_offset)) * 3 + 1] = 255;
                qr_display_buffer[((y + y_offset) * w + (x + x_offset)) * 3 + 2] = 255;
            }
        }
    }

    display_raw_buffer(qr_display_buffer, get_display_buffer_size());
}

void prov_display_qr() {
    esp_qrcode_config_t qr_config = {
        .display_func = prov_display_qr_helper,
        .max_qrcode_version = 40,
        .qrcode_ecc_level = ESP_QRCODE_ECC_MED,
    };

    qr_display_buffer = (uint8_t*)heap_caps_calloc(get_display_buffer_size(), sizeof(uint8_t), MALLOC_CAP_SPIRAM);
    if (qr_display_buffer == NULL) {
        ESP_LOGE(TAG, "malloc failed: display buffer");
        return;
    }
    memset(qr_display_buffer, 0, get_display_buffer_size());

    esp_err_t err = esp_qrcode_generate(&qr_config, get_provisioning_qr_payload());
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "QR code generation failed");
    }
}

void provisioning_task(void* pvParameter) {
    ProvisioningTaskNotification_t notification;
    bool provisioning_started = false;

    while (true) {
        if (xTaskNotifyWait(0, ULONG_MAX, (uint32_t*)&notification, portMAX_DELAY) == pdTRUE) {
            switch (notification) {
            case STOP_PROVISIONING:
                if (provisioning_started) {
                    ESP_LOGI(TAG, "stopping provisioning");
                    vTaskDelay(1000);
                    wifi_prov_mgr_stop_provisioning();
                    provisioning_started = false;
                }
                break;
            case START_PROVISIONING:
                if (provisioning_started) {
                    break;
                }
                ESP_LOGI(TAG, "starting provisioner");
                wifi_prov_mgr_init({ .scheme = wifi_prov_scheme_ble, .scheme_event_handler = WIFI_PROV_SCHEME_BLE_EVENT_HANDLER_FREE_BTDM });
                wifi_prov_mgr_endpoint_create("certmgr");
                wifi_prov_mgr_start_provisioning(WIFI_PROV_SECURITY_1, provisioning_pop_token, provisioning_device_name, NULL);
                wifi_prov_mgr_endpoint_register("certmgr", certmgr_handler, NULL);
                provisioning_started = true;
                break;
            case RESET_PROVISIONING:
                esp_restart();
                break;
            case RESET_SM_ON_FAILURE:
                ESP_LOGD(TAG, "reset sm state on failure");
                wifi_prov_mgr_reset_sm_state_on_failure();
                break;
            case DISPLAY_PROV_QR:
                prov_display_qr();
                break;
            default:
                break;
            }
        }
    }
}

void provisioning_init() {
    uint8_t mac[6];
    esp_efuse_mac_get_default(mac);

    char macStr[13];
    snprintf(macStr, 13, "%02X%02X%02X%02X%02X%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    snprintf(provisioning_device_name, 32, "%s-%s", DEVICE_NAME_PREFIX, macStr);

    esp_fill_random(provisioning_pop_token, 8);
    for (int i = 0; i < 8; i++) {
        provisioning_pop_token[i] = (provisioning_pop_token[i] % 26) + 'A';
    }
    provisioning_pop_token[8] = '\0';

    snprintf(provisioning_qr_payload, 64, "%s;%s", provisioning_device_name, provisioning_pop_token);

    esp_event_handler_register(WIFI_PROV_EVENT, ESP_EVENT_ANY_ID, &prov_event_handler, NULL);

    xTaskCreatePinnedToCore(provisioning_task, "provisioning", 4096, NULL, 2, &xProvisioningTask, 1);
}
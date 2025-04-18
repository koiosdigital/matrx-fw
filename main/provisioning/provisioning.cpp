#include "provisioning.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <esp_event.h>
#include <esp_log.h>
#include <esp_wifi.h>
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include <esp_random.h>
#include <esp_mac.h>
#include <wifi_provisioning/manager.h>
#include <wifi_provisioning/scheme_ble.h>

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "qrcode.h"

#include "crypto.h"
#include "display.h"
#include "matrx.pb-c.h"

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

esp_err_t custom_prov_config_data_handler(uint32_t session_id, const uint8_t* inbuf, ssize_t inlen, uint8_t** outbuf, ssize_t* outlen, void* priv_data) {
    if (inbuf) {
        ESP_LOGI(TAG, "Received data: %.*s", inlen, (char*)inbuf);
        Matrx__CertMgrMessage* cert_mgr_message = matrx__cert_mgr_message__unpack(NULL, inlen, (uint8_t*)inbuf);
        if (cert_mgr_message == NULL) {
            ESP_LOGE(TAG, "failed to unpack cert mgr message");
            matrx__cert_mgr_message__free_unpacked(cert_mgr_message, NULL);
            return ESP_FAIL;
        }

        Matrx__CertMgrMessage* response = nullptr;

        if (cert_mgr_message->message_case == Matrx__CertMgrMessage__MessageCase::MATRX__CERT_MGR_MESSAGE__MESSAGE_PROVISIONING_STATUS_REQUEST) {
            ESP_LOGI(TAG, "Received provisioning status request");

            bool has_csr = crypto_get_csr(NULL, NULL) == ESP_OK;
            bool has_cert = crypto_get_device_cert(NULL, NULL) == ESP_OK;

            ESP_LOGI(TAG, "has_csr: %d, has_cert: %d", has_csr, has_cert);

            // Send provisioning status response
            Matrx__ProvisioningStatusResponse* status_response = nullptr;
            matrx__provisioning_status_response__init(status_response);

            status_response->has_cert = has_cert;
            status_response->has_csr = has_csr;

            matrx__cert_mgr_message__init(response);
            response->message_case = Matrx__CertMgrMessage__MessageCase::MATRX__CERT_MGR_MESSAGE__MESSAGE_PROVISIONING_STATUS_RESPONSE;
            response->provisioning_status_response = status_response;

            //pack
            size_t response_len = matrx__cert_mgr_message__get_packed_size(response);
            *outlen = response_len;
            *outbuf = (uint8_t*)malloc(response_len);
            if (*outbuf == NULL) {
                ESP_LOGE(TAG, "System out of memory");
                return ESP_ERR_NO_MEM;
            }
            matrx__cert_mgr_message__pack(response, *outbuf);

            //free intermediate data
            matrx__provisioning_status_request__free_unpacked(cert_mgr_message->provisioning_status_request, NULL);
            matrx__cert_mgr_message__free_unpacked(cert_mgr_message, NULL);
            matrx__provisioning_status_response__free_unpacked(status_response, NULL);
            matrx__cert_mgr_message__free_unpacked(response, NULL);
        }
        else if (cert_mgr_message->message_case == Matrx__CertMgrMessage__MessageCase::MATRX__CERT_MGR_MESSAGE__MESSAGE_CSR_REQUEST) {
            ESP_LOGI(TAG, "Received CSR request");

            // Send provisioning status response
            Matrx__CSRResponse* csr_response = nullptr;
            matrx__csrresponse__init(csr_response);

            char* csr = (char*)heap_caps_calloc(4096, sizeof(char), MALLOC_CAP_SPIRAM);
            size_t len = 4096;

            crypto_get_csr(csr, &len);

            csr_response->csr_size = len;
            csr_response->csr.data = (uint8_t*)malloc(len);
            if (csr_response->csr.data == NULL) {
                ESP_LOGE(TAG, "System out of memory");
                return ESP_ERR_NO_MEM;
            }
            memcpy(csr_response->csr.data, csr, len);
            csr_response->csr.len = len;
            free(csr);

            matrx__cert_mgr_message__init(response);
            response->message_case = Matrx__CertMgrMessage__MessageCase::MATRX__CERT_MGR_MESSAGE__MESSAGE_CSR_RESPONSE;
            response->csr_response = csr_response;

            //pack
            size_t response_len = matrx__cert_mgr_message__get_packed_size(response);
            *outlen = response_len;
            *outbuf = (uint8_t*)malloc(response_len);
            if (*outbuf == NULL) {
                ESP_LOGE(TAG, "System out of memory");
                return ESP_ERR_NO_MEM;
            }
            matrx__cert_mgr_message__pack(response, *outbuf);

            //free intermediate data
            matrx__csrrequest__free_unpacked(cert_mgr_message->csr_request, NULL);
            matrx__cert_mgr_message__free_unpacked(cert_mgr_message, NULL);
            matrx__csrresponse__free_unpacked(csr_response, NULL);
            matrx__cert_mgr_message__free_unpacked(response, NULL);
        }
        else if (cert_mgr_message->message_case == Matrx__CertMgrMessage__MessageCase::MATRX__CERT_MGR_MESSAGE__MESSAGE_SET_CERT_REQUEST) {
            ESP_LOGI(TAG, "Received set cert request");

            // Send provisioning status response
            Matrx__SetCertResponse* set_cert_response = nullptr;
            matrx__set_cert_response__init(set_cert_response);
            set_cert_response->success = true;

            esp_err_t ok = crypto_set_device_cert((char*)(cert_mgr_message->set_cert_request->cert.data), cert_mgr_message->set_cert_request->cert.len);
            if (ok != ESP_OK) {
                ESP_LOGE(TAG, "Failed to set device cert");
                set_cert_response->success = false;
            }

            matrx__cert_mgr_message__init(response);
            response->message_case = Matrx__CertMgrMessage__MessageCase::MATRX__CERT_MGR_MESSAGE__MESSAGE_SET_CERT_RESPONSE;
            response->set_cert_response = set_cert_response;

            //pack
            size_t response_len = matrx__cert_mgr_message__get_packed_size(response);
            *outlen = response_len;
            *outbuf = (uint8_t*)malloc(response_len);
            if (*outbuf == NULL) {
                ESP_LOGE(TAG, "System out of memory");
                return ESP_ERR_NO_MEM;
            }
            matrx__cert_mgr_message__pack(response, *outbuf);

            //free intermediate data
            matrx__set_cert_request__free_unpacked(cert_mgr_message->set_cert_request, NULL);
            matrx__cert_mgr_message__free_unpacked(cert_mgr_message, NULL);
            matrx__set_cert_response__free_unpacked(set_cert_response, NULL);
            matrx__cert_mgr_message__free_unpacked(response, NULL);
        }

        return ESP_OK;
    }

    ESP_LOGE(TAG, "Received empty data");
    return ESP_FAIL;
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
                    wifi_prov_mgr_stop_provisioning();
                    provisioning_started = false;
                    vTaskDelete(NULL);
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
                wifi_prov_mgr_endpoint_register("certmgr", custom_prov_config_data_handler, NULL);
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
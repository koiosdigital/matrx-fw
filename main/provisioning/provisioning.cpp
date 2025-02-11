#include "provisioning.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <esp_event.h>
#include <esp_log.h>
#include <esp_wifi.h>
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include <esp_random.h>
#include <wifi_provisioning/manager.h>
#include <wifi_provisioning/scheme_ble.h>

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "qrcode.h"

#include "crypto.h"
#include "display.h"

static const char* TAG = "provisioning";

char provisioning_device_name[32];
char provisioning_qr_payload[64];
char provisioning_pop_token[17];

TaskHandle_t xProvisioningTask = nullptr;

typedef enum ProvisioningTaskNotification_t {
    STOP_PROVISIONING = 1,
    START_PROVISIONING = 2,
    RESET_PROVISIONING = 3,
    RESET_SM_ON_FAILURE = 4,
    PKI_PROV_ATTEMPT_ENROLL = 5,
    DISPLAY_PROV_QR = 6,
} ProvisioningTaskNotification_t;

void prov_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_base == WIFI_PROV_EVENT) {
        switch (event_id) {
        case WIFI_PROV_CRED_FAIL: {
            ESP_LOGE(TAG, "provisioning error");

            if (xProvisioningTask != NULL) {
                xTaskNotify(xProvisioningTask, ProvisioningTaskNotification_t::RESET_SM_ON_FAILURE, eSetValueWithOverwrite);
            }

            break;
        }
        case WIFI_PROV_CRED_SUCCESS: {
            ESP_LOGI(TAG, "provisioning successful");
            break;
        }
        case WIFI_PROV_END: {
            ESP_LOGI(TAG, "provisioning end");
            wifi_prov_mgr_deinit();
            break;
        }
        case WIFI_PROV_START: {
            ESP_LOGI(TAG, "provisioning started");
            if (xProvisioningTask != NULL) {
                xTaskNotify(xProvisioningTask, ProvisioningTaskNotification_t::DISPLAY_PROV_QR, eSetValueWithOverwrite);
            }
            break;
        }
        case WIFI_PROV_CRED_RECV: {
            ESP_LOGI(TAG, "credentials received, attempting connection");
            break;
        }
        case WIFI_PROV_DEINIT: {
            ESP_LOGI(TAG, "provisioning deinit");
            break;
        }
        }
    }
}

void prov_wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_base == IP_EVENT) {
        switch (event_id) {
        case IP_EVENT_STA_GOT_IP: {
            if (crypto_get_device_cert(NULL, NULL) != ESP_OK) {
                ESP_LOGI(TAG, "device not enrolled in PKI, attempting to provision");
                if (xProvisioningTask != NULL) {
                    xTaskNotify(xProvisioningTask, ProvisioningTaskNotification_t::PKI_PROV_ATTEMPT_ENROLL, eSetValueWithOverwrite);
                }
            }
            break;
        }
        }
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

void pki_prov_start_enroll() {
    ESP_LOGI(TAG, "attempting to enroll in PKI");

    esp_http_client_config_t config = {
        .url = PKI_PROVISIONING_ENDPOINT,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_err_t err;
    cJSON* root = nullptr;
    char* post_data = nullptr;
    esp_http_client_handle_t http_client = esp_http_client_init(&config);

    char* csr_buffer = (char*)heap_caps_malloc(4096, MALLOC_CAP_SPIRAM);
    memset(csr_buffer, 0, 4096);

    size_t csr_len = 0;
    crypto_get_csr(csr_buffer, &csr_len);

    if (csr_len == 0) {
        ESP_LOGE(TAG, "CSR not found");
        goto exit;
    }

    //PKI provisioner allows all requests from factory flash network
    //Users attempting to flash on their own networks go through a seperate app setup
    //flow that tells the PKI provisioner to expect a different network after payment
    //Servers aren't free you know

    esp_http_client_set_method(http_client, HTTP_METHOD_POST);
    esp_http_client_set_header(http_client, "Content-Type", "application/json");
    esp_http_client_set_header(http_client, "X-Device-Name", get_provisioning_device_name());

    root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "csr", csr_buffer);

    post_data = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    esp_http_client_set_post_field(http_client, post_data, strlen(post_data));

    err = esp_http_client_perform(http_client);
    if (err == ESP_OK) {
        int status_code = esp_http_client_get_status_code(http_client);
        if (status_code == 200) {
            char* response = (char*)heap_caps_malloc(4096, MALLOC_CAP_SPIRAM);
            memset(response, 0, 4096);

            int response_len = esp_http_client_read(http_client, response, 4096);
            if (response_len > 0) {
                cJSON* root = cJSON_Parse(response);
                cJSON* cert = cJSON_GetObjectItem(root, "cert");

                if (cert != NULL) {
                    char* cert_buffer = cert->valuestring;

                    ESP_LOGI(TAG, "PKI enrollment OK: cert len %d", strlen(cert_buffer));
                    crypto_set_device_cert(cert_buffer, strlen(cert_buffer));
                    crypto_clear_csr();

                    free(cert_buffer);
                }

                cJSON_Delete(root);
            }

            free(response);
        }
    }
    else {
        ESP_LOGE(TAG, "HTTP POST request failed: %s", esp_err_to_name(err));
        ESP_LOGE(TAG, "status code: %d", esp_http_client_get_status_code(http_client));

        goto exit;
    }

exit:
    esp_http_client_cleanup(http_client);
    if (post_data != NULL) {
        free(post_data);
    }
    if (csr_buffer != NULL) {
        free(csr_buffer);
    }
}

void prov_display_qr_helper(esp_qrcode_handle_t qrcode) {
    uint8_t* display_buffer = (uint8_t*)heap_caps_malloc(get_display_buffer_size(), MALLOC_CAP_SPIRAM);

    if (display_buffer == NULL) {
        ESP_LOGE(TAG, "malloc failed: display buffer");
        return;
    }

    memset(display_buffer, 0, get_display_buffer_size());

    //We want to center the QR code on the display
    int qr_size = esp_qrcode_get_size(qrcode);
    int w, h = 0;
    get_display_dimensions(&w, &h);

    int x_offset = (w - qr_size) / 2;
    int y_offset = (h - qr_size) / 2;

    for (int y = 0; y < qr_size; y++) {
        for (int x = 0; x < qr_size; x++) {
            if (!esp_qrcode_get_module(qrcode, x, y)) {
                display_buffer[((y + y_offset) * w + (x + x_offset)) * 3] = 255;
                display_buffer[((y + y_offset) * w + (x + x_offset)) * 3 + 1] = 255;
                display_buffer[((y + y_offset) * w + (x + x_offset)) * 3 + 2] = 255;
            }
        }
    }

    display_raw_buffer(display_buffer, get_display_buffer_size());
}

void prov_display_qr() {
    esp_qrcode_config_t qr_config = {
        .display_func = prov_display_qr_helper,
        .max_qrcode_version = 10,
        .qrcode_ecc_level = ESP_QRCODE_ECC_LOW,
    };

    esp_err_t err = esp_qrcode_generate(&qr_config, get_provisioning_qr_payload());
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "QR code generation failed");
    }
}

void provisioning_task(void* pvParameter) {
    ESP_LOGD(TAG, "task started");

    ProvisioningTaskNotification_t notification;

    while (true) {
        if (xTaskNotifyWait(0, ULONG_MAX, (uint32_t*)&notification, portMAX_DELAY) == pdTRUE) {
            wifi_prov_mgr_config_t config;

            switch (notification) {
            case STOP_PROVISIONING:
                ESP_LOGI(TAG, "stopping provisioner");
                wifi_prov_mgr_stop_provisioning();

                break;
            case START_PROVISIONING:
                ESP_LOGI(TAG, "starting provisioner");

                config = { .scheme = wifi_prov_scheme_ble, .scheme_event_handler = WIFI_PROV_SCHEME_BLE_EVENT_HANDLER_FREE_BT };

                wifi_prov_mgr_init(config);

                wifi_prov_mgr_start_provisioning(WIFI_PROV_SECURITY_1, provisioning_pop_token, provisioning_device_name, NULL);

                break;
            case RESET_PROVISIONING:
                ESP_LOGI(TAG, "reset provisioner");
                ESP_ERROR_CHECK(wifi_prov_mgr_reset_provisioning());
                esp_restart();
                break;
            case RESET_SM_ON_FAILURE:
                ESP_LOGD(TAG, "reset sm state on failure");
                wifi_prov_mgr_reset_sm_state_on_failure();
                break;
            case PKI_PROV_ATTEMPT_ENROLL:
                pki_prov_start_enroll();
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
    //fill provisioning device name
    uint8_t mac[6];
    esp_wifi_get_mac(WIFI_IF_STA, mac);

    char macStr[13];
    snprintf(macStr, 13, "%02X%02X%02X%02X%02X%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    snprintf(provisioning_device_name, 32, "%s-%s", DEVICE_NAME_PREFIX, macStr);

    //generate pop token
    esp_fill_random(provisioning_pop_token, 16);
    for (int i = 0; i < 16; i++) {
        provisioning_pop_token[i] = (provisioning_pop_token[i] % 26) + 'A';
    }
    provisioning_pop_token[16] = '\0';

    //generate QR payload
    snprintf(provisioning_qr_payload, 64, "%s;%s", provisioning_device_name, provisioning_pop_token);

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_PROV_EVENT, ESP_EVENT_ANY_ID, &prov_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, &prov_wifi_event_handler, NULL));

    xTaskCreatePinnedToCore(provisioning_task, "provisioning", 2048, NULL, 5, &xProvisioningTask, 1);
}

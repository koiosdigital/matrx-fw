#include "wifi.h"

#include <esp_err.h>
#include <esp_event.h>
#include <esp_log.h>
#include <esp_wifi.h>
#include <esp_http_client.h>
#include <esp_crt_bundle.h>
#include "cJSON.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <string.h>

#include "provisioning.h"
#include "sprites.h"
#include "sockets.h"
#include "crypto.h"

static const char* TAG = "wifi";

SemaphoreHandle_t cert_mutex = NULL;

int data_offset = 0;
static esp_err_t _http_event_handle(esp_http_client_event_t* evt)
{
    char* buf = (char*)(evt->user_data);

    switch (evt->event_id) {
    case HTTP_EVENT_ON_DATA:
        if (!esp_http_client_is_chunked_response(evt->client)) {
            memcpy(buf + data_offset, evt->data, evt->data_len);
            data_offset += evt->data_len;
        }
        break;
    case HTTP_EVENT_ON_FINISH:
        ESP_LOGI(TAG, "HTTP_EVENT_ON_FINISH");
        data_offset = 0;
        break;
    case HTTP_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "HTTP_EVENT_DISCONNECTED");
        break;
    default:
        break;
    }
    return ESP_OK;
}

void fetch_device_cert(void* pvParameter) {
    if (xSemaphoreTake(cert_mutex, portMAX_DELAY) != pdTRUE) {
        ESP_LOGE(TAG, "failed to take cert mutex");
        return;
    }

    ESP_LOGI(TAG, "attempting to enroll in PKI");

    char* response = (char*)heap_caps_calloc(2048, sizeof(char), MALLOC_CAP_SPIRAM);

    esp_http_client_config_t config = {
        .url = "https://example.com",
        .event_handler = _http_event_handle,
        .user_data = response,
        .crt_bundle_attach = esp_crt_bundle_attach
    };

    cJSON* root = NULL;
    char* post_data = NULL;
    char* cert_buffer = NULL;

    int status_code = 0;

    esp_err_t err = ESP_OK;
    esp_http_client_handle_t http_client = esp_http_client_init(&config);

    /*
    char* csr_buffer = (char*)heap_caps_calloc(4096, sizeof(char), MALLOC_CAP_SPIRAM);

    size_t csr_len = 4096;
    crypto_get_csr(csr_buffer, &csr_len);

    if (csr_len == 0) {
        ESP_LOGE(TAG, "CSR not found");
        goto exit;
    }

    esp_http_client_set_method(http_client, HTTP_METHOD_POST);
    esp_http_client_set_header(http_client, "Content-Type", "application/json");
    esp_http_client_set_header(http_client, "X-Device-Name", get_provisioning_device_name());

    root = cJSON_CreateObject();
    if (root == NULL) {
        ESP_LOGE(TAG, "cJSON_CreateObject failed");
        goto exit;
    }

    if (cJSON_AddStringToObject(root, "csr", csr_buffer) == NULL) {
        ESP_LOGE(TAG, "cJSON_AddStringToObject failed");
        goto exit;
    }

    post_data = cJSON_PrintUnformatted(root);

    if (post_data == NULL) {
        ESP_LOGE(TAG, "cJSON_PrintUnformatted failed");
        goto exit;
    }

    err = esp_http_client_set_post_field(http_client, post_data, strlen(post_data));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_http_client_set_post_field failed");
        goto exit;
    }

    */

    ESP_LOGI(TAG, "watermark: %d", uxTaskGetStackHighWaterMark(NULL));

    err = esp_http_client_perform(http_client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP POST request failed: %s", esp_err_to_name(err));
        ESP_LOGE(TAG, "status code: %d", esp_http_client_get_status_code(http_client));

        goto exit;
    }

    status_code = esp_http_client_get_status_code(http_client);
    if (status_code == 200) {
        ESP_LOGI(TAG, "response length: %d", strlen(response));

        cJSON* root = cJSON_Parse(response);
        if (root == NULL) {
            ESP_LOGE(TAG, "failed to parse JSON");
            goto exit;
        }

        cJSON* cert = cJSON_GetObjectItem(root, "cert");
        if (cert == NULL) {
            ESP_LOGE(TAG, "failed to get cert");
            goto exit;
        }

        cert_buffer = cert->valuestring;
        crypto_set_device_cert(cert_buffer, strlen(cert_buffer));
        crypto_clear_csr();

        ESP_LOGI(TAG, "device enrolled in PKI");
    }
    else {
        ESP_LOGE(TAG, "failed to enroll in PKI, status code: %d", status_code);
    }

exit:
    esp_http_client_cleanup(http_client);
    free(response);
    //free(post_data);
    //cJSON_Delete(root);
    //free(csr_buffer);
    xSemaphoreGive(cert_mutex);
}

void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    static int wifiConnectionAttempts = 0;
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
        case WIFI_EVENT_STA_START: {
            wifi_config_t wifi_cfg;
            esp_wifi_get_config(WIFI_IF_STA, &wifi_cfg);

            if (!strlen((const char*)wifi_cfg.sta.ssid)) {
                provisioning_task_init();
                vTaskDelay(pdMS_TO_TICKS(500));
                start_provisioning();
                break;
            }

            esp_wifi_connect();
            //show_fs_sprite("/fs/connect_wifi.webp");
            break;
        }
        case WIFI_EVENT_STA_DISCONNECTED: {
            wifiConnectionAttempts++;
            sockets_disconnect();

            if (wifiConnectionAttempts > 5) {
                provisioning_task_init();
                vTaskDelay(pdMS_TO_TICKS(500));
                start_provisioning();
            }

            esp_wifi_connect();
            break;
        }
        }
    }
    else if (event_base == IP_EVENT) {
        if (event_id == IP_EVENT_STA_GOT_IP) {
            ESP_LOGI(TAG, "STA got IP");
            wifiConnectionAttempts = 0;
            stop_provisioning();

            if (crypto_get_state() == CRYPTO_STATE_VALID_CSR) {
                cert_mutex = xSemaphoreCreateBinary();
                xSemaphoreGive(cert_mutex);

                xTaskCreate(fetch_device_cert, "fetch_device_cert", 8192, NULL, 5, NULL);
                vTaskDelay(pdMS_TO_TICKS(1000));

                while (xSemaphoreTake(cert_mutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
                    ESP_LOGI(TAG, "waiting for cert mutex");
                }
            }

            sockets_connect();
        }
    }
}

void wifi_disconnect() {
    esp_wifi_disconnect();
}

void wifi_clear_credentials() {
    wifi_config_t wifi_cfg;
    memset(&wifi_cfg, 0, sizeof(wifi_cfg));
    esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);
    esp_restart();
}

void wifi_init() {
    esp_netif_init();

    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL);

    esp_netif_t* netif = esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);
    esp_wifi_set_mode(wifi_mode_t::WIFI_MODE_STA);

    esp_netif_set_hostname(netif, get_provisioning_device_name());

    esp_wifi_start();
}
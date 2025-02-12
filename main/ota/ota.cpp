#include "ota.h"

#include "esp_https_ota.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_app_format.h"
#include "esp_wifi.h"

#include "semver.h"
#include "cJSON.h"

static const char* TAG = "ota";

esp_http_client_config_t config = {
    .url = OTA_MANIFEST_URL,
    .crt_bundle_attach = esp_crt_bundle_attach,
};

esp_http_client_handle_t http_client;
char* response = nullptr;
int status_code = 0;
int response_len = 0;
esp_err_t err;
char binURL[256];

cJSON* root, * latest, * bin, * host = NULL;
semver_t current_version, latest_version;

void do_ota(void* pvParameter) {
    char* binURL = (char*)pvParameter;

    if (binURL == NULL) {
        ESP_LOGE(TAG, "binURL is null");
        return;
    }

    esp_http_client_config_t config = {
        .url = binURL,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_https_ota_config_t ota_config = {
        .http_config = &config,
    };

    esp_err_t err = esp_https_ota(&ota_config);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA failed: %s", esp_err_to_name(err));
    }
    else {
        ESP_LOGI(TAG, "OTA successful");
        esp_restart();
    }
}

void ota_timer_handler(void* pvParameter) {
    vTaskDelay(pdMS_TO_TICKS(1000 * 60 * 5)); // wait 5 minutes before starting OTA

    while (1) {
        http_client = esp_http_client_init(&config);

        if (err != ESP_OK) {
            ESP_LOGE(TAG, "failed to get partition description");
            goto exit;
        }

        semver_parse_version(esp_app_get_description()->version, &current_version);

        err = esp_http_client_perform(http_client);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "HTTP GET request failed: %s", esp_err_to_name(err));
            goto exit;
        }

        status_code = esp_http_client_get_status_code(http_client);
        if (status_code != 200) {
            ESP_LOGE(TAG, "HTTP GET request failed: %d", status_code);
            goto exit;
        }

        response = (char*)heap_caps_malloc(512, MALLOC_CAP_SPIRAM);
        if (response == NULL) {
            ESP_LOGE(TAG, "malloc failed: response");
            goto exit;
        }
        memset(response, 0, 512);

        response_len = esp_http_client_read(http_client, response, 512);
        root = cJSON_Parse(response);
        if (root == NULL) {
            ESP_LOGE(TAG, "failed to parse JSON");
            goto exit;
        }

        latest = cJSON_GetObjectItem(root, "version");
        semver_parse_version(latest->valuestring, &latest_version);

        if (semver_compare(latest_version, current_version) != 1) {
            ESP_LOGI(TAG, "no update available");
            goto exit;
        }

        bin = cJSON_GetObjectItem(root, "bin");
        host = cJSON_GetObjectItem(root, "host");

        snprintf(binURL, 256, "https://%s%s", host->valuestring, bin->valuestring);

        xTaskCreate(do_ota, "ota_task", 4096, &binURL, 5, NULL);

    exit:
        esp_http_client_cleanup(http_client);
        if (response != NULL) {
            free(response);
        }

        vTaskDelay(pdMS_TO_TICKS(1000 * 60 * 60 * 6));
    }
}

void ota_init() {
    xTaskCreate(ota_timer_handler, "ota_timer", 1024, NULL, 5, NULL);
}
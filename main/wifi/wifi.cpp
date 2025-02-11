#include "wifi.h"

#include <esp_err.h>
#include <esp_event.h>
#include <esp_log.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <string.h>

#include "provisioning.h"

static const char* TAG = "wifi";

void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    static int wifiConnectionAttempts = 0;
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
        case WIFI_EVENT_STA_START: {
            ESP_LOGI(TAG, "STA started");

            /* check if device has been provisioned */
            wifi_config_t wifi_cfg;
            esp_wifi_get_config(WIFI_IF_STA, &wifi_cfg);
            if (strlen((const char*)wifi_cfg.sta.ssid)) {
                esp_wifi_connect();
            }
            else {
                start_provisioning();
            }
            break;
        }
        case WIFI_EVENT_STA_DISCONNECTED: {
            wifiConnectionAttempts++;
            ESP_LOGI(TAG, "STA disconnected");

            //TODO: disconnect from wss

            if (wifiConnectionAttempts > 5) {
                start_provisioning();
            }
            ESP_LOGI(TAG, "STA reconnecting..");
            esp_wifi_connect();
            break;
        }
        }
    }
    else if (event_base == IP_EVENT) {
        if (event_id == IP_EVENT_STA_GOT_IP) {
            wifiConnectionAttempts = 0;
            ESP_LOGI(TAG, "STA connected!");

            stop_provisioning();

            //TODO: reconnect to wss
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
    ESP_ERROR_CHECK(esp_netif_init());

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

    esp_netif_t* netif = esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(wifi_mode_t::WIFI_MODE_STA));

    esp_netif_set_hostname(netif, get_provisioning_device_name());

    ESP_ERROR_CHECK(esp_wifi_start());
}
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

void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    static int wifiConnectionAttempts = 0;
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
        case WIFI_EVENT_STA_START: {
            wifi_config_t wifi_cfg;
            esp_wifi_get_config(WIFI_IF_STA, &wifi_cfg);

            if (!strlen((const char*)wifi_cfg.sta.ssid)) {
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

    //clear credentials

    esp_wifi_init(&cfg);

    esp_wifi_set_mode(wifi_mode_t::WIFI_MODE_STA);

    esp_netif_set_hostname(netif, get_provisioning_device_name());

    esp_wifi_start();
}
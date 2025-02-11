#include "sockets.h"

#include "esp_log.h"
#include "esp_websocket_client.h"
#include "esp_event.h"
#include "esp_crt_bundle.h"

#include "crypto.h"
#include "display.h"

static const char* TAG = "sockets";
TaskHandle_t xSocketsTask = nullptr;

QueueHandle_t xSocketsQueue = nullptr;

typedef struct ProcessableMessage_t {
    char* message;
    size_t message_len;
    bool is_outbox;
} ProcessableMessage_t;

static void websocket_event_handler(void* handler_args, esp_event_base_t base, int32_t event_id, void* event_data)
{
    esp_websocket_event_data_t* data = (esp_websocket_event_data_t*)event_data;
    static char* dbuf = nullptr;

    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "connected");
        display_clear_status_bar();
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "disconnected");
        display_set_status_bar(255, 0, 0);
        break;
    case WEBSOCKET_EVENT_DATA:
        ESP_LOGI(TAG, "WEBSOCKET_EVENT_DATA");

        if (data->payload_offset == 0) {
            if (dbuf) {
                free(dbuf);
            }
            dbuf = (char*)malloc(data->payload_len + 1);
            if (dbuf == NULL) {
                ESP_LOGE(TAG, "malloc failed: dbuf (%d)", data->payload_len);
                return;
            }
        }

        if (dbuf) {
            memcpy(dbuf + data->payload_offset, data->data_ptr, data->data_len);
        }

        if (data->payload_len + data->payload_offset >= data->data_len) {
            ESP_LOGD(TAG, "total payload length=%d, data_len=%d, current payload offset=%d\r\n", data->payload_len, data->data_len, data->payload_offset);

            ProcessableMessage_t message;
            message.message = dbuf;
            message.message_len = data->payload_len;
            message.is_outbox = false;

            if (xQueueSend(xSocketsQueue, &message, pdMS_TO_TICKS(50)) != pdTRUE) {
                ESP_LOGE(TAG, "failed to send message to queue");
            }
        }

        break;
    }
}

void sockets_task(void* pvParameter)
{
    while (1) {
        if (crypto_get_state() == CryptoState_t::CRYPTO_STATE_VALID_CERT) {
            ESP_LOGI(TAG, "device provisioned");
        }
        vTaskDelay(pdMS_TO_TICKS(5000));
    }

    esp_ds_data_ctx_t* ds_data_ctx = crypto_get_ds_data_ctx();
    if (ds_data_ctx == NULL) {
        ESP_LOGE(TAG, "ds data ctx is NULL");
        return;
    }

    char* cert = (char*)heap_caps_malloc(2048, MALLOC_CAP_SPIRAM);
    size_t cert_len;

    esp_err_t err = crypto_get_device_cert(cert, &cert_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to get device cert");
        return;
    }

    esp_websocket_client_config_t websocket_cfg = {
        .uri = SOCKETS_URI,
        .port = 443,
        .client_cert = cert,
        .client_cert_len = cert_len,
        .client_ds_data = ds_data_ctx->esp_ds_data,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_websocket_client_handle_t client = esp_websocket_client_init(&websocket_cfg);
    esp_websocket_register_events(client, WEBSOCKET_EVENT_ANY, websocket_event_handler, (void*)client);
    esp_websocket_client_start(client);

    ProcessableMessage_t message;

    while (1)
    {
        if (xQueueReceive(xSocketsQueue, &message, portMAX_DELAY) == pdTRUE) {
            if (message.message == NULL) {
                continue;
            }

            if (message.is_outbox) {
                ESP_LOGI(TAG, "sending message with length %d", message.message_len);
                esp_websocket_client_send_text(client, message.message, message.message_len, pdMS_TO_TICKS(1000));
            }
            else {
                ESP_LOGI(TAG, "received message with length %d", message.message_len);
                for (int i = 0; i < message.message_len; i++) {
                    ESP_LOGI(TAG, "%c", message.message[i]);
                }
            }
        }
    }
}

void sockets_init()
{
    xSocketsQueue = xQueueCreate(10, sizeof(ProcessableMessage_t));
    xTaskCreatePinnedToCore(sockets_task, "sockets", 2048, NULL, 5, &xSocketsTask, 1);
}
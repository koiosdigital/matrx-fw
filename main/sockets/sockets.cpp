#include "sockets.h"

#include "esp_log.h"
#include "esp_websocket_client.h"
#include "esp_event.h"
#include "esp_crt_bundle.h"

#include "crypto.h"
#include "display.h"
#include "scheduler.h"
#include "sprites.h"

#include "matrx.pb-c.h"
#include <esp_partition.h>
#include <mbedtls/base64.h>

static const char* TAG = "sockets";
TaskHandle_t xSocketsTask = nullptr;

QueueHandle_t xSocketsQueue = nullptr;
esp_websocket_client_handle_t client = nullptr;

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
        if (!scheduler_has_schedule()) {
            show_fs_sprite("/fs/ready.webp");
            request_schedule();
        }
        attempt_coredump_upload();
        scheduler_resume();
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "disconnected");
        scheduler_pause();
        show_fs_sprite("/fs/connect_cloud.webp");
        break;
    case WEBSOCKET_EVENT_DATA:
        ESP_LOGI(TAG, "WEBSOCKET_EVENT_DATA");

        if (data->payload_offset == 0) {
            free(dbuf);
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

void handle_schedule_response(Matrx__ScheduleResponse* response)
{
    if (response->n_schedule_items == 0) {
        ESP_LOGI(TAG, "no schedule items");
        return;
    }
}

void handle_render_response(Matrx__RenderResponse* response)
{
    ESP_LOGI(TAG, "received render response");
}

void handle_pin_schedule_item(Matrx__PinScheduleItem* pin)
{
    ESP_LOGI(TAG, "received pin schedule item");
}

void handle_skip_schedule_item(Matrx__SkipScheduleItem* skip)
{
    ESP_LOGI(TAG, "received skip schedule item");
}

void handle_message(Matrx__SocketMessage* message)
{
    switch (message->message_case) {
    case MATRX__SOCKET_MESSAGE__MESSAGE_SCHEDULE_RESPONSE:
        ESP_LOGI(TAG, "received schedule response message");
        handle_schedule_response(message->schedule_response);
        break;
    case MATRX__SOCKET_MESSAGE__MESSAGE_RENDER_RESPONSE:
        ESP_LOGI(TAG, "received render response message");
        handle_render_response(message->render_response);
        break;
    case MATRX__SOCKET_MESSAGE__MESSAGE_RESTART:
        ESP_LOGI(TAG, "received restart message");
        esp_restart();
        break;
    case MATRX__SOCKET_MESSAGE__MESSAGE_PIN_SCHEDULE_ITEM:
        ESP_LOGI(TAG, "received pin schedule item message");
        handle_pin_schedule_item(message->pin_schedule_item);
        break;
    case MATRX__SOCKET_MESSAGE__MESSAGE_SKIP_SCHEDULE_ITEM:
        ESP_LOGI(TAG, "received skip schedule item message");
        handle_skip_schedule_item(message->skip_schedule_item);
        break;
    default:
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

    client = esp_websocket_client_init(&websocket_cfg);
    esp_websocket_register_events(client, WEBSOCKET_EVENT_ANY, websocket_event_handler, (void*)client);

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
                free(message.message);
            }
            else {
                ESP_LOGI(TAG, "received message with length %d", message.message_len);
                for (int i = 0; i < message.message_len; i++) {
                    ESP_LOGI(TAG, "%c", message.message[i]);
                }

                Matrx__SocketMessage* socket_message = matrx__socket_message__unpack(NULL, message.message_len, (uint8_t*)message.message);
                if (socket_message == NULL) {
                    ESP_LOGE(TAG, "failed to unpack socket message");
                    free(message.message);
                    continue;
                }
                handle_message(socket_message);
            }
        }
    }
}

void sockets_init()
{
    xSocketsQueue = xQueueCreate(10, sizeof(ProcessableMessage_t));
    xTaskCreatePinnedToCore(sockets_task, "sockets", 2048, NULL, 5, &xSocketsTask, 1);
}

void sockets_connect()
{
    if (client != nullptr) {
        esp_websocket_client_start(client);
    }
}

void sockets_disconnect()
{
    if (client != nullptr) {
        esp_websocket_client_close(client, pdMS_TO_TICKS(1000));
    }
}

void request_render(uint8_t* schedule_item_uuid) {
    Matrx__RequestRender request = MATRX__REQUEST_RENDER__INIT;
    request.schedule_item_uuid.data = schedule_item_uuid;
    request.schedule_item_uuid.len = 16;

    Matrx__SocketMessage message = MATRX__SOCKET_MESSAGE__INIT;
    message.message_case = MATRX__SOCKET_MESSAGE__MESSAGE_REQUEST_RENDER;
    message.request_render = &request;

    size_t len = matrx__socket_message__get_packed_size(&message);
    uint8_t* buffer = (uint8_t*)malloc(len);
    if (buffer == NULL) {
        ESP_LOGE(TAG, "failed to allocate message buffer");
        return;
    }

    ProcessableMessage_t p_message;
    p_message.message = (char*)buffer;
    p_message.message_len = len;
    p_message.is_outbox = true;

    if (xQueueSend(xSocketsQueue, &p_message, pdMS_TO_TICKS(50)) != pdTRUE) {
        ESP_LOGE(TAG, "failed to send message to queue");
    }
}

void upload_coredump(uint8_t* core_dump, size_t core_dump_len) {
    Matrx__UploadCoreDump upload = MATRX__UPLOAD_CORE_DUMP__INIT;
    upload.core_dump.data = core_dump;
    upload.core_dump.len = core_dump_len;

    Matrx__SocketMessage message = MATRX__SOCKET_MESSAGE__INIT;
    message.message_case = MATRX__SOCKET_MESSAGE__MESSAGE_UPLOAD_CORE_DUMP;
    message.upload_core_dump = &upload;

    size_t len = matrx__socket_message__get_packed_size(&message);
    uint8_t* buffer = (uint8_t*)malloc(len);
    if (buffer == NULL) {
        ESP_LOGE(TAG, "failed to allocate message buffer");
        return;
    }

    ProcessableMessage_t p_message;
    p_message.message = (char*)buffer;
    p_message.message_len = len;
    p_message.is_outbox = true;

    if (xQueueSend(xSocketsQueue, &p_message, pdMS_TO_TICKS(50)) != pdTRUE) {
        ESP_LOGE(TAG, "failed to send message to queue");
    }
}

void request_schedule() {
    Matrx__RequestSchedule request = MATRX__REQUEST_SCHEDULE__INIT;

    Matrx__SocketMessage message = MATRX__SOCKET_MESSAGE__INIT;
    message.message_case = MATRX__SOCKET_MESSAGE__MESSAGE_REQUEST_SCHEDULE;
    message.request_schedule = &request;

    size_t len = matrx__socket_message__get_packed_size(&message);
    uint8_t* buffer = (uint8_t*)malloc(len);
    if (buffer == NULL) {
        ESP_LOGE(TAG, "failed to allocate message buffer");
        return;
    }

    ProcessableMessage_t p_message;
    p_message.message = (char*)buffer;
    p_message.message_len = len;
    p_message.is_outbox = true;

    if (xQueueSend(xSocketsQueue, &p_message, pdMS_TO_TICKS(50)) != pdTRUE) {
        ESP_LOGE(TAG, "failed to send message to queue");
    }
}

void attempt_coredump_upload() {
    ESP_LOGI(TAG, "attempting coredump upload");

    // Find the coredump partition
    const esp_partition_t* core_dump_partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_COREDUMP, "coredump");

    if (!core_dump_partition)
    {
        return;
    }

    // Read the core dump size
    size_t core_dump_size = core_dump_partition->size;

    size_t encoded_size = 0;
    uint8_t* encoded_data;
    bool is_erased = true;

    uint8_t* core_dump_data = (uint8_t*)heap_caps_malloc(core_dump_size + 1, MALLOC_CAP_SPIRAM); // used as return, so freed in calling function
    if (!core_dump_data)
    {
        ESP_LOGE(TAG, "Failed to allocate memory for core dump");
        goto exit;
    }
    if (esp_partition_read(core_dump_partition, 0, core_dump_data, core_dump_size) != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to read core dump data");
        goto exit;
    }

    //if coredump is all 0xFF, then it's erased
    for (size_t i = 0; i < core_dump_size; i++)
    {
        if (core_dump_data[i] != 0xFF)
        {
            is_erased = false;
            break;
        }
    }

    if (is_erased)
    {
        ESP_LOGI(TAG, "Core dump partition is empty");
        goto exit;
    }

    // Calculate Base64 encoded size
    mbedtls_base64_encode(NULL, 0, &encoded_size, core_dump_data, core_dump_size);
    encoded_data = (uint8_t*)heap_caps_malloc(encoded_size + 1, MALLOC_CAP_SPIRAM); // used as return, so freed in calling function
    if (!encoded_data)
    {
        ESP_LOGE(TAG, "Failed to allocate memory for encoded data");
        goto exit;
    }

    // Encode core dump to Base64
    if (mbedtls_base64_encode(encoded_data, encoded_size, &encoded_size, core_dump_data, core_dump_size) != 0)
    {
        goto exit;
    }

    encoded_data[encoded_size] = '\0'; // Null-terminate the Base64 string
    upload_coredump(encoded_data, encoded_size);

    //clear the coredump partition
    if (esp_partition_erase_range(core_dump_partition, 0, core_dump_size) != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to erase core dump partition");
    }

exit:
    free(core_dump_data);
    free(encoded_data);
}
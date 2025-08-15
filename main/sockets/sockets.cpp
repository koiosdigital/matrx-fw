#include "sockets.h"

#include "esp_log.h"
#include "esp_websocket_client.h"
#include "esp_event.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_wifi.h"
#include "esp_timer.h"

#include "kd_common.h"
#include "display.h"
#include "scheduler.h"
#include "sprites.h"

#include "kd_matrx.pb-c.h"
#include "kd_global.pb-c.h"
#include "device-api.pb-c.h"

#include "cJSON.h"
#include <esp_partition.h>
#include <mbedtls/base64.h>

static const char* TAG = "sockets";
TaskHandle_t xSocketsTask = NULL;

QueueHandle_t xSocketsQueue = NULL;
esp_websocket_client_handle_t client = NULL;

#define UUID_SIZE_BYTES 16

void send_socket_message(Kd__DeviceAPIMessage* message);

// Helper function to send actual render request
static void send_render_request_internal(uint8_t* schedule_item_uuid) {
    ESP_LOGI(TAG, "sending render request for item");

    Kd__RequestRender request = KD__REQUEST_RENDER__INIT;
    request.uuid.data = schedule_item_uuid;
    request.uuid.len = UUID_SIZE_BYTES;

    Kd__KDMatrxMessage message = KD__KDMATRX_MESSAGE__INIT;
    message.message_case = KD__KDMATRX_MESSAGE__MESSAGE_REQUEST_RENDER;
    message.request_render = &request;

    Kd__DeviceAPIMessage device_message = KD__DEVICE_APIMESSAGE__INIT;
    device_message.message_case = KD__DEVICE_APIMESSAGE__MESSAGE_KD_MATRX_MESSAGE;
    device_message.kd_matrx_message = &message;

    send_socket_message(&device_message);
}

typedef struct ProcessableMessage_t {
    char* message;
    size_t message_len;
    bool is_outbox;
} ProcessableMessage_t;

static void websocket_event_handler(void* handler_args, esp_event_base_t base, int32_t event_id, void* event_data)
{
    esp_websocket_event_data_t* data = (esp_websocket_event_data_t*)event_data;
    static char* dbuf = NULL;

    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "connected");
        show_fs_sprite("/fs/ready.webp");
        request_schedule();
        attempt_coredump_upload();
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "disconnected");
        if (dbuf) {
            free(dbuf);
            dbuf = NULL;
        }
        show_fs_sprite("/fs/connect_cloud.webp");
        break;
    case WEBSOCKET_EVENT_DATA:
        if (data->payload_offset == 0) {
            if (dbuf) {
                free(dbuf);
                dbuf = NULL;
            }

            dbuf = (char*)heap_caps_calloc(data->payload_len + 1, sizeof(char), MALLOC_CAP_SPIRAM);
            if (dbuf == NULL) {
                ESP_LOGE(TAG, "malloc failed: dbuf (%d)", data->payload_len);
                return;
            }
        }

        if (dbuf) {
            memcpy(dbuf + data->payload_offset, data->data_ptr, data->data_len);
        }

        if (data->payload_offset + data->data_len >= data->payload_len) {
            ProcessableMessage_t message;
            message.message = dbuf;
            message.message_len = data->payload_len;
            message.is_outbox = false;

            if (xQueueSend(xSocketsQueue, &message, pdMS_TO_TICKS(50)) != pdTRUE) {
                ESP_LOGE(TAG, "failed to send message to queue");
                free(dbuf);
            }
            dbuf = NULL; // Reset dbuf after processing
        }

        break;
    default:
        break;
    }
}

void handle_schedule_response(Kd__ScheduleResponse* response)
{
    if (response->n_schedule_items == 0) {
        scheduler_clear();
        return;
    }

    scheduler_set_schedule(response);
    scheduler_start();
}

void handle_render_response(Kd__RenderResponse* response)
{
    ScheduleItem_t* item = find_schedule_item(response->uuid.data);
    if (item == NULL) {
        ESP_LOGE(TAG, "failed to find schedule item");
        return;
    }

    if (response->render_error == true) {
        item->flags.skipped_server = true;
        return;
    }

    item->flags.skipped_server = false;
    sprite_update_data(item->sprite, response->sprite_data.data, response->sprite_data.len);
}

void handle_pin_schedule_item(Kd__PinScheduleItem* pin)
{
    ESP_LOGI(TAG, "received pin schedule item");
}

void handle_skip_schedule_item(Kd__SkipScheduleItem* skip)
{
    ESP_LOGI(TAG, "received skip schedule item");
}

void handle_message(Kd__KDMatrxMessage* message)
{
    switch (message->message_case) {
    case KD__KDMATRX_MESSAGE__MESSAGE_SCHEDULE_RESPONSE:
        handle_schedule_response(message->schedule_response);
        break;
    case KD__KDMATRX_MESSAGE__MESSAGE_RENDER_RESPONSE:
        handle_render_response(message->render_response);
        break;
    case KD__KDMATRX_MESSAGE__MESSAGE_PIN_SCHEDULE_ITEM:
        handle_pin_schedule_item(message->pin_schedule_item);
        break;
    case KD__KDMATRX_MESSAGE__MESSAGE_SKIP_SCHEDULE_ITEM:
        handle_skip_schedule_item(message->skip_schedule_item);
        break;
    default:
        break;
    }

    // Don't free the message here - it's freed by the caller along with the parent device_message
}

void event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT) {
        sockets_disconnect();
    }
    if (event_base == IP_EVENT) {
        sockets_connect();
    }
}

void sockets_task(void* pvParameter)
{
    while (1) {
        if (kd_common_crypto_get_state() != CryptoState_t::CRYPTO_STATE_VALID_CERT) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        break;
    }

    esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &event_handler, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL);

    esp_ds_data_ctx_t* ds_data_ctx = kd_common_crypto_get_ctx();
    char* cert = (char*)calloc(4096, sizeof(char));
    size_t cert_len = 4096;

    kd_common_get_device_cert(cert, &cert_len);

    esp_websocket_client_config_t websocket_cfg = {
        .uri = SOCKETS_URI,
        .port = 9091,
        //.client_cert = cert,
        //.client_cert_len = cert_len + 1,
        //.client_ds_data = ds_data_ctx,
        //.crt_bundle_attach = esp_crt_bundle_attach,
        .reconnect_timeout_ms = 5000,
        .ping_interval_sec = 5,
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
                if (esp_websocket_client_is_connected(client)) {
                    esp_err_t ret = esp_websocket_client_send_bin(client, message.message, message.message_len, pdMS_TO_TICKS(10000));
                    if (ret == -1) {
                        ESP_LOGE(TAG, "failed to send websocket message: %s", esp_err_to_name(ret));
                    }
                }
                else {
                    ESP_LOGW(TAG, "websocket not connected, dropping outbox message");
                }
                free(message.message);
                continue;
            }

            Kd__DeviceAPIMessage* device_message = kd__device_apimessage__unpack(NULL, message.message_len, (uint8_t*)message.message);
            if (device_message == NULL) {
                ESP_LOGE(TAG, "failed to unpack device message");
                free(message.message);
                continue;
            }
            if (device_message->message_case != KD__DEVICE_APIMESSAGE__MESSAGE_KD_MATRX_MESSAGE) {
                kd__device_apimessage__free_unpacked(device_message, NULL);
                free(message.message);
                continue;
            }
            handle_message(device_message->kd_matrx_message);
            kd__device_apimessage__free_unpacked(device_message, NULL);
            free(message.message);
        }
    }
}

void sockets_init()
{
    xSocketsQueue = xQueueCreate(20, sizeof(ProcessableMessage_t));

    xTaskCreatePinnedToCore(sockets_task, "sockets", 4096, NULL, 5, &xSocketsTask, 1);
}

void sockets_connect()
{
    if (client == NULL) {
        return;
    }

    esp_websocket_client_start(client);
}

void sockets_disconnect()
{
    if (client == NULL) {
        return;
    }

    esp_websocket_client_close(client, pdMS_TO_TICKS(1000));
}

void send_socket_message(Kd__DeviceAPIMessage* message)
{
    // Check if websocket is connected before queuing
    if (client == NULL || !esp_websocket_client_is_connected(client)) {
        ESP_LOGW(TAG, "websocket not connected, dropping message");
        return;
    }

    size_t len = kd__device_apimessage__get_packed_size(message);
    uint8_t* buffer = (uint8_t*)heap_caps_calloc(len, sizeof(uint8_t), MALLOC_CAP_SPIRAM);
    if (buffer == NULL) {
        ESP_LOGE(TAG, "failed to allocate message buffer");
        return;
    }

    kd__device_apimessage__pack(message, buffer);
    ProcessableMessage_t p_message;
    p_message.message = (char*)buffer;
    p_message.message_len = len;
    p_message.is_outbox = true;

    if (xQueueSend(xSocketsQueue, &p_message, pdMS_TO_TICKS(50)) != pdTRUE) {
        ESP_LOGE(TAG, "failed to send message to queue");
        free(buffer);  // Free buffer if queue send fails
        return;
    }

    ESP_LOGI(TAG, "sent message to queue: %.*s", (int)len, p_message.message);
}

void request_render(uint8_t* schedule_item_uuid) {
    send_render_request_internal(schedule_item_uuid);
}

void upload_coredump(uint8_t* core_dump, size_t core_dump_len) {
    Kd__UploadCoreDump upload = KD__UPLOAD_CORE_DUMP__INIT;
    upload.core_dump.data = core_dump;
    upload.core_dump.len = core_dump_len;

    Kd__KDGlobalMessage message = KD__KDGLOBAL_MESSAGE__INIT;
    message.message_case = KD__KDGLOBAL_MESSAGE__MESSAGE_UPLOAD_CORE_DUMP;
    message.upload_core_dump = &upload;

    Kd__DeviceAPIMessage device_message = KD__DEVICE_APIMESSAGE__INIT;
    device_message.message_case = KD__DEVICE_APIMESSAGE__MESSAGE_KD_GLOBAL_MESSAGE;
    device_message.kd_global_message = &message;

    send_socket_message(&device_message);
}

void request_schedule() {
    Kd__RequestSchedule request = KD__REQUEST_SCHEDULE__INIT;

    Kd__KDMatrxMessage message = KD__KDMATRX_MESSAGE__INIT;
    message.message_case = KD__KDMATRX_MESSAGE__MESSAGE_REQUEST_SCHEDULE;
    message.request_schedule = &request;

    Kd__DeviceAPIMessage device_message = KD__DEVICE_APIMESSAGE__INIT;
    device_message.message_case = KD__DEVICE_APIMESSAGE__MESSAGE_KD_MATRX_MESSAGE;
    device_message.kd_matrx_message = &message;

    send_socket_message(&device_message);
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
    uint8_t* encoded_data = 0;
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
    encoded_data = (uint8_t*)malloc(encoded_size + 1); // used as return, so freed in calling function
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
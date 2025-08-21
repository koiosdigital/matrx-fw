#include "sockets.h"

#include "esp_log.h"
#include "esp_websocket_client.h"
#include "esp_event.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_wifi.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

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
#define RENDER_REQUEST_TIMEOUT_MS 5000
#define MAX_PENDING_RENDER_REQUESTS 10

// Structure to track pending render requests
typedef struct PendingRenderRequest_t {
    uint8_t uuid[UUID_SIZE_BYTES];
    int64_t timestamp_ms;
    bool in_use;
} PendingRenderRequest_t;

// Array to track pending render requests
static PendingRenderRequest_t pending_render_requests[MAX_PENDING_RENDER_REQUESTS] = { 0 };
static SemaphoreHandle_t render_requests_mutex = NULL;

// Connection watchdog - track last activity to detect stale connections
static int64_t last_websocket_activity_ms = 0;
#define CONNECTION_WATCHDOG_TIMEOUT_MS 60000  // 60 seconds of no activity triggers reconnect

// WiFi connectivity state - tracks if we can connect to WebSocket
static bool connectable = false;

// Socket connection error tracking
static uint32_t socket_connection_error_count = 0;
#define MAX_SOCKET_CONNECTION_ERRORS 5

// Task notifications for socket operations
typedef enum SocketTaskNotification_t {
    SOCKET_TASK_REINIT = 1,
    SOCKET_TASK_RECONNECT = 2,
} SocketTaskNotification_t;

void send_socket_message(Kd__DeviceAPIMessage* message);

// Forward declarations for internal functions
static esp_err_t socket_init();
static void socket_deinit();

// Helper functions for tracking render requests
static void cleanup_expired_render_requests() {
    if (render_requests_mutex == NULL) return;

    xSemaphoreTake(render_requests_mutex, portMAX_DELAY);

    int64_t current_time = esp_timer_get_time() / 1000; // Convert to milliseconds

    for (int i = 0; i < MAX_PENDING_RENDER_REQUESTS; i++) {
        if (pending_render_requests[i].in_use) {
            if (current_time - pending_render_requests[i].timestamp_ms >= RENDER_REQUEST_TIMEOUT_MS) {
                pending_render_requests[i].in_use = false;
                memset(pending_render_requests[i].uuid, 0, UUID_SIZE_BYTES);
            }
        }
    }

    xSemaphoreGive(render_requests_mutex);
}

static bool is_render_request_pending(uint8_t* uuid) {
    if (render_requests_mutex == NULL || uuid == NULL) return false;

    cleanup_expired_render_requests();

    xSemaphoreTake(render_requests_mutex, portMAX_DELAY);

    for (int i = 0; i < MAX_PENDING_RENDER_REQUESTS; i++) {
        if (pending_render_requests[i].in_use &&
            memcmp(pending_render_requests[i].uuid, uuid, UUID_SIZE_BYTES) == 0) {
            xSemaphoreGive(render_requests_mutex);
            return true;
        }
    }

    xSemaphoreGive(render_requests_mutex);
    return false;
}

static bool add_pending_render_request(uint8_t* uuid) {
    if (render_requests_mutex == NULL || uuid == NULL) return false;

    cleanup_expired_render_requests();

    xSemaphoreTake(render_requests_mutex, portMAX_DELAY);

    // Find an empty slot
    for (int i = 0; i < MAX_PENDING_RENDER_REQUESTS; i++) {
        if (!pending_render_requests[i].in_use) {
            memcpy(pending_render_requests[i].uuid, uuid, UUID_SIZE_BYTES);
            pending_render_requests[i].timestamp_ms = esp_timer_get_time() / 1000;
            pending_render_requests[i].in_use = true;

            xSemaphoreGive(render_requests_mutex);
            ESP_LOGD(TAG, "Added pending render request for UUID");
            return true;
        }
    }

    xSemaphoreGive(render_requests_mutex);
    ESP_LOGW(TAG, "No space for new render request, max pending requests reached");
    return false;
}

static void remove_pending_render_request(uint8_t* uuid) {
    if (render_requests_mutex == NULL || uuid == NULL) return;

    xSemaphoreTake(render_requests_mutex, portMAX_DELAY);

    for (int i = 0; i < MAX_PENDING_RENDER_REQUESTS; i++) {
        if (pending_render_requests[i].in_use &&
            memcmp(pending_render_requests[i].uuid, uuid, UUID_SIZE_BYTES) == 0) {
            pending_render_requests[i].in_use = false;
            memset(pending_render_requests[i].uuid, 0, UUID_SIZE_BYTES);
            ESP_LOGD(TAG, "Removed pending render request for UUID");
            break;
        }
    }

    xSemaphoreGive(render_requests_mutex);
}

static void clear_all_pending_render_requests() {
    if (render_requests_mutex == NULL) return;

    xSemaphoreTake(render_requests_mutex, portMAX_DELAY);

    for (int i = 0; i < MAX_PENDING_RENDER_REQUESTS; i++) {
        pending_render_requests[i].in_use = false;
        memset(pending_render_requests[i].uuid, 0, UUID_SIZE_BYTES);
    }

    xSemaphoreGive(render_requests_mutex);
}

// Static variables for certificate handling - initialized once and reused
static char* static_cert = NULL;
static size_t static_cert_len = 0;
static esp_ds_data_ctx_t* static_ds_data_ctx = NULL;

// Helper function to clean up static certificate data
static void cleanup_static_cert_data() {
    if (static_cert != NULL) {
        free(static_cert);
        static_cert = NULL;
        static_cert_len = 0;
        static_ds_data_ctx = NULL;
    }
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
        last_websocket_activity_ms = esp_timer_get_time() / 1000; // Update activity timestamp
        socket_connection_error_count = 0; // Reset error counter on successful connection
        show_fs_sprite("ready");
        request_schedule();
        attempt_coredump_upload();
        break;

    case WEBSOCKET_EVENT_DISCONNECTED:
        if (dbuf) {
            free(dbuf);
            dbuf = NULL;
        }
        // Clear all pending render requests since websocket is disconnected
        clear_all_pending_render_requests();
        show_fs_sprite("connect");
        if (xSocketsTask != NULL) {
            xTaskNotify(xSocketsTask, SOCKET_TASK_RECONNECT, eSetValueWithOverwrite);
        }
        break;

    case WEBSOCKET_EVENT_ERROR:
        if (dbuf) {
            free(dbuf);
            dbuf = NULL;
        }
        clear_all_pending_render_requests();

        // Count websocket errors when WiFi is connected
        if (connectable) {
            socket_connection_error_count++;
            if (socket_connection_error_count >= MAX_SOCKET_CONNECTION_ERRORS) {
                esp_restart();
            }
        }

        if (xSocketsTask != NULL) {
            xTaskNotify(xSocketsTask, SOCKET_TASK_REINIT, eSetValueWithOverwrite);
        }
        break;

    case WEBSOCKET_EVENT_DATA:
        last_websocket_activity_ms = esp_timer_get_time() / 1000; // Update activity timestamp

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
        ESP_LOGD(TAG, "Unhandled websocket event: %ld", event_id);
        break;
    }
}

// Websocket initialization function
static esp_err_t socket_init() {
    if (client != NULL) {
        ESP_LOGW(TAG, "Client already initialized, deinitializing first");
        socket_deinit();
    }

    // Initialize certificate data if not already done
    if (static_cert == NULL) {
        static_ds_data_ctx = kd_common_crypto_get_ctx();
        static_cert = (char*)calloc(4096, sizeof(char));
        if (static_cert == NULL) {
            ESP_LOGE(TAG, "Failed to allocate certificate buffer");
            return ESP_ERR_NO_MEM;
        }

        static_cert_len = 4096;
        esp_err_t ret = kd_common_get_device_cert(static_cert, &static_cert_len);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to get device certificate: %s", esp_err_to_name(ret));
            free(static_cert);
            static_cert = NULL;
            return ret;
        }
    }
    else {
        ESP_LOGD(TAG, "Using cached certificate data");
    }

    esp_websocket_client_config_t websocket_cfg = {
        .uri = SOCKETS_URI,
        .port = 443,
        .client_cert = static_cert,
        .client_cert_len = static_cert_len + 1,
        .client_ds_data = static_ds_data_ctx,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .reconnect_timeout_ms = 1000,      // Faster initial reconnect attempts  
        .network_timeout_ms = 5000,        // Shorter timeout to fail fast and retry
        .ping_interval_sec = 10,           // Keep connection alive with pings
    };

    client = esp_websocket_client_init(&websocket_cfg);
    if (client == NULL) {
        ESP_LOGE(TAG, "Failed to initialize websocket client");
        return ESP_FAIL;
    }

    esp_err_t ret = esp_websocket_register_events(client, WEBSOCKET_EVENT_ANY, websocket_event_handler, (void*)client);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register websocket events: %s", esp_err_to_name(ret));
        esp_websocket_client_destroy(client);
        client = NULL;
        return ret;
    }
    return ESP_OK;
}

// Websocket deinitialization function
static void socket_deinit() {
    ESP_LOGI(TAG, "Deinitializing websocket client");

    if (client != NULL) {
        // Stop the client first
        esp_websocket_client_stop(client);

        // Destroy the client
        esp_websocket_client_destroy(client);
        client = NULL;

        // Clear all pending render requests
        clear_all_pending_render_requests();
    }
}

// Helper function to send actual render request
static void send_render_request_internal(uint8_t* schedule_item_uuid) {
    Kd__RequestRender request = KD__REQUEST_RENDER__INIT;
    request.uuid.data = schedule_item_uuid;
    request.uuid.len = UUID_SIZE_BYTES;

    request.width = CONFIG_MATRIX_WIDTH;
    request.height = CONFIG_MATRIX_HEIGHT;

    Kd__KDMatrxMessage message = KD__KDMATRX_MESSAGE__INIT;
    message.message_case = KD__KDMATRX_MESSAGE__MESSAGE_REQUEST_RENDER;
    message.request_render = &request;

    Kd__DeviceAPIMessage device_message = KD__DEVICE_APIMESSAGE__INIT;
    device_message.message_case = KD__DEVICE_APIMESSAGE__MESSAGE_KD_MATRX_MESSAGE;
    device_message.kd_matrx_message = &message;

    send_socket_message(&device_message);
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
    // Remove from pending requests tracking as we received a response
    remove_pending_render_request(response->uuid.data);

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

void handle_modify_schedule_item(Kd__ModifyScheduleItem* modify)
{

}

void handle_global_message(Kd__KDGlobalMessage* message) {
    switch (message->message_case) {
    case KD__KDGLOBAL_MESSAGE__MESSAGE_JOIN_RESPONSE:
        break;
    default:
        break;
    }
}

void handle_matrx_message(Kd__KDMatrxMessage* message)
{
    switch (message->message_case) {
    case KD__KDMATRX_MESSAGE__MESSAGE_SCHEDULE_RESPONSE:
        handle_schedule_response(message->schedule_response);
        break;
    case KD__KDMATRX_MESSAGE__MESSAGE_RENDER_RESPONSE:
        handle_render_response(message->render_response);
        break;
    case KD__KDMATRX_MESSAGE__MESSAGE_MODIFY_SCHEDULE_ITEM:
        handle_modify_schedule_item(message->modify_schedule_item);
        break;
    default:
        break;
    }

    // Don't free the message here - it's freed by the caller along with the parent device_message
}

void event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT) {
        if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
            connectable = false;
            sockets_disconnect();
        }
    }
    if (event_base == IP_EVENT) {
        if (event_id == IP_EVENT_STA_GOT_IP) {
            connectable = true;
            socket_connection_error_count = 0; // Reset error counter when WiFi connects
            sockets_connect();
        }
    }
}

void sockets_task(void* pvParameter)
{
    uint32_t reconnect_delay_ms = 1000; // Start with 1 second
    const uint32_t max_reconnect_delay_ms = 30000; // Maximum 30 seconds
    uint32_t init_retry_count = 0;

    // Wait for crypto to be ready
    while (1) {
        if (kd_common_crypto_get_state() != CryptoState_t::CRYPTO_STATE_VALID_CERT) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        break;
    }

    esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &event_handler, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL);

    // Initialize activity timestamp
    last_websocket_activity_ms = esp_timer_get_time() / 1000;

    // Initialize websocket client with infinite retry logic - NEVER GIVE UP!
    while (1) {
        esp_err_t ret = socket_init();
        if (ret == ESP_OK) {
            reconnect_delay_ms = 1000; // Reset delay on success
            socket_connection_error_count = 0; // Reset error counter on successful init
            break;
        }
        else {
            init_retry_count++;

            // Count initialization errors when WiFi is connected
            if (connectable) {
                socket_connection_error_count++;
                ESP_LOGW(TAG, "Socket init error, connection error count: %lu/%d", socket_connection_error_count, MAX_SOCKET_CONNECTION_ERRORS);

                if (socket_connection_error_count >= MAX_SOCKET_CONNECTION_ERRORS) {
                    ESP_LOGE(TAG, "Maximum socket connection errors reached (%d), restarting ESP32...", MAX_SOCKET_CONNECTION_ERRORS);
                    esp_restart();
                }
            }

            vTaskDelay(pdMS_TO_TICKS(reconnect_delay_ms));

            // Exponential backoff with cap
            reconnect_delay_ms = (reconnect_delay_ms * 2 > max_reconnect_delay_ms) ?
                max_reconnect_delay_ms : reconnect_delay_ms * 2;
        }
    }

    ProcessableMessage_t message;
    TickType_t last_cleanup_time = xTaskGetTickCount();

    while (1)
    {
        // Check for task notifications (reinit/reconnect commands)
        uint32_t notification_value;
        if (xTaskNotifyWait(0, ULONG_MAX, &notification_value, pdMS_TO_TICKS(100)) == pdTRUE) {
            SocketTaskNotification_t notification = (SocketTaskNotification_t)notification_value;

            switch (notification) {
            case SOCKET_TASK_REINIT:
                socket_deinit();
                vTaskDelay(pdMS_TO_TICKS(reconnect_delay_ms));
                while (1) {
                    esp_err_t ret = socket_init();
                    if (ret == ESP_OK) {
                        reconnect_delay_ms = 1000; // Reset delay on success
                        socket_connection_error_count = 0; // Reset error counter on successful reinit
                        break;
                    }
                    else {
                        ESP_LOGE(TAG, "Failed to reinitialize websocket: %s", esp_err_to_name(ret));

                        // Count reinit errors when WiFi is connected
                        if (connectable) {
                            socket_connection_error_count++;
                            ESP_LOGW(TAG, "Socket reinit error, connection error count: %lu/%d", socket_connection_error_count, MAX_SOCKET_CONNECTION_ERRORS);

                            if (socket_connection_error_count >= MAX_SOCKET_CONNECTION_ERRORS) {
                                ESP_LOGE(TAG, "Maximum socket connection errors reached (%d), restarting ESP32...", MAX_SOCKET_CONNECTION_ERRORS);
                                esp_restart();
                            }
                        }

                        vTaskDelay(pdMS_TO_TICKS(reconnect_delay_ms));

                        // Exponential backoff for next attempt
                        reconnect_delay_ms = (reconnect_delay_ms * 2 > max_reconnect_delay_ms) ?
                            max_reconnect_delay_ms : reconnect_delay_ms * 2;
                    }
                }
                break;

            case SOCKET_TASK_RECONNECT:
                if (client != NULL) {
                    sockets_connect();
                    reconnect_delay_ms = 1000; // Reset delay on reconnect attempt
                }
                else {
                    ESP_LOGW(TAG, "Cannot reconnect, client not initialized, reinitializing");
                    // Keep trying to reinitialize until success - NEVER GIVE UP!
                    while (1) {
                        esp_err_t ret = socket_init();
                        if (ret == ESP_OK) {
                            sockets_connect();
                            reconnect_delay_ms = 1000; // Reset delay on success
                            socket_connection_error_count = 0; // Reset error counter on successful init
                            break;
                        }
                        else {
                            ESP_LOGE(TAG, "Failed to initialize for reconnect: %s", esp_err_to_name(ret));

                            // Count reconnect init errors when WiFi is connected
                            if (connectable) {
                                socket_connection_error_count++;
                                ESP_LOGW(TAG, "Socket reconnect init error, connection error count: %lu/%d", socket_connection_error_count, MAX_SOCKET_CONNECTION_ERRORS);

                                if (socket_connection_error_count >= MAX_SOCKET_CONNECTION_ERRORS) {
                                    ESP_LOGE(TAG, "Maximum socket connection errors reached (%d), restarting ESP32...", MAX_SOCKET_CONNECTION_ERRORS);
                                    esp_restart();
                                }
                            }

                            vTaskDelay(pdMS_TO_TICKS(reconnect_delay_ms));

                            // Exponential backoff
                            reconnect_delay_ms = (reconnect_delay_ms * 2 > max_reconnect_delay_ms) ?
                                max_reconnect_delay_ms : reconnect_delay_ms * 2;
                        }
                    }
                }
                break;

            default:
                ESP_LOGW(TAG, "Unknown socket task notification: %lu", notification_value);
                break;
            }
        }

        // Use timeout to allow periodic cleanup of expired render requests
        if (xQueueReceive(xSocketsQueue, &message, pdMS_TO_TICKS(900)) == pdTRUE) {
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

            switch (device_message->message_case) {
            case KD__DEVICE_APIMESSAGE__MESSAGE_KD_GLOBAL_MESSAGE:
                handle_global_message(device_message->kd_global_message);
                break;
            case KD__DEVICE_APIMESSAGE__MESSAGE_KD_MATRX_MESSAGE:
                handle_matrx_message(device_message->kd_matrx_message);
                break;
            default:
                break;
            }

            kd__device_apimessage__free_unpacked(device_message, NULL);
            free(message.message);
        }

        // Periodic cleanup and aggressive connection monitoring (every 5 seconds)
        TickType_t current_time = xTaskGetTickCount();
        if (current_time - last_cleanup_time >= pdMS_TO_TICKS(5000)) {
            cleanup_expired_render_requests();
            last_cleanup_time = current_time;

            int64_t current_time_ms = esp_timer_get_time() / 1000;

            // NEVER GIVE UP - Proactive connection monitoring and recovery!
            if (client != NULL && esp_websocket_client_is_connected(client)) {
                // Check for connection watchdog timeout
                if (last_websocket_activity_ms > 0 &&
                    (current_time_ms - last_websocket_activity_ms) > CONNECTION_WATCHDOG_TIMEOUT_MS) {
                    xTaskNotify(xSocketsTask, SOCKET_TASK_RECONNECT, eSetValueWithOverwrite);
                }
            }
            else if (client != NULL && !esp_websocket_client_is_connected(client)) {
                sockets_connect();
            }
            else if (client == NULL) {
                xTaskNotify(xSocketsTask, SOCKET_TASK_REINIT, eSetValueWithOverwrite);
            }
        }
    }
}

void sockets_init()
{
    connectable = false;

    xSocketsQueue = xQueueCreate(20, sizeof(ProcessableMessage_t));
    if (xSocketsQueue == NULL) {
        ESP_LOGE(TAG, "Failed to create sockets queue");
        return;
    }

    // Initialize mutex for tracking render requests
    render_requests_mutex = xSemaphoreCreateMutex();
    if (render_requests_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create render requests mutex");
        return;
    }

    BaseType_t ret = xTaskCreatePinnedToCore(sockets_task, "sockets", 8192, NULL, 5, &xSocketsTask, 1);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create sockets task");
        return;
    }
}

void sockets_deinit()
{
    // Reset connectivity state
    connectable = false;

    // Stop and destroy websocket client
    socket_deinit();

    // Clean up static certificate data
    cleanup_static_cert_data();

    // Delete task if it exists
    if (xSocketsTask != NULL) {
        vTaskDelete(xSocketsTask);
        xSocketsTask = NULL;
    }

    // Delete queue
    if (xSocketsQueue != NULL) {
        vQueueDelete(xSocketsQueue);
        xSocketsQueue = NULL;
    }

    // Delete mutex
    if (render_requests_mutex != NULL) {
        vSemaphoreDelete(render_requests_mutex);
        render_requests_mutex = NULL;
    }
}

void sockets_connect()
{
    if (!connectable) {
        ESP_LOGW(TAG, "Cannot connect: WiFi not connected (connectable=false)");
        return;
    }

    if (client == NULL) {
        ESP_LOGW(TAG, "Cannot connect: websocket client not initialized");
        return;
    }

    esp_err_t ret = esp_websocket_client_start(client);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start websocket client: %s", esp_err_to_name(ret));

        // Count connection errors when WiFi is connected
        if (connectable) {
            socket_connection_error_count++;
            ESP_LOGW(TAG, "Socket connection error count: %lu/%d", socket_connection_error_count, MAX_SOCKET_CONNECTION_ERRORS);

            if (socket_connection_error_count >= MAX_SOCKET_CONNECTION_ERRORS) {
                ESP_LOGE(TAG, "Maximum socket connection errors reached (%d), restarting ESP32...", MAX_SOCKET_CONNECTION_ERRORS);
                esp_restart();
            }
        }
    }
}

void sockets_disconnect()
{
    if (client == NULL) {
        ESP_LOGW(TAG, "Cannot disconnect: websocket client not initialized");
        return;
    }

    esp_err_t ret = esp_websocket_client_close(client, pdMS_TO_TICKS(1000));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to close websocket client: %s", esp_err_to_name(ret));
    }
}

void send_socket_message(Kd__DeviceAPIMessage* message)
{
    if (!connectable) {
        return;
    }

    if (client == NULL || !esp_websocket_client_is_connected(client)) {
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
}

void request_render(uint8_t* schedule_item_uuid) {
    if (schedule_item_uuid == NULL) {
        ESP_LOGE(TAG, "request_render called with null UUID");
        return;
    }

    char uuid_str[UUID_SIZE_BYTES * 2 + 1] = { 0 };
    for (int i = 0; i < UUID_SIZE_BYTES; i++) {
        sprintf(uuid_str + i * 2, "%02x", schedule_item_uuid[i]);
    }
    ESP_LOGI(TAG, "Requesting render for UUID: %s", uuid_str);

    // Check if a render request for this UUID is already pending
    if (is_render_request_pending(schedule_item_uuid)) {
        ESP_LOGD(TAG, "Render request already pending for UUID, skipping duplicate");
        return;
    }

    // Add to pending requests tracking
    if (!add_pending_render_request(schedule_item_uuid)) {
        ESP_LOGW(TAG, "Failed to add render request to pending list, sending anyway");
    }

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
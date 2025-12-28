#include "sockets.h"

#include <esp_log.h>
#include <esp_websocket_client.h>
#include <esp_event.h>
#include <esp_crt_bundle.h>
#include <esp_wifi.h>
#include <esp_timer.h>
#include <esp_partition.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>

#include <kd_common.h>
#include "display.h"
#include "scheduler.h"
#include "sprites.h"
#include "config.h"

#include <kd/v1/matrx.pb-c.h>

#include <cstring>

static const char* TAG = "sockets";

namespace {

    // Configuration
    constexpr size_t OUTBOX_QUEUE_SIZE = 16;
    constexpr size_t INBOX_QUEUE_SIZE = 8;
    constexpr size_t MAX_MESSAGE_SIZE = 150 * 1024;  // 150KB max for large sprite data
    constexpr int SEND_TIMEOUT_MS = 10000;
    constexpr int RECONNECT_TIMEOUT_MS = 5000;
    constexpr int PING_INTERVAL_SEC = 30;
    constexpr int64_t CLAIM_RETRY_INTERVAL_MS = 5000;
    constexpr size_t CLAIM_TOKEN_MAX_LEN = 2048;

    // Connection state
    enum class ConnectionState : uint8_t {
        WaitingForNetwork,
        WaitingForCrypto,
        WaitingForOTA,
        Connecting,
        Connected,
        Disconnected,
    };

    // Queued message (for both inbound and outbound)
    struct QueuedMessage {
        uint8_t* data;
        size_t len;
    };

    // Receive buffer for handling fragmented messages
    struct ReceiveBuffer {
        uint8_t* data = nullptr;
        size_t capacity = 0;
        size_t received = 0;
        size_t expected = 0;

        void reset() {
            if (data != nullptr) {
                free(data);
                data = nullptr;
            }
            capacity = 0;
            received = 0;
            expected = 0;
        }

        bool allocate(size_t size) {
            reset();
            if (size > MAX_MESSAGE_SIZE) {
                ESP_LOGE(TAG, "Message too large: %zu > %zu", size, MAX_MESSAGE_SIZE);
                return false;
            }
            data = static_cast<uint8_t*>(heap_caps_malloc(size, MALLOC_CAP_SPIRAM));
            if (data == nullptr) {
                ESP_LOGE(TAG, "Failed to allocate receive buffer: %zu bytes", size);
                return false;
            }
            capacity = size;
            expected = size;
            received = 0;
            return true;
        }

        bool append(const uint8_t* src, size_t len) {
            if (data == nullptr || received + len > capacity) {
                ESP_LOGE(TAG, "Buffer overflow: received=%zu + len=%zu > capacity=%zu",
                    received, len, capacity);
                return false;
            }
            std::memcpy(data + received, src, len);
            received += len;
            return true;
        }

        bool is_complete() const {
            return data != nullptr && received >= expected && expected > 0;
        }

        // Transfer ownership of buffer to caller
        uint8_t* release() {
            uint8_t* ptr = data;
            data = nullptr;
            capacity = 0;
            received = 0;
            expected = 0;
            return ptr;
        }
    };

    // Socket state
    struct SocketState {
        // FreeRTOS handles
        TaskHandle_t task = nullptr;
        QueueHandle_t outbox = nullptr;  // Messages to send
        QueueHandle_t inbox = nullptr;   // Messages received
        SemaphoreHandle_t mutex = nullptr;

        // WebSocket client
        esp_websocket_client_handle_t client = nullptr;
        ConnectionState state = ConnectionState::WaitingForNetwork;

        // Receive buffer (protected by event handler context)
        ReceiveBuffer rx_buf;

        // Device claim state
        bool needs_claimed = false;
        int64_t last_claim_attempt_ms = 0;

        // Certificate (cached)
        char* cert = nullptr;
        size_t cert_len = 0;
        esp_ds_data_ctx_t* ds_ctx = nullptr;

        bool is_connected() const {
            return state == ConnectionState::Connected &&
                client != nullptr &&
                esp_websocket_client_is_connected(client);
        }

        void cleanup_cert() {
            if (cert != nullptr) {
                free(cert);
                cert = nullptr;
                cert_len = 0;
                ds_ctx = nullptr;
            }
        }
    };

    SocketState sock;

    // Forward declarations
    void process_inbox_message(const uint8_t* data, size_t len);
    void attempt_device_claim();
    void upload_coredump_if_present();

    //------------------------------------------------------------------------------
    // Message Handlers
    //------------------------------------------------------------------------------

    void handle_schedule_response(Kd__V1__MatrxSchedule* response) {
        if (response == nullptr || response->n_schedule_items == 0) {
            ESP_LOGI(TAG, "Received empty schedule");
            scheduler_clear();
            show_fs_sprite("ready");
            return;
        }

        ESP_LOGI(TAG, "Received schedule with %zu items", response->n_schedule_items);
        scheduler_set_schedule(response);
        scheduler_start();
    }

    void handle_render_response(Kd__V1__MatrxSpriteData* response) {
        if (response == nullptr ||
            response->sprite_uuid.data == nullptr ||
            response->sprite_uuid.len != UUID_SIZE_BYTES) {
            ESP_LOGW(TAG, "Invalid render response");
            return;
        }

        scheduler_handle_render_response(
            response->sprite_uuid.data,
            response->sprite_data.data,
            response->sprite_data.len,
            response->render_error
        );
    }

    void handle_device_config_request() {
        system_config_t cfg = config_get_system_config();

        Kd__V1__DeviceConfig device_config = KD__V1__DEVICE_CONFIG__INIT;
        device_config.screen_enabled = cfg.screen_enabled;
        device_config.screen_brightness = cfg.screen_brightness;
        device_config.auto_brightness_enabled = cfg.auto_brightness_enabled;
        device_config.screen_off_lux = cfg.screen_off_lux;

        Kd__V1__MatrxMessage message = KD__V1__MATRX_MESSAGE__INIT;
        message.message_case = KD__V1__MATRX_MESSAGE__MESSAGE_DEVICE_CONFIG;
        message.device_config = &device_config;

        // Pack and queue
        size_t len = kd__v1__matrx_message__get_packed_size(&message);
        auto* buf = static_cast<uint8_t*>(heap_caps_malloc(len, MALLOC_CAP_SPIRAM));
        if (buf != nullptr) {
            kd__v1__matrx_message__pack(&message, buf);
            QueuedMessage msg = { buf, len };
            if (xQueueSend(sock.outbox, &msg, 0) != pdTRUE) {
                free(buf);
            }
        }
    }

    void handle_device_config(const Kd__V1__DeviceConfig* device_config) {
        if (device_config == nullptr) return;

        system_config_t new_cfg = config_get_system_config();
        new_cfg.screen_enabled = device_config->screen_enabled;
        if (device_config->screen_brightness <= 255) {
            new_cfg.screen_brightness = static_cast<uint8_t>(device_config->screen_brightness);
        }
        new_cfg.auto_brightness_enabled = device_config->auto_brightness_enabled;
        if (device_config->screen_off_lux <= 65535) {
            new_cfg.screen_off_lux = static_cast<uint16_t>(device_config->screen_off_lux);
        }

        config_update_system_config(&new_cfg, true, true, true, true);
        ESP_LOGI(TAG, "Applied device config from server");
    }

    void handle_join_response(Kd__V1__JoinResponse* response) {
        if (response == nullptr) return;

        ESP_LOGI(TAG, "Join response: claimed=%d, needs_claimed=%d",
            response->is_claimed, response->needs_claimed);

        sockets_send_device_info();

        sock.needs_claimed = response->needs_claimed || !response->is_claimed;

        if (!sock.needs_claimed) {
            kd_common_clear_claim_token();
        }
        else {
            attempt_device_claim();
        }
    }

    void process_inbox_message(const uint8_t* data, size_t len) {
        auto* message = kd__v1__matrx_message__unpack(nullptr, len, data);
        if (message == nullptr) {
            ESP_LOGE(TAG, "Failed to unpack message (%zu bytes)", len);
            return;
        }

        switch (message->message_case) {
        case KD__V1__MATRX_MESSAGE__MESSAGE_SCHEDULE:
            handle_schedule_response(message->schedule);
            break;
        case KD__V1__MATRX_MESSAGE__MESSAGE_SPRITE_DATA:
            handle_render_response(message->sprite_data);
            break;
        case KD__V1__MATRX_MESSAGE__MESSAGE_DEVICE_CONFIG_REQUEST:
            handle_device_config_request();
            break;
        case KD__V1__MATRX_MESSAGE__MESSAGE_DEVICE_CONFIG:
            handle_device_config(message->device_config);
            break;
        case KD__V1__MATRX_MESSAGE__MESSAGE_JOIN_RESPONSE:
            handle_join_response(message->join_response);
            break;
        default:
            ESP_LOGD(TAG, "Unhandled message type: %d", message->message_case);
            break;
        }

        kd__v1__matrx_message__free_unpacked(message, nullptr);
    }

    //------------------------------------------------------------------------------
    // Outbound Message Helpers
    //------------------------------------------------------------------------------

    bool queue_outbound_message(const Kd__V1__MatrxMessage* message) {
        if (message == nullptr) return false;

        size_t len = kd__v1__matrx_message__get_packed_size(message);
        auto* buf = static_cast<uint8_t*>(heap_caps_malloc(len, MALLOC_CAP_SPIRAM));
        if (buf == nullptr) {
            ESP_LOGE(TAG, "Failed to allocate outbound message buffer");
            return false;
        }

        kd__v1__matrx_message__pack(message, buf);

        QueuedMessage msg = { buf, len };
        if (xQueueSend(sock.outbox, &msg, pdMS_TO_TICKS(100)) != pdTRUE) {
            ESP_LOGW(TAG, "Outbox full, dropping message");
            free(buf);
            return false;
        }

        return true;
    }

    void attempt_device_claim() {
        if (!sock.needs_claimed || !sock.is_connected()) return;

        int64_t now_ms = esp_timer_get_time() / 1000;
        if (sock.last_claim_attempt_ms > 0 &&
            (now_ms - sock.last_claim_attempt_ms) < CLAIM_RETRY_INTERVAL_MS) {
            return;
        }

        auto* token = static_cast<uint8_t*>(heap_caps_malloc(CLAIM_TOKEN_MAX_LEN, MALLOC_CAP_SPIRAM));
        if (token == nullptr) {
            ESP_LOGE(TAG, "Failed to allocate claim token buffer");
            return;
        }

        size_t token_len = CLAIM_TOKEN_MAX_LEN;
        esp_err_t err = kd_common_get_claim_token(reinterpret_cast<char*>(token), &token_len);
        if (err != ESP_OK || token_len == 0) {
            free(token);
            return;
        }

        Kd__V1__ClaimDevice claim = KD__V1__CLAIM_DEVICE__INIT;
        claim.claim_token.data = token;
        claim.claim_token.len = token_len;

        Kd__V1__MatrxMessage message = KD__V1__MATRX_MESSAGE__INIT;
        message.message_case = KD__V1__MATRX_MESSAGE__MESSAGE_CLAIM_DEVICE;
        message.claim_device = &claim;

        queue_outbound_message(&message);
        sock.last_claim_attempt_ms = now_ms;

        free(token);
        ESP_LOGI(TAG, "Sent device claim request");
    }

    //------------------------------------------------------------------------------
    // WebSocket Event Handler
    //------------------------------------------------------------------------------

    void websocket_event_handler(void*, esp_event_base_t, int32_t event_id, void* event_data) {
        auto* data = static_cast<esp_websocket_event_data_t*>(event_data);

        switch (event_id) {
        case WEBSOCKET_EVENT_CONNECTED:
            ESP_LOGI(TAG, "WebSocket connected");
            sock.state = ConnectionState::Connected;
            show_fs_sprite("ready");

            // Request schedule immediately
            request_schedule();

            // Upload coredump if present
            upload_coredump_if_present();
            break;

        case WEBSOCKET_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "WebSocket disconnected (transport)");
            sock.state = ConnectionState::Disconnected;
            sock.rx_buf.reset();
            // Drain inbox to clear any partial/corrupt messages
            {
                QueuedMessage msg;
                while (xQueueReceive(sock.inbox, &msg, 0) == pdTRUE) {
                    if (msg.data != nullptr) free(msg.data);
                }
            }
            show_fs_sprite("connecting");
            break;

        case WEBSOCKET_EVENT_CLOSED:
            ESP_LOGI(TAG, "WebSocket closed (clean)");
            sock.state = ConnectionState::Disconnected;
            sock.rx_buf.reset();
            show_fs_sprite("connecting");
            break;

        case WEBSOCKET_EVENT_ERROR:
            ESP_LOGE(TAG, "WebSocket error");
            sock.state = ConnectionState::Disconnected;
            sock.rx_buf.reset();
            break;

        case WEBSOCKET_EVENT_DATA:
            // Handle incoming data (may be fragmented)
            // Ignore control frames and empty messages
            if (data->op_code == 0x08 || data->op_code == 0x09 || data->op_code == 0x0A) {
                break;
            }
            if (data->payload_len == 0 || data->data_len == 0 || data->data_ptr == nullptr) {
                break;
            }

            // First fragment of a new message
            if (data->payload_offset == 0) {
                // Reset any stale buffer before allocating new one
                sock.rx_buf.reset();
                if (!sock.rx_buf.allocate(data->payload_len)) {
                    ESP_LOGE(TAG, "Failed to allocate buffer for %d byte message", data->payload_len);
                    break;
                }
            }

            // Ensure we have a valid buffer before appending
            if (sock.rx_buf.data == nullptr) {
                ESP_LOGW(TAG, "Received data fragment without buffer (offset=%d)", data->payload_offset);
                break;
            }

            // Append data to buffer
            if (!sock.rx_buf.append(reinterpret_cast<const uint8_t*>(data->data_ptr), data->data_len)) {
                ESP_LOGE(TAG, "Failed to append data to receive buffer");
                sock.rx_buf.reset();
                break;
            }

            // Check if message is complete
            if (sock.rx_buf.is_complete()) {
                size_t msg_len = sock.rx_buf.received;
                uint8_t* msg_data = sock.rx_buf.release();
                QueuedMessage msg = { msg_data, msg_len };

                if (xQueueSend(sock.inbox, &msg, 0) != pdTRUE) {
                    ESP_LOGE(TAG, "Inbox full, dropping message");
                    free(msg_data);
                }
            }
            break;

        default:
            ESP_LOGD(TAG, "WebSocket event: %ld", event_id);
            break;
        }
    }

    //------------------------------------------------------------------------------
    // WiFi Event Handler
    //------------------------------------------------------------------------------

    void wifi_event_handler(void*, esp_event_base_t event_base, int32_t event_id, void*) {
        if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
            ESP_LOGI(TAG, "WiFi disconnected");
            sock.state = ConnectionState::WaitingForNetwork;
        }
        else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
            ESP_LOGI(TAG, "WiFi connected, got IP");
            if (sock.state == ConnectionState::WaitingForNetwork) {
                sock.state = ConnectionState::WaitingForCrypto;
            }
        }
    }

    //------------------------------------------------------------------------------
    // Client Initialization
    //------------------------------------------------------------------------------

    esp_err_t init_websocket_client() {
        if (sock.client != nullptr) {
            esp_websocket_client_destroy(sock.client);
            sock.client = nullptr;
        }

        // Load certificate if not cached
        if (sock.cert == nullptr) {
            sock.ds_ctx = kd_common_crypto_get_ctx();
            sock.cert = static_cast<char*>(heap_caps_calloc(4096, sizeof(char), MALLOC_CAP_SPIRAM));
            if (sock.cert == nullptr) {
                ESP_LOGE(TAG, "Failed to allocate certificate buffer");
                return ESP_ERR_NO_MEM;
            }

            sock.cert_len = 4096;
            esp_err_t ret = kd_common_get_device_cert(sock.cert, &sock.cert_len);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "Failed to get device certificate: %s", esp_err_to_name(ret));
                sock.cleanup_cert();
                return ret;
            }
            ESP_LOGI(TAG, "Loaded device certificate (%zu bytes)", sock.cert_len);
        }

        esp_websocket_client_config_t config = {};
        // TODO: Switch to production URI with mTLS
        config.uri = "ws://192.168.0.206";
        config.port = 9091;
        // config.client_cert = sock.cert;
        // config.client_cert_len = sock.cert_len + 1;
        // config.client_ds_data = sock.ds_ctx;
        // config.crt_bundle_attach = esp_crt_bundle_attach;
        config.reconnect_timeout_ms = RECONNECT_TIMEOUT_MS;
        config.network_timeout_ms = 10000;
        config.ping_interval_sec = PING_INTERVAL_SEC;
        config.disable_auto_reconnect = false;
        config.buffer_size = 4096;

        sock.client = esp_websocket_client_init(&config);
        if (sock.client == nullptr) {
            ESP_LOGE(TAG, "Failed to init websocket client");
            return ESP_FAIL;
        }

        esp_err_t ret = esp_websocket_register_events(sock.client, WEBSOCKET_EVENT_ANY,
            websocket_event_handler, nullptr);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to register websocket events: %s", esp_err_to_name(ret));
            esp_websocket_client_destroy(sock.client);
            sock.client = nullptr;
            return ret;
        }

        return ESP_OK;
    }

    //------------------------------------------------------------------------------
    // Coredump Upload
    //------------------------------------------------------------------------------

    void upload_coredump_if_present() {
        const esp_partition_t* partition = esp_partition_find_first(
            ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_COREDUMP, "coredump");

        if (partition == nullptr) return;

        size_t size = partition->size;
        auto* data = static_cast<uint8_t*>(heap_caps_malloc(size, MALLOC_CAP_SPIRAM));
        if (data == nullptr) {
            ESP_LOGE(TAG, "Failed to allocate memory for coredump");
            return;
        }

        if (esp_partition_read(partition, 0, data, size) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to read coredump");
            free(data);
            return;
        }

        // Check if partition is erased (all 0xFF)
        bool is_erased = true;
        for (size_t i = 0; i < 256 && i < size; i++) {  // Just check first 256 bytes
            if (data[i] != 0xFF) {
                is_erased = false;
                break;
            }
        }

        if (!is_erased) {
            ESP_LOGI(TAG, "Uploading coredump (%zu bytes)", size);

            Kd__V1__UploadCoreDump upload = KD__V1__UPLOAD_CORE_DUMP__INIT;
            upload.core_dump.data = data;
            upload.core_dump.len = size;

            Kd__V1__MatrxMessage message = KD__V1__MATRX_MESSAGE__INIT;
            message.message_case = KD__V1__MATRX_MESSAGE__MESSAGE_UPLOAD_CORE_DUMP;
            message.upload_core_dump = &upload;

            if (queue_outbound_message(&message)) {
                // Erase after queuing
                if (esp_partition_erase_range(partition, 0, size) != ESP_OK) {
                    ESP_LOGE(TAG, "Failed to erase coredump partition");
                }
            }
        }

        free(data);
    }

    //------------------------------------------------------------------------------
    // Main Task
    //------------------------------------------------------------------------------

    void sockets_task(void*) {
        ESP_LOGI(TAG, "Sockets task started");

        // Register WiFi event handlers
        esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED,
            wifi_event_handler, nullptr);
        esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
            wifi_event_handler, nullptr);

        // Check if already connected
        if (kd_common_is_wifi_connected()) {
            sock.state = ConnectionState::WaitingForCrypto;
        }

        while (true) {
            // State machine
            switch (sock.state) {
            case ConnectionState::WaitingForNetwork:
                vTaskDelay(pdMS_TO_TICKS(500));
                continue;

            case ConnectionState::WaitingForCrypto:
                if (kd_common_crypto_get_state() == CryptoState_t::CRYPTO_STATE_VALID_CERT) {
                    ESP_LOGI(TAG, "Crypto ready");
#ifdef ENABLE_OTA
                    sock.state = ConnectionState::WaitingForOTA;
                    show_fs_sprite("check_updates");
#else
                    sock.state = ConnectionState::Connecting;
#endif
                }
                else {
                    vTaskDelay(pdMS_TO_TICKS(500));
                }
                continue;

            case ConnectionState::WaitingForOTA:
#ifdef ENABLE_OTA
                if (kd_common_ota_has_completed_boot_check()) {
                    ESP_LOGI(TAG, "OTA check complete");
                    sock.state = ConnectionState::Connecting;
                }
                else {
                    vTaskDelay(pdMS_TO_TICKS(500));
                }
#else
                sock.state = ConnectionState::Connecting;
#endif
                continue;

            case ConnectionState::Connecting:
                show_fs_sprite("connecting");
                if (init_websocket_client() == ESP_OK) {
                    esp_err_t ret = esp_websocket_client_start(sock.client);
                    if (ret == ESP_OK) {
                        ESP_LOGI(TAG, "WebSocket client started");
                        // State will be updated by event handler
                        sock.state = ConnectionState::Disconnected;  // Waiting for CONNECTED event
                    }
                    else {
                        ESP_LOGE(TAG, "Failed to start websocket client: %s", esp_err_to_name(ret));
                        vTaskDelay(pdMS_TO_TICKS(RECONNECT_TIMEOUT_MS));
                    }
                }
                else {
                    vTaskDelay(pdMS_TO_TICKS(RECONNECT_TIMEOUT_MS));
                }
                continue;

            case ConnectionState::Connected:
            case ConnectionState::Disconnected:
                // Normal operation - process queues
                break;
            }

            // Process outbound messages
            QueuedMessage out_msg;
            while (xQueueReceive(sock.outbox, &out_msg, 0) == pdTRUE) {
                if (out_msg.data == nullptr) continue;

                if (sock.is_connected()) {
                    int ret = esp_websocket_client_send_bin(
                        sock.client,
                        reinterpret_cast<const char*>(out_msg.data),
                        out_msg.len,
                        pdMS_TO_TICKS(SEND_TIMEOUT_MS)
                    );
                    if (ret < 0) {
                        ESP_LOGE(TAG, "Failed to send message (%zu bytes)", out_msg.len);
                    }
                }
                free(out_msg.data);
            }

            // Process inbound messages
            QueuedMessage in_msg;
            while (xQueueReceive(sock.inbox, &in_msg, 0) == pdTRUE) {
                if (in_msg.data != nullptr) {
                    process_inbox_message(in_msg.data, in_msg.len);
                    free(in_msg.data);
                }
            }

            // Periodic tasks (when connected)
            static TickType_t last_periodic = 0;
            TickType_t now = xTaskGetTickCount();
            if (now - last_periodic >= pdMS_TO_TICKS(5000)) {
                last_periodic = now;

                if (sock.is_connected()) {
                    attempt_device_claim();
                }
            }

            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }

}  // namespace

//------------------------------------------------------------------------------
// Public API
//------------------------------------------------------------------------------

void sockets_init() {
    sock.mutex = xSemaphoreCreateMutex();
    if (sock.mutex == nullptr) {
        ESP_LOGE(TAG, "Failed to create mutex");
        return;
    }

    sock.outbox = xQueueCreate(OUTBOX_QUEUE_SIZE, sizeof(QueuedMessage));
    if (sock.outbox == nullptr) {
        ESP_LOGE(TAG, "Failed to create outbox queue");
        return;
    }

    sock.inbox = xQueueCreate(INBOX_QUEUE_SIZE, sizeof(QueuedMessage));
    if (sock.inbox == nullptr) {
        ESP_LOGE(TAG, "Failed to create inbox queue");
        return;
    }

    BaseType_t ret = xTaskCreatePinnedToCore(
        sockets_task, "sockets", 4096, nullptr, 5, &sock.task, 1);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create sockets task");
    }

    ESP_LOGI(TAG, "Sockets initialized");
}

void sockets_deinit() {
    if (sock.client != nullptr) {
        esp_websocket_client_stop(sock.client);
        esp_websocket_client_destroy(sock.client);
        sock.client = nullptr;
    }

    sock.cleanup_cert();
    sock.rx_buf.reset();

    if (sock.task != nullptr) {
        vTaskDelete(sock.task);
        sock.task = nullptr;
    }

    // Drain and delete queues
    QueuedMessage msg;
    if (sock.outbox != nullptr) {
        while (xQueueReceive(sock.outbox, &msg, 0) == pdTRUE) {
            free(msg.data);
        }
        vQueueDelete(sock.outbox);
        sock.outbox = nullptr;
    }
    if (sock.inbox != nullptr) {
        while (xQueueReceive(sock.inbox, &msg, 0) == pdTRUE) {
            free(msg.data);
        }
        vQueueDelete(sock.inbox);
        sock.inbox = nullptr;
    }

    if (sock.mutex != nullptr) {
        vSemaphoreDelete(sock.mutex);
        sock.mutex = nullptr;
    }
}

bool sockets_is_connected() {
    return sock.is_connected();
}

void request_schedule() {
    Kd__V1__ScheduleRequest request = KD__V1__SCHEDULE_REQUEST__INIT;

    Kd__V1__MatrxMessage message = KD__V1__MATRX_MESSAGE__INIT;
    message.message_case = KD__V1__MATRX_MESSAGE__MESSAGE_SCHEDULE_REQUEST;
    message.schedule_request = &request;

    queue_outbound_message(&message);
}

void sockets_send_currently_displaying(uint8_t* uuid) {
    if (uuid == nullptr) return;

    Kd__V1__CurrentlyDisplayingUpdate update = KD__V1__CURRENTLY_DISPLAYING_UPDATE__INIT;
    update.uuid.data = uuid;
    update.uuid.len = UUID_SIZE_BYTES;

    Kd__V1__MatrxMessage message = KD__V1__MATRX_MESSAGE__INIT;
    message.message_case = KD__V1__MATRX_MESSAGE__MESSAGE_CURRENTLY_DISPLAYING_UPDATE;
    message.currently_displaying_update = &update;

    queue_outbound_message(&message);
}

void sockets_send_device_info() {
    Kd__V1__DeviceInfo info = KD__V1__DEVICE_INFO__INIT;
    info.width = CONFIG_MATRIX_WIDTH;
    info.height = CONFIG_MATRIX_HEIGHT;
    info.has_light_sensor = true;

    Kd__V1__MatrxMessage message = KD__V1__MATRX_MESSAGE__INIT;
    message.message_case = KD__V1__MATRX_MESSAGE__MESSAGE_DEVICE_INFO;
    message.device_info = &info;

    queue_outbound_message(&message);
}

void sockets_send_device_config() {
    handle_device_config_request();
}

void sockets_send_modify_schedule_item(const uint8_t* uuid, bool pinned, bool skipped) {
    if (uuid == nullptr) return;

    Kd__V1__ModifyScheduleItem modify = KD__V1__MODIFY_SCHEDULE_ITEM__INIT;
    modify.uuid.data = const_cast<uint8_t*>(uuid);
    modify.uuid.len = UUID_SIZE_BYTES;
    modify.user_pinned = pinned;
    modify.user_skipped = skipped;

    Kd__V1__MatrxMessage message = KD__V1__MATRX_MESSAGE__INIT;
    message.message_case = KD__V1__MATRX_MESSAGE__MESSAGE_MODIFY_SCHEDULE_ITEM;
    message.modify_schedule_item = &modify;

    queue_outbound_message(&message);
}

void send_render_request_to_server(const uint8_t* uuid) {
    if (uuid == nullptr) return;

    Kd__V1__SpriteRenderRequest request = KD__V1__SPRITE_RENDER_REQUEST__INIT;
    request.sprite_uuid.data = const_cast<uint8_t*>(uuid);
    request.sprite_uuid.len = UUID_SIZE_BYTES;
    request.device_width = CONFIG_MATRIX_WIDTH;
    request.device_height = CONFIG_MATRIX_HEIGHT;

    Kd__V1__MatrxMessage message = KD__V1__MATRX_MESSAGE__INIT;
    message.message_case = KD__V1__MATRX_MESSAGE__MESSAGE_SPRITE_RENDER_REQUEST;
    message.sprite_render_request = &request;

    queue_outbound_message(&message);
}

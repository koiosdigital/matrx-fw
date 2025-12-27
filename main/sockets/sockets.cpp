#include "sockets.h"

#include <esp_log.h>
#include <esp_websocket_client.h>
#include <esp_event.h>
#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <esp_wifi.h>
#include <esp_timer.h>
#include <esp_system.h>
#include <esp_partition.h>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <kd_common.h>
#include "display.h"
#include "scheduler.h"
#include "sprites.h"
#include "config.h"
#include "../render_requests/render_requests.h"

#include <kd/v1/matrx.pb-c.h>

#include <cstring>
#include <memory>

static const char* TAG = "sockets";

// Forward declaration (definition after anonymous namespace)
void send_socket_message(const Kd__V1__MatrxMessage* message);

namespace {

    // Constants (UUID_SIZE_BYTES is defined in scheduler.h)
    constexpr int64_t CONNECTION_WATCHDOG_TIMEOUT_MS = 60000;
    constexpr uint32_t MAX_SOCKET_CONNECTION_ERRORS = 5;
    constexpr int64_t CLAIM_RETRY_INTERVAL_MS = 5000;
    constexpr size_t CLAIM_TOKEN_MAX_LEN = 2048;

    // Task notification types
    enum class SocketNotification : uint32_t {
        Reinit = 1,
        Reconnect = 2,
    };

    // Message for queue processing
    struct ProcessableMessage {
        char* message = nullptr;
        size_t message_len = 0;
        bool is_outbox = false;
    };

    // Encapsulated socket state
    struct SocketState {
        TaskHandle_t task = nullptr;
        QueueHandle_t queue = nullptr;
        esp_websocket_client_handle_t client = nullptr;

        // Render request deduplication
        uint8_t last_render_uuid[UUID_SIZE_BYTES] = {};
        bool has_last_render = false;

        // Connection state
        int64_t last_activity_ms = 0;
        bool connectable = false;
        uint32_t error_count = 0;

        // Claim flow state
        bool needs_claimed = false;
        int64_t last_claim_attempt_ms = 0;

        // Certificate data (cached for reuse)
        char* cert = nullptr;
        size_t cert_len = 0;
        esp_ds_data_ctx_t* ds_ctx = nullptr;

        bool is_duplicate_render(const uint8_t* uuid) const {
            if (uuid == nullptr || !has_last_render) return false;
            return std::memcmp(last_render_uuid, uuid, UUID_SIZE_BYTES) == 0;
        }

        void track_render(const uint8_t* uuid) {
            if (uuid == nullptr) return;
            std::memcpy(last_render_uuid, uuid, UUID_SIZE_BYTES);
            has_last_render = true;
        }

        void clear_render_tracking(const uint8_t* uuid) {
            if (uuid == nullptr) return;
            if (has_last_render && std::memcmp(last_render_uuid, uuid, UUID_SIZE_BYTES) == 0) {
                has_last_render = false;
                std::memset(last_render_uuid, 0, UUID_SIZE_BYTES);
            }
        }

        void reset_render_tracking() {
            has_last_render = false;
            std::memset(last_render_uuid, 0, UUID_SIZE_BYTES);
        }

        void cleanup_cert() {
            if (cert != nullptr) {
                free(cert);
                cert = nullptr;
                cert_len = 0;
                ds_ctx = nullptr;
            }
        }

        void disconnect_wifi_after_errors(const char* context) {
            ESP_LOGE(TAG, "Max socket errors (%lu) during %s, disconnecting WiFi",
                MAX_SOCKET_CONNECTION_ERRORS, context);
            connectable = false;

            esp_err_t err = esp_wifi_disconnect();
            if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_INIT && err != ESP_ERR_WIFI_NOT_STARTED) {
                ESP_LOGW(TAG, "Failed to disconnect WiFi: %s", esp_err_to_name(err));
            }
            error_count = 0;
        }

        void clear_queue() {
            if (queue == nullptr) return;

            ProcessableMessage msg;
            size_t cleared = 0;
            while (xQueueReceive(queue, &msg, 0) == pdTRUE) {
                if (msg.message != nullptr) {
                    free(msg.message);
                }
                cleared++;
            }
            if (cleared > 0) {
                ESP_LOGI(TAG, "Cleared %zu queued messages on disconnect", cleared);
            }
        }
    };

    SocketState sock;

    // Forward declarations (send_socket_message is defined outside this namespace)
    esp_err_t socket_init();
    void socket_deinit();

    void attempt_device_claim() {
        if (!sock.needs_claimed || !sock.connectable) return;
        if (sock.client == nullptr || !esp_websocket_client_is_connected(sock.client)) return;

        int64_t now_ms = esp_timer_get_time() / 1000;
        if (sock.last_claim_attempt_ms > 0 &&
            (now_ms - sock.last_claim_attempt_ms) < CLAIM_RETRY_INTERVAL_MS) {
            return;
        }

        auto token = static_cast<uint8_t*>(heap_caps_malloc(CLAIM_TOKEN_MAX_LEN, MALLOC_CAP_SPIRAM));
        if (token == nullptr) {
            ESP_LOGE(TAG, "no mem for claim token");
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

        send_socket_message(&message);
        sock.last_claim_attempt_ms = now_ms;

        free(token);
    }

    void websocket_event_handler(void*, esp_event_base_t, int32_t event_id, void* event_data) {
        auto* data = static_cast<esp_websocket_event_data_t*>(event_data);
        static char* dbuf = nullptr;

        switch (event_id) {
        case WEBSOCKET_EVENT_CONNECTED:
            sock.last_activity_ms = esp_timer_get_time() / 1000;
            sock.error_count = 0;
            show_fs_sprite("ready");
            request_schedule();
            attempt_coredump_upload();
            break;

        case WEBSOCKET_EVENT_DISCONNECTED:
            if (dbuf) {
                free(dbuf);
                dbuf = nullptr;
            }
            sock.reset_render_tracking();
            sock.clear_queue();
            show_fs_sprite("connecting");
            if (sock.task != nullptr) {
                xTaskNotify(sock.task, static_cast<uint32_t>(SocketNotification::Reconnect),
                    eSetValueWithOverwrite);
            }
            break;

        case WEBSOCKET_EVENT_ERROR:
            if (dbuf) {
                free(dbuf);
                dbuf = nullptr;
            }
            sock.reset_render_tracking();
            sock.clear_queue();

            if (sock.connectable) {
                sock.error_count++;
                if (sock.error_count >= MAX_SOCKET_CONNECTION_ERRORS) {
                    sock.disconnect_wifi_after_errors("websocket error event");
                }
            }

            if (sock.task != nullptr) {
                xTaskNotify(sock.task, static_cast<uint32_t>(SocketNotification::Reinit),
                    eSetValueWithOverwrite);
            }
            break;

        case WEBSOCKET_EVENT_DATA:
            sock.last_activity_ms = esp_timer_get_time() / 1000;

            if (data->payload_offset == 0) {
                if (dbuf) {
                    free(dbuf);
                    dbuf = nullptr;
                }
                dbuf = static_cast<char*>(
                    heap_caps_calloc(data->payload_len + 1, sizeof(char), MALLOC_CAP_SPIRAM));
                if (dbuf == nullptr) {
                    ESP_LOGE(TAG, "malloc failed: dbuf (%d)", data->payload_len);
                    return;
                }
            }

            if (dbuf) {
                std::memcpy(dbuf + data->payload_offset, data->data_ptr, data->data_len);
            }

            if (data->payload_offset + data->data_len >= data->payload_len) {
                ProcessableMessage msg = { dbuf, static_cast<size_t>(data->payload_len), false };
                if (xQueueSend(sock.queue, &msg, pdMS_TO_TICKS(100)) != pdTRUE) {
                    ESP_LOGE(TAG, "Queue full, dropping message (len=%d)", data->payload_len);
                    free(dbuf);
                }
                dbuf = nullptr;
            }
            break;

        default:
            ESP_LOGD(TAG, "Unhandled websocket event: %ld", event_id);
            break;
        }
    }

    esp_err_t socket_init() {
        if (sock.client != nullptr) {
            socket_deinit();
        }

        // Initialize certificate data if not already done
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
        }
        else {
            ESP_LOGV(TAG, "Using cached certificate data");
        }

        esp_websocket_client_config_t websocket_cfg = {
            .uri = "ws://192.168.0.215",
            .port = 9091,
            //.client_cert = sock.cert,
            //.client_cert_len = sock.cert_len + 1,
            //.client_ds_data = sock.ds_ctx,
            //.crt_bundle_attach = esp_crt_bundle_attach,
            .reconnect_timeout_ms = 1000,
            .network_timeout_ms = 5000,
            .ping_interval_sec = 10,
        };

        sock.client = esp_websocket_client_init(&websocket_cfg);
        if (sock.client == nullptr) {
            ESP_LOGE(TAG, "Failed to initialize websocket client");
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

    void socket_deinit() {
        if (sock.client != nullptr) {
            esp_websocket_client_stop(sock.client);
            esp_websocket_client_destroy(sock.client);
            sock.client = nullptr;
            sock.reset_render_tracking();
        }
    }

    void send_render_request_internal(const uint8_t* uuid) {
        if (uuid == nullptr) return;

        Kd__V1__SpriteRenderRequest request = KD__V1__SPRITE_RENDER_REQUEST__INIT;
        request.sprite_uuid.data = const_cast<uint8_t*>(uuid);  // protobuf-c doesn't use const
        request.sprite_uuid.len = UUID_SIZE_BYTES;
        request.device_width = CONFIG_MATRIX_WIDTH;
        request.device_height = CONFIG_MATRIX_HEIGHT;

        Kd__V1__MatrxMessage message = KD__V1__MATRX_MESSAGE__INIT;
        message.message_case = KD__V1__MATRX_MESSAGE__MESSAGE_SPRITE_RENDER_REQUEST;
        message.sprite_render_request = &request;

        send_socket_message(&message);
    }

    void handle_schedule_response(Kd__V1__MatrxSchedule* response) {
        ESP_LOGV(TAG, "Handling schedule response");
        if (response == nullptr || response->n_schedule_items == 0) {
            scheduler_clear();
            show_fs_sprite("ready");
            return;
        }

        scheduler_set_schedule(response);
        scheduler_start();
    }

    void handle_render_response(Kd__V1__MatrxSpriteData* response) {
        if (response == nullptr || response->sprite_uuid.data == nullptr ||
            response->sprite_uuid.len != UUID_SIZE_BYTES) {
            return;
        }

        // Clear socket-level tracking
        sock.clear_render_tracking(response->sprite_uuid.data);

        // Delegate to render_requests module for validation and state management
        render_response_received(
            response->sprite_uuid.data,
            response->sprite_data.data,
            response->sprite_data.len,
            response->error
        );
    }

    void handle_modify_schedule_item(Kd__V1__ModifyScheduleItem*) {
        // TODO: Implement schedule item modification
    }

    void send_device_config_internal() {
        system_config_t cfg = config_get_system_config();

        Kd__V1__DeviceConfig device_config = KD__V1__DEVICE_CONFIG__INIT;
        device_config.screen_enabled = cfg.screen_enabled;
        device_config.screen_brightness = cfg.screen_brightness;
        device_config.auto_brightness_enabled = cfg.auto_brightness_enabled;
        device_config.screen_off_lux = cfg.screen_off_lux;

        Kd__V1__MatrxMessage message = KD__V1__MATRX_MESSAGE__INIT;
        message.message_case = KD__V1__MATRX_MESSAGE__MESSAGE_DEVICE_CONFIG;
        message.device_config = &device_config;

        send_socket_message(&message);
    }

    void apply_device_config(const Kd__V1__DeviceConfig* device_config) {
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
    }

    void handle_matrx_message(Kd__V1__MatrxMessage* message) {
        if (message == nullptr) return;

        switch (message->message_case) {
        case KD__V1__MATRX_MESSAGE__MESSAGE_SCHEDULE:
            handle_schedule_response(message->schedule);
            break;
        case KD__V1__MATRX_MESSAGE__MESSAGE_SPRITE_DATA:
            handle_render_response(message->sprite_data);
            break;
        case KD__V1__MATRX_MESSAGE__MESSAGE_MODIFY_SCHEDULE_ITEM:
            handle_modify_schedule_item(message->modify_schedule_item);
            break;
        case KD__V1__MATRX_MESSAGE__MESSAGE_DEVICE_CONFIG_REQUEST:
            send_device_config_internal();
            break;
        case KD__V1__MATRX_MESSAGE__MESSAGE_DEVICE_CONFIG:
            apply_device_config(message->device_config);
            break;
        case KD__V1__MATRX_MESSAGE__MESSAGE_JOIN_RESPONSE:
            if (message->join_response == nullptr) break;

            sockets_send_device_info();

            sock.needs_claimed = message->join_response->needs_claimed ||
                !message->join_response->is_claimed;

            if (!sock.needs_claimed) {
                kd_common_clear_claim_token();
            }
            else {
                attempt_device_claim();
            }
            break;
        default:
            break;
        }
    }

    void wifi_ip_event_handler(void*, esp_event_base_t event_base, int32_t event_id, void*) {
        if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
            sock.connectable = false;
            sockets_disconnect();
        }
        else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
            sock.connectable = true;
            sock.error_count = 0;
            sockets_connect();
        }
    }

    void sockets_task_func(void*) {
        constexpr uint32_t MAX_RECONNECT_DELAY_MS = 30000;
        uint32_t reconnect_delay_ms = 1000;

        // Wait for crypto to be ready
        while (kd_common_crypto_get_state() != CryptoState_t::CRYPTO_STATE_VALID_CERT) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }

        // Register event handlers early
        esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED,
            wifi_ip_event_handler, nullptr);
        esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
            wifi_ip_event_handler, nullptr);

        // Check if WiFi is already connected
        if (kd_common_is_wifi_connected()) {
            sock.connectable = true;
            sock.error_count = 0;
        }

#ifdef ENABLE_OTA
        // Wait for OTA boot check
        bool shown_check_updates = false;
        while (!kd_common_ota_has_completed_boot_check()) {
            if (sock.connectable && !shown_check_updates) {
                show_fs_sprite("check_updates");
                shown_check_updates = true;
            }
            vTaskDelay(pdMS_TO_TICKS(1000));
        }

        if (sock.connectable) {
            show_fs_sprite("connecting");
        }
#endif

        sock.last_activity_ms = esp_timer_get_time() / 1000;

        // Initialize with retry logic
        while (socket_init() != ESP_OK) {
            if (sock.connectable) {
                sock.error_count++;
                if (sock.error_count >= MAX_SOCKET_CONNECTION_ERRORS) {
                    sock.disconnect_wifi_after_errors("socket init");
                }
            }

            vTaskDelay(pdMS_TO_TICKS(reconnect_delay_ms));
            reconnect_delay_ms = std::min(reconnect_delay_ms * 2, MAX_RECONNECT_DELAY_MS);
        }
        reconnect_delay_ms = 1000;
        sock.error_count = 0;

        ProcessableMessage message;
        TickType_t last_cleanup_time = xTaskGetTickCount();

        while (true) {
            // Check for task notifications
            uint32_t notification_value = 0;
            if (xTaskNotifyWait(0, ULONG_MAX, &notification_value, pdMS_TO_TICKS(100)) == pdTRUE) {
                auto notification = static_cast<SocketNotification>(notification_value);

                if (notification == SocketNotification::Reinit) {
                    socket_deinit();
                    vTaskDelay(pdMS_TO_TICKS(reconnect_delay_ms));

                    while (socket_init() != ESP_OK) {
                        if (sock.connectable) {
                            sock.error_count++;
                            if (sock.error_count >= MAX_SOCKET_CONNECTION_ERRORS) {
                                sock.disconnect_wifi_after_errors("socket reinit");
                            }
                        }
                        vTaskDelay(pdMS_TO_TICKS(reconnect_delay_ms));
                        reconnect_delay_ms = std::min(reconnect_delay_ms * 2, MAX_RECONNECT_DELAY_MS);
                    }
                    reconnect_delay_ms = 1000;
                    sock.error_count = 0;

                }
                else if (notification == SocketNotification::Reconnect) {
                    if (sock.client != nullptr) {
                        sockets_connect();
                        reconnect_delay_ms = 1000;
                    }
                    else {
                        while (socket_init() != ESP_OK) {
                            if (sock.connectable) {
                                sock.error_count++;
                                if (sock.error_count >= MAX_SOCKET_CONNECTION_ERRORS) {
                                    sock.disconnect_wifi_after_errors("socket reconnect init");
                                }
                            }
                            vTaskDelay(pdMS_TO_TICKS(reconnect_delay_ms));
                            reconnect_delay_ms = std::min(reconnect_delay_ms * 2, MAX_RECONNECT_DELAY_MS);
                        }
                        sockets_connect();
                        reconnect_delay_ms = 1000;
                        sock.error_count = 0;
                    }
                }
            }

            // Process all queued messages (drain the queue)
            while (xQueueReceive(sock.queue, &message, pdMS_TO_TICKS(100)) == pdTRUE) {
                if (message.message == nullptr) continue;

                if (message.is_outbox) {
                    if (esp_websocket_client_is_connected(sock.client)) {
                        int ret = esp_websocket_client_send_bin(sock.client, message.message,
                            message.message_len, pdMS_TO_TICKS(10000));
                        if (ret == -1) {
                            ESP_LOGE(TAG, "failed to send websocket message");
                        }
                    }
                    else {
                        ESP_LOGW(TAG, "websocket not connected, dropping message");
                    }
                    free(message.message);
                    continue;
                }

                auto* matrx_message = kd__v1__matrx_message__unpack(
                    nullptr, message.message_len, reinterpret_cast<const uint8_t*>(message.message));
                if (matrx_message == nullptr) {
                    ESP_LOG_BUFFER_HEXDUMP(TAG, message.message, message.message_len, ESP_LOG_ERROR);
                    ESP_LOGE(TAG, "failed to unpack matrx message");
                    free(message.message);
                    continue;
                }

                handle_matrx_message(matrx_message);
                kd__v1__matrx_message__free_unpacked(matrx_message, nullptr);
                free(message.message);
            }

            // Periodic monitoring (every 5 seconds)
            TickType_t current_time = xTaskGetTickCount();
            if (current_time - last_cleanup_time >= pdMS_TO_TICKS(5000)) {
                last_cleanup_time = current_time;

                attempt_device_claim();

                int64_t current_time_ms = esp_timer_get_time() / 1000;

                if (sock.client != nullptr && esp_websocket_client_is_connected(sock.client)) {
                    if (sock.last_activity_ms > 0 &&
                        (current_time_ms - sock.last_activity_ms) > CONNECTION_WATCHDOG_TIMEOUT_MS) {
                        xTaskNotify(sock.task, static_cast<uint32_t>(SocketNotification::Reconnect),
                            eSetValueWithOverwrite);
                    }
                }
                else if (sock.client != nullptr) {
                    sockets_connect();
                }
                else {
                    xTaskNotify(sock.task, static_cast<uint32_t>(SocketNotification::Reinit),
                        eSetValueWithOverwrite);
                }
            }
        }
    }

}  // namespace

//MARK: Public API

void sockets_init() {
    sock.connectable = false;

    sock.queue = xQueueCreate(32, sizeof(ProcessableMessage));
    if (sock.queue == nullptr) {
        ESP_LOGE(TAG, "Failed to create sockets queue");
        return;
    }

    BaseType_t ret = xTaskCreatePinnedToCore(sockets_task_func, "sockets", 4096,
        nullptr, 5, &sock.task, 1);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create sockets task");
    }
}

void sockets_deinit() {
    sock.connectable = false;
    socket_deinit();
    sock.cleanup_cert();

    if (sock.task != nullptr) {
        vTaskDelete(sock.task);
        sock.task = nullptr;
    }

    if (sock.queue != nullptr) {
        vQueueDelete(sock.queue);
        sock.queue = nullptr;
    }

    sock.reset_render_tracking();
}

void sockets_connect() {
    if (!sock.connectable || sock.client == nullptr) {
        return;
    }

    esp_err_t ret = esp_websocket_client_start(sock.client);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Websocket start failed: %s", esp_err_to_name(ret));

        if (sock.connectable) {
            sock.error_count++;
            if (sock.error_count >= MAX_SOCKET_CONNECTION_ERRORS) {
                sock.disconnect_wifi_after_errors("websocket start");
            }
        }
    }
}

void sockets_disconnect() {
    if (sock.client == nullptr) return;
    esp_websocket_client_close(sock.client, pdMS_TO_TICKS(1000));
}

void send_socket_message(const Kd__V1__MatrxMessage* message) {
    if (!sock.connectable || sock.client == nullptr ||
        !esp_websocket_client_is_connected(sock.client) || message == nullptr) {
        return;
    }

    size_t len = kd__v1__matrx_message__get_packed_size(message);
    auto buffer = static_cast<uint8_t*>(heap_caps_calloc(len, sizeof(uint8_t), MALLOC_CAP_SPIRAM));
    if (buffer == nullptr) {
        ESP_LOGE(TAG, "failed to allocate message buffer");
        return;
    }

    kd__v1__matrx_message__pack(message, buffer);

    ProcessableMessage msg = { reinterpret_cast<char*>(buffer), len, true };
    if (xQueueSend(sock.queue, &msg, pdMS_TO_TICKS(50)) != pdTRUE) {
        ESP_LOGE(TAG, "failed to send message to queue");
        free(buffer);
    }
}

void request_render(const uint8_t* uuid) {
    if (uuid == nullptr) return;

    if (sock.is_duplicate_render(uuid)) return;

    sock.track_render(uuid);
    send_render_request_internal(uuid);
}

// Called by render_requests module to send render requests
void send_render_request_to_server(const uint8_t* uuid) {
    if (uuid == nullptr) return;
    send_render_request_internal(uuid);
}

void upload_coredump(uint8_t* core_dump, size_t core_dump_len) {
    if (core_dump == nullptr || core_dump_len == 0) {
        ESP_LOGE(TAG, "upload_coredump called with empty buffer");
        return;
    }

    Kd__V1__UploadCoreDump upload = KD__V1__UPLOAD_CORE_DUMP__INIT;
    upload.core_dump.data = core_dump;
    upload.core_dump.len = core_dump_len;

    Kd__V1__MatrxMessage message = KD__V1__MATRX_MESSAGE__INIT;
    message.message_case = KD__V1__MATRX_MESSAGE__MESSAGE_UPLOAD_CORE_DUMP;
    message.upload_core_dump = &upload;

    send_socket_message(&message);
}

void request_schedule() {
    Kd__V1__ScheduleRequest request = KD__V1__SCHEDULE_REQUEST__INIT;

    Kd__V1__MatrxMessage message = KD__V1__MATRX_MESSAGE__INIT;
    message.message_case = KD__V1__MATRX_MESSAGE__MESSAGE_SCHEDULE_REQUEST;
    message.schedule_request = &request;

    send_socket_message(&message);
}

void sockets_send_currently_displaying(uint8_t* uuid) {
    if (uuid == nullptr) return;

    Kd__V1__CurrentlyDisplayingUpdate update = KD__V1__CURRENTLY_DISPLAYING_UPDATE__INIT;
    update.installation_uuid.data = uuid;
    update.installation_uuid.len = UUID_SIZE_BYTES;

    Kd__V1__MatrxMessage message = KD__V1__MATRX_MESSAGE__INIT;
    message.message_case = KD__V1__MATRX_MESSAGE__MESSAGE_CURRENTLY_DISPLAYING_UPDATE;
    message.currently_displaying_update = &update;

    send_socket_message(&message);
}

void sockets_send_device_info() {
    Kd__V1__DeviceInfo info = KD__V1__DEVICE_INFO__INIT;
    info.width = CONFIG_MATRIX_WIDTH;
    info.height = CONFIG_MATRIX_HEIGHT;
    info.has_light_sensor = true;

    Kd__V1__MatrxMessage message = KD__V1__MATRX_MESSAGE__INIT;
    message.message_case = KD__V1__MATRX_MESSAGE__MESSAGE_DEVICE_INFO;
    message.device_info = &info;

    send_socket_message(&message);
}

void sockets_send_device_config() {
    send_device_config_internal();
}

void sockets_send_modify_schedule_item(const uint8_t* uuid, bool pinned, bool skipped) {
    if (uuid == nullptr) return;

    Kd__V1__ModifyScheduleItem modify = KD__V1__MODIFY_SCHEDULE_ITEM__INIT;
    modify.uuid.data = const_cast<uint8_t*>(uuid);
    modify.uuid.len = UUID_SIZE_BYTES;
    modify.pinned = pinned;
    modify.skipped = skipped;

    Kd__V1__MatrxMessage message = KD__V1__MATRX_MESSAGE__INIT;
    message.message_case = KD__V1__MATRX_MESSAGE__MESSAGE_MODIFY_SCHEDULE_ITEM;
    message.modify_schedule_item = &modify;

    send_socket_message(&message);
}

void attempt_coredump_upload() {
    const esp_partition_t* partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_COREDUMP, "coredump");

    if (partition == nullptr) return;

    size_t size = partition->size;
    auto data = static_cast<uint8_t*>(heap_caps_malloc(size + 1, MALLOC_CAP_SPIRAM));
    if (data == nullptr) {
        ESP_LOGE(TAG, "Failed to allocate memory for core dump");
        return;
    }

    if (esp_partition_read(partition, 0, data, size) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read core dump data");
        free(data);
        return;
    }

    // Check if erased (all 0xFF)
    bool is_erased = true;
    for (size_t i = 0; i < size; i++) {
        if (data[i] != 0xFF) {
            is_erased = false;
            break;
        }
    }

    if (!is_erased) {
        upload_coredump(data, size);

        if (esp_partition_erase_range(partition, 0, size) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to erase core dump partition");
        }
    }

    free(data);
}
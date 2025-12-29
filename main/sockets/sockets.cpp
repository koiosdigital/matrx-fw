#include "sockets.h"
#include "handlers.h"
#include "messages.h"

#include <esp_log.h>
#include <esp_websocket_client.h>
#include <esp_event.h>
#include <esp_wifi.h>
#include <esp_heap_caps.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>

#include <kd_common.h>
#include <kd/v1/matrx.pb-c.h>

#include "apps.h"
#include "scheduler.h"

static const char* TAG = "sockets";

namespace {

    //------------------------------------------------------------------------------
    // Configuration
    //------------------------------------------------------------------------------

    constexpr size_t QUEUE_SIZE = 4;
    constexpr size_t MAX_MSG_SIZE = 100 * 1024;  // 100KB

    constexpr uint32_t TASK_STACK = 4096;
    constexpr UBaseType_t TASK_PRIORITY = 5;

    //------------------------------------------------------------------------------
    // SPIRAM Allocator for Protobuf
    //------------------------------------------------------------------------------

    void* spiram_alloc(void*, size_t size) {
        return heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
    }

    void spiram_free(void*, void* ptr) {
        heap_caps_free(ptr);
    }

    ProtobufCAllocator spiram_allocator = {
        .alloc = spiram_alloc,
        .free = spiram_free,
        .allocator_data = nullptr,
    };

    //------------------------------------------------------------------------------
    // State
    //------------------------------------------------------------------------------

    enum class State : uint8_t {
        WaitingForNetwork,
        WaitingForCrypto,
        WaitingForOTA,
        Ready,
    };

    struct QueuedMessage {
        uint8_t* data;
        size_t len;
    };

    struct {
        TaskHandle_t task = nullptr;
        QueueHandle_t outbox = nullptr;
        QueueHandle_t inbox = nullptr;
        esp_websocket_client_handle_t client = nullptr;
        State state = State::WaitingForNetwork;

        // Receive buffer
        uint8_t* rx_buf = nullptr;
        size_t rx_len = 0;
        size_t rx_expected = 0;

        bool is_connected() const {
            return client != nullptr && esp_websocket_client_is_connected(client);
        }

        void rx_reset() {
            if (rx_buf) { heap_caps_free(rx_buf); rx_buf = nullptr; }
            rx_len = rx_expected = 0;
        }
    } sock;

    //------------------------------------------------------------------------------
    // WebSocket Events
    //------------------------------------------------------------------------------

    void ws_event_handler(void*, esp_event_base_t, int32_t event_id, void* event_data) {
        auto* data = static_cast<esp_websocket_event_data_t*>(event_data);

        switch (event_id) {
        case WEBSOCKET_EVENT_CONNECTED:
            ESP_LOGI(TAG, "Connected");
            show_fs_sprite("ready");
            msg_send_device_info();
            msg_send_schedule_request();
            break;

        case WEBSOCKET_EVENT_DISCONNECTED:
        case WEBSOCKET_EVENT_CLOSED:
        case WEBSOCKET_EVENT_ERROR:
            ESP_LOGW(TAG, "Disconnected/error (event=%ld)", event_id);
            sock.rx_reset();
            scheduler_on_disconnect();
            break;

        case WEBSOCKET_EVENT_DATA:
            // Skip control frames
            if (data->op_code >= 0x08) break;
            if (data->payload_len == 0 || data->data_ptr == nullptr) break;

            // New message
            if (data->payload_offset == 0) {
                sock.rx_reset();
                if (data->payload_len > MAX_MSG_SIZE) {
                    ESP_LOGE(TAG, "Message too large: %d", data->payload_len);
                    break;
                }
                sock.rx_buf = static_cast<uint8_t*>(
                    heap_caps_calloc(data->payload_len, 1, MALLOC_CAP_SPIRAM));
                if (!sock.rx_buf) {
                    ESP_LOGE(TAG, "Alloc failed: %d bytes", data->payload_len);
                    break;
                }
                sock.rx_expected = data->payload_len;
                ESP_LOGI(TAG, "RX: %d bytes", data->payload_len);
            }

            // Append chunk
            if (sock.rx_buf && sock.rx_len + data->data_len <= sock.rx_expected) {
                memcpy(sock.rx_buf + sock.rx_len, data->data_ptr, data->data_len);
                sock.rx_len += data->data_len;
            }

            // Complete?
            if (sock.rx_buf && sock.rx_len >= sock.rx_expected) {
                QueuedMessage msg = { sock.rx_buf, sock.rx_len };
                if (xQueueSend(sock.inbox, &msg, 0) != pdTRUE) {
                    heap_caps_free(sock.rx_buf);
                }
                sock.rx_buf = nullptr;
                sock.rx_len = sock.rx_expected = 0;
            }
            break;

        default:
            break;
        }
    }

    void wifi_event_handler(void*, esp_event_base_t base, int32_t id, void*) {
        if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
            sock.state = State::WaitingForNetwork;
        }
        else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
            if (sock.state == State::WaitingForNetwork) {
                sock.state = State::WaitingForCrypto;
            }
        }
    }

    //------------------------------------------------------------------------------
    // WebSocket Client
    //------------------------------------------------------------------------------

    esp_err_t start_client() {
        if (sock.client) {
            esp_websocket_client_destroy(sock.client);
            sock.client = nullptr;
        }

        esp_websocket_client_config_t cfg = {};
        cfg.uri = "ws://192.168.0.245:9091";
        cfg.disable_auto_reconnect = false;

        sock.client = esp_websocket_client_init(&cfg);
        if (!sock.client) return ESP_FAIL;

        esp_websocket_register_events(sock.client, WEBSOCKET_EVENT_ANY, ws_event_handler, nullptr);
        return esp_websocket_client_start(sock.client);
    }

    //------------------------------------------------------------------------------
    // Main Task
    //------------------------------------------------------------------------------

    void sockets_task(void*) {
        esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, wifi_event_handler, nullptr);
        esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, nullptr);

        if (kd_common_is_wifi_connected()) {
            sock.state = State::WaitingForCrypto;
        }

        bool started = false;

        while (true) {
            // State machine
            switch (sock.state) {
            case State::WaitingForNetwork:
                vTaskDelay(pdMS_TO_TICKS(500));
                continue;

            case State::WaitingForCrypto:
                if (kd_common_crypto_get_state() == CryptoState_t::CRYPTO_STATE_VALID_CERT) {
#ifdef ENABLE_OTA
                    sock.state = State::WaitingForOTA;
                    show_fs_sprite("check_updates");
#else
                    sock.state = State::Ready;
#endif
                }
                else {
                    vTaskDelay(pdMS_TO_TICKS(500));
                }
                continue;

            case State::WaitingForOTA:
#ifdef ENABLE_OTA
                if (kd_common_ota_has_completed_boot_check()) {
                    sock.state = State::Ready;
                }
                else {
                    vTaskDelay(pdMS_TO_TICKS(500));
                }
#else
                sock.state = State::Ready;
#endif
                continue;

            case State::Ready:
                if (!started) {
                    show_fs_sprite("connecting");
                    if (start_client() == ESP_OK) {
                        ESP_LOGI(TAG, "Client started");
                        started = true;
                    }
                    else {
                        vTaskDelay(pdMS_TO_TICKS(5000));
                    }
                    continue;
                }
                break;
            }

            // Send outbound
            QueuedMessage out;
            while (xQueueReceive(sock.outbox, &out, 0) == pdTRUE) {
                if (out.data && sock.is_connected()) {
                    esp_websocket_client_send_bin(sock.client,
                        reinterpret_cast<char*>(out.data), out.len, pdMS_TO_TICKS(5000));
                }
                if (out.data) heap_caps_free(out.data);
            }

            // Process inbound
            QueuedMessage in;
            while (xQueueReceive(sock.inbox, &in, 0) == pdTRUE) {
                if (in.data) {
                    auto* msg = kd__v1__matrx_message__unpack(&spiram_allocator, in.len, in.data);
                    if (msg) {
                        handle_message(msg);
                        kd__v1__matrx_message__free_unpacked(msg, &spiram_allocator);
                    }
                    heap_caps_free(in.data);
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
    sock.outbox = xQueueCreate(QUEUE_SIZE, sizeof(QueuedMessage));
    sock.inbox = xQueueCreate(QUEUE_SIZE, sizeof(QueuedMessage));

    if (!sock.outbox || !sock.inbox) {
        ESP_LOGE(TAG, "Failed to create queues");
        return;
    }

    msg_init(sock.outbox);

    xTaskCreatePinnedToCore(sockets_task, "sockets", TASK_STACK, nullptr,
        TASK_PRIORITY, &sock.task, 1);

    ESP_LOGI(TAG, "Initialized");
}

void sockets_deinit() {
    if (sock.client) {
        esp_websocket_client_stop(sock.client);
        esp_websocket_client_destroy(sock.client);
        sock.client = nullptr;
    }

    if (sock.task) {
        vTaskDelete(sock.task);
        sock.task = nullptr;
    }

    sock.rx_reset();

    QueuedMessage msg;
    if (sock.outbox) {
        while (xQueueReceive(sock.outbox, &msg, 0) == pdTRUE) heap_caps_free(msg.data);
        vQueueDelete(sock.outbox);
    }
    if (sock.inbox) {
        while (xQueueReceive(sock.inbox, &msg, 0) == pdTRUE) heap_caps_free(msg.data);
        vQueueDelete(sock.inbox);
    }
}

bool sockets_is_connected() {
    return sock.is_connected();
}

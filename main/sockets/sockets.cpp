// Sockets - Event-driven WebSocket client
#include "sockets.h"
#include "handlers.h"
#include "messages.h"

#include <esp_log.h>
#include <esp_websocket_client.h>
#include <esp_event.h>
#include <esp_wifi.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <esp_crt_bundle.h>

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include <kd_common.h>
#include <kd_ota.h>
#include <kd/v1/matrx.pb-c.h>

#include "apps.h"
#include "scheduler.h"

static const char* TAG = "sockets";

namespace {

    //------------------------------------------------------------------------------
    // Configuration
    //------------------------------------------------------------------------------

    constexpr size_t QUEUE_SIZE = 8;
    constexpr size_t MAX_MSG_SIZE = 150 * 1024;  // 150KB
    constexpr int64_t QUEUE_POLL_US = 100 * 1000;  // 100ms timer for queue processing

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
        Connected,
    };

    struct QueuedMessage {
        uint8_t* data;
        size_t len;
    };

    struct {
        QueueHandle_t outbox = nullptr;
        QueueHandle_t inbox = nullptr;
        esp_websocket_client_handle_t client = nullptr;
        esp_timer_handle_t queue_timer = nullptr;
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

    // Forward declarations
    void try_advance_state();
    void process_queues();
    void schedule_reconnect();
    esp_err_t start_client();

    // Timer to check crypto/OTA state periodically until ready
    esp_timer_handle_t state_check_timer = nullptr;

    // mTLS certificate data (cached for reconnections)
    esp_ds_data_ctx_t* static_ds_data_ctx = nullptr;
    char* static_cert = nullptr;
    size_t static_cert_len = 0;

    // Reconnect timer
    esp_timer_handle_t reconnect_timer = nullptr;
    constexpr int64_t RECONNECT_DELAY_US = 2000 * 1000;  // 2 seconds

    //------------------------------------------------------------------------------
    // Queue Processing (called by timer)
    //------------------------------------------------------------------------------

    void queue_timer_callback(void*) {
        process_queues();
    }

    void process_queues() {
        if (!sock.is_connected()) return;

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
    }

    void start_queue_timer() {
        if (sock.queue_timer) {
            esp_timer_start_periodic(sock.queue_timer, QUEUE_POLL_US);
        }
    }

    void stop_queue_timer() {
        if (sock.queue_timer) {
            esp_timer_stop(sock.queue_timer);
        }
    }

    //------------------------------------------------------------------------------
    // Reconnection
    //------------------------------------------------------------------------------

    void reconnect_timer_callback(void*) {
        ESP_LOGI(TAG, "Reconnect timer fired, destroying old client");
        if (sock.client) {
            esp_websocket_client_destroy(sock.client);
            sock.client = nullptr;
        }
        if (sock.state == State::Ready) {
            start_client();
        }
    }

    void schedule_reconnect() {
        if (reconnect_timer) {
            // Stop any pending reconnect first
            esp_timer_stop(reconnect_timer);
            esp_timer_start_once(reconnect_timer, RECONNECT_DELAY_US);
            ESP_LOGI(TAG, "Scheduled reconnect in %lld ms", RECONNECT_DELAY_US / 1000);
        }
    }

    //------------------------------------------------------------------------------
    // WebSocket Events
    //------------------------------------------------------------------------------

    void ws_event_handler(void*, esp_event_base_t, int32_t event_id, void* event_data) {
        auto* data = static_cast<esp_websocket_event_data_t*>(event_data);

        switch (event_id) {
        case WEBSOCKET_EVENT_CONNECTED:
            ESP_LOGI(TAG, "Connected");
            sock.state = State::Connected;
            scheduler_on_connect();
            show_fs_sprite("ready");
            msg_send_device_info();
            msg_send_schedule_request();
            start_queue_timer();
            break;

        case WEBSOCKET_EVENT_DISCONNECTED:
        case WEBSOCKET_EVENT_CLOSED:
        case WEBSOCKET_EVENT_ERROR:
            ESP_LOGW(TAG, "Disconnected/error (event=%ld)", event_id);
            stop_queue_timer();
            sock.rx_reset();
            if (sock.state == State::Connected) {
                sock.state = State::Ready;
                scheduler_on_disconnect();
                show_fs_sprite("connecting");
                // Schedule reconnection via timer (can't block in event handler)
                schedule_reconnect();
            }
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

    //------------------------------------------------------------------------------
    // WebSocket Client
    //------------------------------------------------------------------------------

    esp_err_t start_client() {
        if (sock.client) {
            ESP_LOGW(TAG, "Client already initialized, destroying first");
            esp_websocket_client_destroy(sock.client);
            sock.client = nullptr;
        }

        // Initialize certificate data if not already cached
        if (static_cert == nullptr) {
            static_ds_data_ctx = kd_common_crypto_get_ctx();
            if (static_ds_data_ctx == nullptr) {
                ESP_LOGE(TAG, "Failed to get DS context");
                return ESP_FAIL;
            }

            // First call to get required length
            static_cert_len = 0;
            esp_err_t ret = kd_common_get_device_cert(nullptr, &static_cert_len);
            if (ret != ESP_OK || static_cert_len == 0) {
                ESP_LOGE(TAG, "Failed to get device certificate length: %s", esp_err_to_name(ret));
                return ret;
            }

            static_cert = static_cast<char*>(heap_caps_calloc(static_cert_len + 1, sizeof(char), MALLOC_CAP_SPIRAM));
            if (static_cert == nullptr) {
                ESP_LOGE(TAG, "Failed to allocate certificate buffer");
                return ESP_ERR_NO_MEM;
            }

            ret = kd_common_get_device_cert(static_cert, &static_cert_len);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "Failed to get device certificate: %s", esp_err_to_name(ret));
                heap_caps_free(static_cert);
                static_cert = nullptr;
                return ret;
            }
            ESP_LOGI(TAG, "Loaded device certificate (%zu bytes)", static_cert_len);
        }

        esp_websocket_client_config_t cfg = {};
        cfg.uri = SOCKETS_URL;
        cfg.port = 443;
        cfg.client_cert = static_cert;
        cfg.client_cert_len = static_cert_len + 1;  // Include null terminator for PEM
        cfg.client_ds_data = static_ds_data_ctx;
        cfg.crt_bundle_attach = esp_crt_bundle_attach;
        cfg.reconnect_timeout_ms = 2500;
        cfg.network_timeout_ms = 2500;

        cfg.enable_close_reconnect = true;

        sock.client = esp_websocket_client_init(&cfg);
        if (!sock.client) return ESP_FAIL;

        esp_websocket_register_events(sock.client, WEBSOCKET_EVENT_ANY, ws_event_handler, nullptr);

        esp_err_t err = esp_websocket_client_start(sock.client);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Client started with mTLS");
        }
        return err;
    }

    //------------------------------------------------------------------------------
    // State Machine (event-driven)
    //------------------------------------------------------------------------------

    void try_advance_state() {
        switch (sock.state) {
        case State::WaitingForNetwork:
            // Handled by IP event
            break;

        case State::WaitingForCrypto: {
            CryptoState_t crypto_state = kd_common_crypto_get_state();
            if (crypto_state == CryptoState_t::CRYPTO_STATE_VALID_CERT) {
                sock.state = State::WaitingForOTA;
                //show_fs_sprite("check_updates");
                try_advance_state();
            }
            break;
        }

        case State::WaitingForOTA: {
#ifdef ENABLE_OTA
            bool ota_done = kd_common_ota_has_completed_boot_check();
            if (ota_done) {
                sock.state = State::Ready;
                try_advance_state();
            }
#else
            // OTA disabled, skip directly to Ready
            sock.state = State::Ready;
            try_advance_state();
#endif
            break;
        }

        case State::Ready:
            show_fs_sprite("connecting");
            start_client();
            break;

        case State::Connected:
            // Already connected, nothing to do
            break;
        }
    }

    //------------------------------------------------------------------------------
    // Event Handlers
    //------------------------------------------------------------------------------

    void wifi_event_handler(void*, esp_event_base_t base, int32_t id, void*) {
        if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
            if (sock.state != State::WaitingForNetwork) {
                sock.state = State::WaitingForNetwork;
                stop_queue_timer();
            }
        }
        else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
            ESP_LOGI(TAG, "Got IP event, state=%d", static_cast<int>(sock.state));
            if (sock.state == State::WaitingForNetwork) {
                sock.state = State::WaitingForCrypto;
                if (state_check_timer) {
                    esp_timer_start_periodic(state_check_timer, 1000 * 1000);  // 1s
                }
                try_advance_state();
            }
        }
    }

    void state_check_callback(void*) {
        if (sock.state == State::WaitingForCrypto || sock.state == State::WaitingForOTA) {
            try_advance_state();
        }
        else {
            // No longer need to poll
            esp_timer_stop(state_check_timer);
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

    // Create queue processing timer
    esp_timer_create_args_t queue_timer_args = {
        .callback = queue_timer_callback,
        .arg = nullptr,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "sock_queue",
        .skip_unhandled_events = true,
    };
    esp_timer_create(&queue_timer_args, &sock.queue_timer);

    // Create state check timer (for crypto/OTA polling)
    esp_timer_create_args_t state_timer_args = {
        .callback = state_check_callback,
        .arg = nullptr,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "sock_state",
        .skip_unhandled_events = true,
    };
    esp_timer_create(&state_timer_args, &state_check_timer);

    // Create reconnect timer
    esp_timer_create_args_t reconnect_timer_args = {
        .callback = reconnect_timer_callback,
        .arg = nullptr,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "sock_reconn",
        .skip_unhandled_events = true,
    };
    esp_timer_create(&reconnect_timer_args, &reconnect_timer);

    // Register event handlers
    esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, wifi_event_handler, nullptr);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, nullptr);

    // Check if already connected
    if (kd_common_is_wifi_connected()) {
        sock.state = State::WaitingForCrypto;
        // Start state polling timer
        esp_timer_start_periodic(state_check_timer, 1000 * 1000);  // 1s
        try_advance_state();
    }
}

void sockets_deinit() {
    stop_queue_timer();
    if (reconnect_timer) {
        esp_timer_stop(reconnect_timer);
        esp_timer_delete(reconnect_timer);
        reconnect_timer = nullptr;
    }
    if (state_check_timer) {
        esp_timer_stop(state_check_timer);
        esp_timer_delete(state_check_timer);
        state_check_timer = nullptr;
    }
    if (sock.queue_timer) {
        esp_timer_delete(sock.queue_timer);
        sock.queue_timer = nullptr;
    }

    if (sock.client) {
        esp_websocket_client_stop(sock.client);
        esp_websocket_client_destroy(sock.client);
        sock.client = nullptr;
    }

    sock.rx_reset();

    // Cleanup mTLS certificate data
    if (static_cert) {
        heap_caps_free(static_cert);
        static_cert = nullptr;
        static_cert_len = 0;
    }
    if (static_ds_data_ctx) {
        free(static_ds_data_ctx->esp_ds_data);
        free(static_ds_data_ctx);
        static_ds_data_ctx = nullptr;
    }

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

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

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include <kd_common.h>
#include <kd/v1/matrx.pb-c.h>

#include "apps.h"
#include "scheduler.h"
#include "../cert_renewal/cert_renewal.h"

static const char* TAG = "sockets";

namespace {

    //------------------------------------------------------------------------------
    // Configuration
    //------------------------------------------------------------------------------

    constexpr size_t QUEUE_SIZE = 4;
    constexpr size_t MAX_MSG_SIZE = 100 * 1024;  // 100KB
    constexpr int64_t QUEUE_POLL_US = 50 * 1000;  // 50ms timer for queue processing

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

    // Timer to check crypto/OTA state periodically until ready
    esp_timer_handle_t state_check_timer = nullptr;

    // Timer to retry cert renewal check once NTP syncs
    esp_timer_handle_t cert_check_timer = nullptr;
    constexpr int64_t CERT_CHECK_RETRY_US = 5 * 1000 * 1000;  // 5 seconds

    void cert_check_timer_callback(void*) {
        if (!sock.is_connected()) {
            esp_timer_stop(cert_check_timer);
            return;
        }

        if (kd_common_ntp_is_synced()) {
            esp_timer_stop(cert_check_timer);
            ESP_LOGI(TAG, "NTP synced, checking certificate");
            cert_renewal_check();
        }
    }

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

            // Check cert if NTP is synced, otherwise start timer to wait for it
            if (kd_common_ntp_is_synced()) {
                cert_renewal_check();
            }
            else if (cert_check_timer) {
                ESP_LOGI(TAG, "Waiting for NTP sync to check certificate");
                esp_timer_start_periodic(cert_check_timer, CERT_CHECK_RETRY_US);
            }

            start_queue_timer();
            break;

        case WEBSOCKET_EVENT_DISCONNECTED:
        case WEBSOCKET_EVENT_CLOSED:
        case WEBSOCKET_EVENT_ERROR:
            ESP_LOGW(TAG, "Disconnected/error (event=%ld)", event_id);
            stop_queue_timer();
            if (cert_check_timer) esp_timer_stop(cert_check_timer);
            sock.rx_reset();
            sock.state = State::Ready;  // Will try to reconnect
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

        esp_err_t err = esp_websocket_client_start(sock.client);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Client started");
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

        case State::WaitingForCrypto:
            if (kd_common_crypto_get_state() == CryptoState_t::CRYPTO_STATE_VALID_CERT) {
#ifdef ENABLE_OTA
                sock.state = State::WaitingForOTA;
                show_fs_sprite("check_updates");
                try_advance_state();  // Check OTA immediately
#else
                sock.state = State::Ready;
                try_advance_state();
#endif
            }
            break;

        case State::WaitingForOTA:
#ifdef ENABLE_OTA
            if (kd_common_ota_has_completed_boot_check()) {
                sock.state = State::Ready;
                try_advance_state();
            }
#else
            sock.state = State::Ready;
            try_advance_state();
#endif
            break;

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
            if (sock.state == State::WaitingForNetwork) {
                sock.state = State::WaitingForCrypto;
                // Start state polling timer
                if (state_check_timer) {
                    esp_timer_start_periodic(state_check_timer, 100 * 1000);  // 100ms
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

    // Create cert check timer (for waiting on NTP sync)
    esp_timer_create_args_t cert_timer_args = {
        .callback = cert_check_timer_callback,
        .arg = nullptr,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "cert_check",
        .skip_unhandled_events = true,
    };
    esp_timer_create(&cert_timer_args, &cert_check_timer);

    // Register event handlers
    esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, wifi_event_handler, nullptr);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, nullptr);

    // Check if already connected
    if (kd_common_is_wifi_connected()) {
        sock.state = State::WaitingForCrypto;
        // Start state polling timer
        esp_timer_start_periodic(state_check_timer, 100 * 1000);  // 100ms
        try_advance_state();
    }

    ESP_LOGI(TAG, "Initialized (event-driven)");
}

void sockets_deinit() {
    stop_queue_timer();
    if (state_check_timer) {
        esp_timer_stop(state_check_timer);
        esp_timer_delete(state_check_timer);
        state_check_timer = nullptr;
    }
    if (cert_check_timer) {
        esp_timer_stop(cert_check_timer);
        esp_timer_delete(cert_check_timer);
        cert_check_timer = nullptr;
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

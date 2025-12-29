// Certificate renewal module
// Currently disabled - calls are commented out in sockets.cpp

#include "cert_renewal.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <esp_heap_caps.h>
#include <mbedtls/x509_crt.h>
#include <time.h>

#include <kd_common.h>
#include "../components/kd_common/src/crypto.h"
#include <kd/v1/matrx.pb-c.h>
#include "../sockets/messages.h"

static const char* TAG = "cert_renewal";

namespace {

// Check cert expiry every 24 hours
constexpr int64_t CHECK_INTERVAL_MS = 24 * 60 * 60 * 1000;

// Renew when less than 3 years remaining
constexpr int64_t RENEWAL_THRESHOLD_SEC = 3LL * 365 * 24 * 60 * 60;

// CSR buffer size
constexpr size_t CSR_BUFFER_SIZE = 4096;

// State
const char** g_cert = nullptr;
size_t* g_cert_len = nullptr;
int64_t last_check_ms = 0;
bool renewal_in_progress = false;

int64_t get_seconds_until_expiry() {
    if (g_cert == nullptr || *g_cert == nullptr || g_cert_len == nullptr || *g_cert_len == 0) {
        return -1;
    }

    mbedtls_x509_crt crt;
    mbedtls_x509_crt_init(&crt);

    int ret = mbedtls_x509_crt_parse(&crt,
        reinterpret_cast<const unsigned char*>(*g_cert),
        *g_cert_len + 1);

    if (ret != 0) {
        ESP_LOGE(TAG, "Failed to parse certificate: -0x%04X", -ret);
        mbedtls_x509_crt_free(&crt);
        return -1;
    }

    time_t now;
    time(&now);

    struct tm expiry_tm = {};
    expiry_tm.tm_year = crt.valid_to.year - 1900;
    expiry_tm.tm_mon = crt.valid_to.mon - 1;
    expiry_tm.tm_mday = crt.valid_to.day;
    expiry_tm.tm_hour = crt.valid_to.hour;
    expiry_tm.tm_min = crt.valid_to.min;
    expiry_tm.tm_sec = crt.valid_to.sec;

    time_t expiry = mktime(&expiry_tm);
    mbedtls_x509_crt_free(&crt);

    if (expiry == (time_t)-1) {
        ESP_LOGE(TAG, "Failed to convert expiry time");
        return -1;
    }

    return static_cast<int64_t>(difftime(expiry, now));
}

}  // namespace

void cert_renewal_init(const char** cert, size_t* cert_len) {
    g_cert = cert;
    g_cert_len = cert_len;
    last_check_ms = 0;
    renewal_in_progress = false;
    ESP_LOGI(TAG, "Cert renewal module initialized");
}

bool cert_renewal_check() {
    ESP_LOGI(TAG, "Checking certificate renewal status");

    if (renewal_in_progress) {
        ESP_LOGD(TAG, "Renewal already in progress");
        return false;
    }

    // Check if system time is valid (after 2024)
    time_t now;
    time(&now);
    struct tm* tm_now = localtime(&now);
    if (tm_now == nullptr || tm_now->tm_year < 124) {
        ESP_LOGW(TAG, "System time not valid yet (year=%d)", tm_now ? tm_now->tm_year + 1900 : 0);
        return false;
    }

    int64_t now_ms = esp_timer_get_time() / 1000;

    // Only check periodically
    if (last_check_ms > 0 && (now_ms - last_check_ms) < CHECK_INTERVAL_MS) {
        ESP_LOGD(TAG, "Skipping check, last check was %lld ms ago", now_ms - last_check_ms);
        return false;
    }
    last_check_ms = now_ms;

    int64_t seconds_remaining = get_seconds_until_expiry();
    if (seconds_remaining < 0) {
        ESP_LOGW(TAG, "Could not determine certificate expiry");
        return false;
    }

    int64_t days_remaining = seconds_remaining / (24 * 60 * 60);
    int64_t threshold_days = RENEWAL_THRESHOLD_SEC / (24 * 60 * 60);
    ESP_LOGI(TAG, "Certificate expires in %lld days (threshold: %lld days)", days_remaining, threshold_days);

    if (seconds_remaining <= RENEWAL_THRESHOLD_SEC) {
        ESP_LOGI(TAG, "Certificate expiring soon, requesting renewal");
        renewal_in_progress = true;
        cert_renewal_send_request();
        return true;
    }

    ESP_LOGI(TAG, "Certificate valid, no renewal needed");
    return false;
}

bool cert_renewal_handle_response(Kd__V1__CertResponse* response) {
    renewal_in_progress = false;

    if (response == nullptr) return false;

    if (!response->success) {
        ESP_LOGE(TAG, "Cert renewal failed: %s",
            response->error ? response->error : "unknown error");
        return false;
    }

    if (response->device_cert.data == nullptr || response->device_cert.len == 0) {
        ESP_LOGE(TAG, "Cert renewal response missing certificate");
        return false;
    }

    ESP_LOGI(TAG, "Received new certificate (%zu bytes)", response->device_cert.len);

    esp_err_t err = crypto_set_device_cert(
        reinterpret_cast<char*>(response->device_cert.data),
        response->device_cert.len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to store new certificate: %s", esp_err_to_name(err));
        return false;
    }

    ESP_LOGI(TAG, "Certificate stored successfully");
    last_check_ms = 0;

    // Return true to signal caller should reconnect
    return true;
}

void cert_renewal_send_request() {
    msg_send_cert_renewal_request();
}

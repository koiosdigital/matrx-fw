#ifndef CRYPTO_H
#define CRYPTO_H

#include "esp_err.h"
#include "esp_ds.h"

#define NVS_CRYPTO_PARTITION "nvs_factory"
#define NVS_CRYPTO_NAMESPACE "secure_crypto"

#define NVS_CRYPTO_DEVICE_CERT "dev_cert"
#define NVS_CRYPTO_CIPHERTEXT "cipher_c"
#define NVS_CRYPTO_IV "iv"
#define NVS_CRYPTO_DS_KEY_ID "ds_key_id"
#define NVS_CRYPTO_RSA_LEN "rsa_len"
#define NVS_CRYPTO_CSR "csr"

#define DS_KEY_BLOCK EFUSE_BLK_KEY3
#define KEY_SIZE 4096

typedef struct esp_ds_data_ctx {
    esp_ds_data_t* esp_ds_data;
    uint8_t efuse_key_id;
    uint16_t rsa_length_bits;
} esp_ds_data_ctx_t;

typedef enum CryptoState_t {
    CRYPTO_STATE_UNINITIALIZED,
    CRYPTO_STATE_KEY_GENERATED,
    CRYPTO_STATE_VALID_CSR,
    CRYPTO_STATE_VALID_CERT
} CryptoState_t;

esp_err_t crypto_init(void);
esp_ds_data_ctx_t* crypto_get_ds_data_ctx();
esp_err_t crypto_get_csr(char* buffer, size_t* len);
esp_err_t crypto_clear_csr();
esp_err_t crypto_get_device_cert(char* buffer, size_t* len);
esp_err_t crypto_set_device_cert(char* buffer, size_t len);
CryptoState_t crypto_get_state();

#endif
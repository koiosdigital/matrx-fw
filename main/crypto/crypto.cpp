#include "crypto.h"

#include "esp_ds.h"
#include "esp_efuse.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"
#include "esp_wifi.h"
#include "esp_task_wdt.h"

#include "mbedtls/rsa.h"
#include "mbedtls/x509_csr.h"
#include "mbedtls/platform_util.h"
#include "mbedtls/error.h"
#include "mbedtls/sha256.h"
#include "mbedtls/bignum.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"

#include "provisioning.h"

#include "string.h"
#include "stdlib.h"
#include "display.h"

static const char* TAG = "crypto";
CryptoState_t crypto_state = CRYPTO_STATE_UNINITIALIZED;
SemaphoreHandle_t keygen_mutex = NULL;

void crypto_generate_key(void* pvParameter) {
    mbedtls_rsa_context* rsa = (mbedtls_rsa_context*)pvParameter;
    xSemaphoreTake(keygen_mutex, portMAX_DELAY);

    esp_err_t error = ESP_OK;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context ctrDrbg;
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&ctrDrbg);


    ESP_LOGI(TAG, "gen RSA key: %d bits", KEY_SIZE);

    esp_task_wdt_config_t twdt_config = {
        .timeout_ms = 1000 * 60 * 2, // 2 minutes
        .idle_core_mask = (1 << portNUM_PROCESSORS) - 1, // Bitmask of all cores
        .trigger_panic = false,
    };
    esp_task_wdt_reconfigure(&twdt_config);

    //seed the entropy source
    error = mbedtls_ctr_drbg_seed(&ctrDrbg, mbedtls_entropy_func, &entropy,
        NULL, 0);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "ctr_drbg_seed failed");
        goto exit;
    }

    error = mbedtls_rsa_gen_key(rsa, mbedtls_ctr_drbg_random, &ctrDrbg, KEY_SIZE, 65537);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "rsa_gen_key failed");
        goto exit;
    }

    error = mbedtls_rsa_complete(rsa);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "rsa_complete failed");
        goto exit;
    }

    ESP_LOGI(TAG, "keygen complete");

exit:
    twdt_config.timeout_ms = 3000;
    esp_task_wdt_reconfigure(&twdt_config);

    mbedtls_ctr_drbg_free(&ctrDrbg);
    mbedtls_entropy_free(&entropy);
    xSemaphoreGive(keygen_mutex);
}

esp_err_t crypto_calculate_rinv_mprime(mbedtls_mpi* N, mbedtls_mpi* rinv, uint32_t* mprime) {
    esp_err_t error = ESP_OK;

    mbedtls_mpi rr, ls32, a;
    uint32_t a32 = 0;
    mbedtls_mpi_init(&rr);
    mbedtls_mpi_init(&ls32);
    mbedtls_mpi_init(&a);

    ESP_LOGD(TAG, "calculating rinv, mprime");

    error = mbedtls_mpi_lset(&rr, 1);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "lset failed: rr");
        goto exit;
    }

    error = mbedtls_mpi_shift_l(&rr, KEY_SIZE * 2);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "shift_l failed: rr");
        goto exit;
    }

    error = mbedtls_mpi_mod_mpi(rinv, &rr, N);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "mod_mpi failed: rinv");
        goto exit;
    }

    error = mbedtls_mpi_lset(&ls32, 1);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "lset failed: ls32");
        goto exit;
    }

    error = mbedtls_mpi_shift_l(&ls32, 32);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "shift_l failed: ls32");
        goto exit;
    }

    error = mbedtls_mpi_inv_mod(&a, N, &ls32);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "inv_mod failed: a");
        goto exit;
    }

    // a32 = a 
    error = mbedtls_mpi_write_binary_le(&a, (uint8_t*)&a32, sizeof(uint32_t));
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "write_binary_le failed: a");
        goto exit;
    }

    // mprime
    *mprime = ((int32_t)a32 * -1) & 0xFFFFFFFF;

exit:
    mbedtls_mpi_free(&rr);
    mbedtls_mpi_free(&ls32);
    mbedtls_mpi_free(&a);
    return error;
}

esp_err_t crypto_rinv_mprime_to_ds_params(mbedtls_mpi* D, mbedtls_mpi* N, mbedtls_mpi* rinv, uint32_t mprime, esp_ds_p_data_t* params)
{
    esp_err_t error = ESP_OK;

    ESP_LOGD(TAG, "converting to DS params");

    error = mbedtls_mpi_write_binary_le(D, (uint8_t*)params->Y, sizeof(params->Y));
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "write_binary_le failed: Y");
        return error;
    }
    error = mbedtls_mpi_write_binary_le(N, (uint8_t*)params->M, sizeof(params->M));
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "write_binary_le failed: M");
        return error;
    }
    error = mbedtls_mpi_write_binary_le(rinv, (uint8_t*)params->Rb, sizeof(params->Rb));
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "write_binary_le failed: Rb");
        return error;
    }

    //reverse_bytes((uint8_t*)params->Y, sizeof(pd_4096_bit_t)); // big to little endian
    //reverse_bytes((uint8_t*)params->M, sizeof(pd_4096_bit_t)); // big to little endian
    //reverse_bytes((uint8_t*)params->Rb, sizeof(pd_4096_bit_t));// big to little endian

    params->M_prime = mprime;
    params->length = ESP_DS_RSA_2048;

    return error;
}

void reverse_bytes(uint8_t* data, size_t len) {
    for (size_t i = 0; i < len / 2; i++) {
        uint8_t temp = data[i];
        data[i] = data[len - i - 1];
        data[len - i - 1] = temp;
    }
}

esp_err_t crypto_store_csr(mbedtls_rsa_context* rsa, nvs_handle_t handle) {
    esp_err_t error = ESP_OK;

    mbedtls_x509write_csr req;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context ctr_drbg;
    mbedtls_pk_context pk;
    char cn[128];
    unsigned char* csr_buffer = NULL;

    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&ctr_drbg);

    //seed the entropy source
    error = mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy, NULL, 0);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "ctr_drbg_seed failed");
        goto exit;
    }

    //generate CSR
    mbedtls_x509write_csr_init(&req);
    mbedtls_x509write_csr_set_md_alg(&req, MBEDTLS_MD_SHA256);

    error = mbedtls_x509write_csr_set_key_usage(&req, MBEDTLS_X509_KU_DIGITAL_SIGNATURE);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "set_key_usage failed");
        goto exit;
    }

    error = mbedtls_x509write_csr_set_ns_cert_type(&req, MBEDTLS_X509_NS_CERT_TYPE_SSL_CLIENT);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "set_ns_cert_type failed");
        goto exit;
    }

    snprintf(cn, sizeof(cn), "CN=%s.iotdevices.koiosdigital.net", get_provisioning_device_name());

    error = mbedtls_x509write_csr_set_subject_name(&req, cn);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "set_subject_name failed");
        goto exit;
    }

    error = mbedtls_pk_setup(&pk, mbedtls_pk_info_from_type(MBEDTLS_PK_RSA));
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "pk_setup failed");
        goto exit;
    }

    error = mbedtls_rsa_copy(mbedtls_pk_rsa(pk), rsa);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "rsa_copy failed");
        goto exit;
    }

    mbedtls_x509write_csr_set_key(&req, &pk);

    csr_buffer = (unsigned char*)calloc(4096, sizeof(unsigned char));
    if (csr_buffer == NULL) {
        ESP_LOGE(TAG, "malloc failed: csr");
        goto exit;
    }

    error = mbedtls_x509write_csr_pem(&req, csr_buffer, 4096, mbedtls_ctr_drbg_random, &ctr_drbg);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "csr_pem failed");
        goto exit;
    }

    error = nvs_set_blob(handle, NVS_CRYPTO_CSR, csr_buffer, 4096);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "nvs_set_blob failed");
        goto exit;
    }

    error = nvs_commit(handle);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "nvs_commit failed");
        goto exit;
    }

exit:
    nvs_close(handle);
    mbedtls_x509write_csr_free(&req);
    mbedtls_ctr_drbg_free(&ctr_drbg);
    mbedtls_entropy_free(&entropy);
    free(csr_buffer);

    return error;
}

esp_err_t crypto_keygen_if_needed() {
    esp_err_t error = ESP_OK;

    // allocations
    esp_ds_p_data_t* params = NULL;
    esp_ds_data_t* encrypted = NULL;
    mbedtls_mpi rinv;
    mbedtls_rsa_context rsa;

    mbedtls_rsa_init(&rsa);
    mbedtls_mpi_init(&rinv);

    uint32_t mprime = 0;

    bool has_fuses = esp_efuse_get_key_purpose(DS_KEY_BLOCK) ==
        ESP_EFUSE_KEY_PURPOSE_HMAC_DOWN_DIGITAL_SIGNATURE;

    if (has_fuses) {
        ESP_LOGD(TAG, "fuses already burned");
        crypto_state = CRYPTO_STATE_KEY_GENERATED;
        goto exit;
    }

    keygen_mutex = xSemaphoreCreateBinary();

    // get RSA keypair
    xTaskCreatePinnedToCore(crypto_generate_key, "crypto_keygen", 4096, (void*)&rsa, 5, NULL, 1);
    vTaskDelay(pdMS_TO_TICKS(1000));

    while (xSemaphoreTake(keygen_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGI(TAG, "waiting for keygen");
    }

    // rinv, mprime
    error = crypto_calculate_rinv_mprime(&rsa.private_N, &rinv, &mprime);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "rinv, mprime failed");
        goto exit;
    }

    // alloc
    params = (esp_ds_p_data_t*)calloc(sizeof(esp_ds_p_data_t), 1);
    if (params == NULL) {
        ESP_LOGE(TAG, "malloc failed: esp_ds_p_data_t");
        error = ESP_ERR_NO_MEM;
        goto exit;
    }

    // esp_ds params
    error = crypto_rinv_mprime_to_ds_params(&rsa.private_D, &rsa.private_N, &rinv, mprime, params);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "rinv, mprime to ds params failed");
        goto exit;
    }

    // iv & hmac
    uint8_t iv[16];
    uint8_t hmac[32];
    esp_fill_random(iv, 16);
    esp_fill_random(hmac, 32);

    // allocate encrypted data
    encrypted = (esp_ds_data_t*)heap_caps_calloc(sizeof(esp_ds_data_t), 1, MALLOC_CAP_DMA);
    if (encrypted == NULL) {
        ESP_LOGE(TAG, "malloc failed: esp_ds_data_t");
        error = ESP_ERR_NO_MEM;
        goto exit;
    }

    error = esp_ds_encrypt_params(encrypted, iv, params, hmac);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "encrypt params failed");
        goto exit;
    }

    //open nvs handle
    nvs_handle handle;
    error = nvs_open_from_partition(NVS_CRYPTO_PARTITION, NVS_CRYPTO_NAMESPACE, NVS_READWRITE, &handle);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "nvs open failed");
        goto exit;
    }

    //write to nvs
    error = nvs_set_blob(handle, NVS_CRYPTO_CIPHERTEXT, encrypted->c, ESP_DS_C_LEN);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "nvs set blob failed: ciphertext");
        goto exit;
    }

    error = nvs_set_blob(handle, NVS_CRYPTO_IV, iv, ESP_DS_IV_LEN);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "nvs set blob failed: iv");
        goto exit;
    }

    error = nvs_set_u8(handle, NVS_CRYPTO_DS_KEY_ID, DS_KEY_BLOCK);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "nvs set u8 failed: ds key id");
        goto exit;
    }

    error = nvs_set_u16(handle, NVS_CRYPTO_RSA_LEN, KEY_SIZE);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "nvs set u16 failed: rsa key length");
        goto exit;
    }

    error = nvs_commit(handle);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "nvs commit failed");
        goto exit;
    }

    error = crypto_store_csr(&rsa, handle);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "store csr failed");
        goto exit;
    }

    error = esp_efuse_write_key(DS_KEY_BLOCK, ESP_EFUSE_KEY_PURPOSE_HMAC_DOWN_DIGITAL_SIGNATURE, hmac, 32);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "burn hmac failed");
        goto exit;
    }

    //read protection
    error = esp_efuse_set_read_protect(DS_KEY_BLOCK);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "read protect failed");
        goto exit;
    }

exit:
    nvs_close(handle);
    free(params);
    free(encrypted);
    mbedtls_mpi_free(&rinv);
    mbedtls_rsa_free(&rsa);
    return error;
}

esp_ds_data_ctx_t* crypto_get_ds_data_ctx(void)
{
    esp_err_t error;
    esp_ds_data_ctx_t* ds_data_ctx;
    nvs_handle handle;
    uint32_t len = 0;

    ds_data_ctx = (esp_ds_data_ctx_t*)calloc(sizeof(esp_ds_data_ctx_t), 1);
    if (ds_data_ctx == NULL) {
        ESP_LOGE(TAG, "malloc failed: ds_data_ctx");
        goto exit;
    }

    ds_data_ctx->esp_ds_data = (esp_ds_data_t*)calloc(sizeof(esp_ds_data_t), 1);
    if (ds_data_ctx->esp_ds_data == NULL) {
        ESP_LOGE(TAG, "malloc failed: esp_ds_data");
        goto exit;
    }

    error = nvs_open_from_partition(NVS_CRYPTO_PARTITION, NVS_CRYPTO_NAMESPACE, NVS_READONLY, &handle);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "nvs open failed");
        goto exit;
    }

    len = ESP_DS_C_LEN;
    error = nvs_get_blob(handle, NVS_CRYPTO_CIPHERTEXT, (char*)ds_data_ctx->esp_ds_data->c, (size_t*)&len);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "ds setup error: failed reading ciphertext");
        goto exit;
    }

    len = ESP_DS_IV_LEN;
    error = nvs_get_blob(handle, NVS_CRYPTO_IV, (char*)ds_data_ctx->esp_ds_data->iv, (size_t*)&len);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "ds setup error: failed reading iv");
        goto exit;
    }

    error = nvs_get_u8(handle, NVS_CRYPTO_DS_KEY_ID, &ds_data_ctx->efuse_key_id);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "ds setup error: failed reading efuse key id");
        goto exit;
    }

    error = nvs_get_u16(handle, NVS_CRYPTO_RSA_LEN, &ds_data_ctx->rsa_length_bits);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "ds setup error: failed reading rsa key length");
        goto exit;
    }

    nvs_close(handle);
    return ds_data_ctx;

exit:
    free(ds_data_ctx->esp_ds_data);
    nvs_close(handle);
    free(ds_data_ctx);
    return NULL;
}

esp_err_t crypto_get_csr(char* buffer, size_t* len) {
    esp_err_t error;
    nvs_handle handle;

    error = nvs_open_from_partition(NVS_CRYPTO_PARTITION, NVS_CRYPTO_NAMESPACE, NVS_READONLY, &handle);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "nvs open failed");
        goto exit;
    }

    error = nvs_get_blob(handle, NVS_CRYPTO_CSR, buffer, len);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "nvs get csr failed");
        goto exit;
    }

exit:
    nvs_close(handle);
    return error;
}

esp_err_t crypto_clear_csr() {
    esp_err_t error;
    nvs_handle handle;

    error = nvs_open_from_partition(NVS_CRYPTO_PARTITION, NVS_CRYPTO_NAMESPACE, NVS_READWRITE, &handle);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "nvs open failed");
        goto exit;
    }

    error = nvs_erase_key(handle, NVS_CRYPTO_CSR);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "nvs erase csr failed");
        goto exit;
    }

    error = nvs_commit(handle);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "nvs commit failed");
        goto exit;
    }

exit:
    nvs_close(handle);
    return error;
}

esp_err_t crypto_get_device_cert(char* buffer, size_t* len) {
    esp_err_t error;
    nvs_handle handle;

    error = nvs_open_from_partition(NVS_CRYPTO_PARTITION, NVS_CRYPTO_NAMESPACE, NVS_READONLY, &handle);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "nvs open failed");
        goto exit;
    }

    error = nvs_get_blob(handle, NVS_CRYPTO_DEVICE_CERT, buffer, len);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "nvs get device cert failed");
        goto exit;
    }

exit:
    nvs_close(handle);
    return error;
}

esp_err_t crypto_set_device_cert(char* buffer, size_t len) {
    esp_err_t error;
    nvs_handle handle;

    error = nvs_open_from_partition(NVS_CRYPTO_PARTITION, NVS_CRYPTO_NAMESPACE, NVS_READWRITE, &handle);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "nvs open failed");
        goto exit;
    }

    error = nvs_set_blob(handle, NVS_CRYPTO_DEVICE_CERT, buffer, len);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "nvs set device cert failed");
        goto exit;
    }

    error = nvs_commit(handle);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "nvs commit failed");
        goto exit;
    }

exit:
    nvs_close(handle);
    return error;
}

CryptoState_t crypto_get_state() {
    return crypto_state;
}

esp_err_t crypto_init(void) {
    //will most likely only run in factory, so we can keygen here.
    crypto_keygen_if_needed();

    if (crypto_state == CRYPTO_STATE_KEY_GENERATED) {
        if (crypto_get_csr(NULL, NULL) == ESP_OK) {
            crypto_state = CRYPTO_STATE_VALID_CSR;
        }

        if (crypto_get_device_cert(NULL, NULL) == ESP_OK) {
            crypto_state = CRYPTO_STATE_VALID_CERT;
        }
    }

    return ESP_OK;
}
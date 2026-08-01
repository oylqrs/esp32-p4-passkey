#include "kdbx_internal.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/aes.h"

static const char *TAG = "kdbx";

static bool is_aes_kdf_uuid(const uint8_t uuid[16])
{
    static const uint8_t aes_kdf_uuid[16] = {
        0xc9, 0xd9, 0xf3, 0x9a, 0x62, 0x8a, 0x44, 0x60,
        0xbf, 0x74, 0x0d, 0x08, 0xc1, 0x8a, 0x4f, 0xea,
    };

    return memcmp(uuid, aes_kdf_uuid, sizeof(aes_kdf_uuid)) == 0;
}

esp_err_t kdbx_derive_aes_kdf_master_key(const kdbx_header_t *header,
                                         const char *password,
                                         uint8_t master_key[32],
                                         uint8_t hmac_base_key[64])
{
    int64_t total_start_us = kdbx_now_us();
    if (!header->has_master_seed || !header->has_kdf_uuid ||
        !header->has_kdf_seed || !header->has_kdf_rounds) {
        ESP_LOGW(TAG, "Missing KDBX AES-KDF material");
        kdbx_log_elapsed("AES-KDF failed missing material", total_start_us);
        return ESP_ERR_INVALID_STATE;
    }

    if (!is_aes_kdf_uuid(header->kdf_uuid)) {
        ESP_LOGW(TAG, "KDF UUID is not AES-KDF");
        kdbx_log_elapsed("AES-KDF failed unsupported UUID", total_start_us);
        return ESP_ERR_NOT_SUPPORTED;
    }

    uint8_t password_hash[32];
    uint8_t composite_key[32];
    uint8_t transformed_key[32];
    uint8_t master_input[64];

    int64_t step_start_us = kdbx_now_us();
    esp_err_t ret = kdbx_sha256_bytes((const uint8_t *)password, strlen(password), password_hash);
    if (ret != ESP_OK) {
        kdbx_log_elapsed("AES-KDF failed hashing password", total_start_us);
        return ret;
    }

    ret = kdbx_sha256_bytes(password_hash, sizeof(password_hash), composite_key);
    if (ret != ESP_OK) {
        kdbx_log_elapsed("AES-KDF failed hashing composite key", total_start_us);
        return ret;
    }
    kdbx_log_elapsed("AES-KDF password/composite hash", step_start_us);

    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    int aes_ret = mbedtls_aes_setkey_enc(&aes, header->kdf_seed, 256);
    if (aes_ret != 0) {
        mbedtls_aes_free(&aes);
        ESP_LOGW(TAG, "AES-KDF setkey failed: %d", aes_ret);
        return ESP_FAIL;
    }

    uint8_t block[32];
    memcpy(block, composite_key, sizeof(block));
    step_start_us = kdbx_now_us();
    ESP_LOGI(TAG, "KDBX AES-KDF transform start rounds=%llu", (unsigned long long)header->kdf_rounds);
    for (uint64_t i = 0; i < header->kdf_rounds; i++) {
        mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_ENCRYPT, block, block);
        mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_ENCRYPT, block + 16, block + 16);
        if ((i & 0x03ff) == 0x03ff) {
            kdbx_crypto_cooperate();
        }
    }
    mbedtls_aes_free(&aes);
    ESP_LOGI(TAG, "KDBX timing: AES-KDF transform rounds=%llu",
             (unsigned long long)header->kdf_rounds);
    kdbx_log_elapsed("AES-KDF transform loop", step_start_us);

    step_start_us = kdbx_now_us();
    ret = kdbx_sha256_bytes(block, sizeof(block), transformed_key);
    if (ret != ESP_OK) {
        kdbx_log_elapsed("AES-KDF failed hashing transformed key", total_start_us);
        return ret;
    }

    memcpy(master_input, header->master_seed, sizeof(header->master_seed));
    memcpy(master_input + sizeof(header->master_seed), transformed_key, sizeof(transformed_key));
    ret = kdbx_sha256_bytes(master_input, sizeof(master_input), master_key);
    if (ret != ESP_OK) {
        kdbx_log_elapsed("AES-KDF failed deriving master key", total_start_us);
        return ret;
    }

    uint8_t hmac_input[65];
    memcpy(hmac_input, master_input, sizeof(master_input));
    hmac_input[64] = 0x01;
    ret = kdbx_sha512_bytes(hmac_input, sizeof(hmac_input), hmac_base_key);
    if (ret != ESP_OK) {
        kdbx_log_elapsed("AES-KDF failed deriving HMAC base key", total_start_us);
        return ret;
    }
    kdbx_log_elapsed("AES-KDF final key hashes", step_start_us);

    kdbx_log_hex_value("AES-KDF transformed key", transformed_key, sizeof(transformed_key));
    kdbx_log_hex_value("KDBX master key", master_key, 32);
    kdbx_log_hex_value("KDBX HMAC base key", hmac_base_key, 64);
    kdbx_log_elapsed("AES-KDF total", total_start_us);
    return ESP_OK;
}

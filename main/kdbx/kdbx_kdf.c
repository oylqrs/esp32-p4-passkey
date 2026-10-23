#include "kdbx_internal.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/aes.h"

#if defined(CONFIG_IDF_TARGET_ESP32P4) && defined(CONFIG_MBEDTLS_HARDWARE_AES) && \
    !defined(CONFIG_MBEDTLS_AES_USE_PSEUDO_ROUND_FUNC)
#define KDBX_USE_ESP32P4_AES_KDF 1
#include "aes/esp_aes.h"
#include "hal/aes_hal.h"
#else
#define KDBX_USE_ESP32P4_AES_KDF 0
#endif

#define KDBX_AES_KDF_P4_BATCH_ROUNDS 1024U

static const char *TAG = "kdbx";

static bool is_aes_kdf_uuid(const uint8_t uuid[16])
{
    static const uint8_t aes_kdf_uuid[16] = {
        0xc9, 0xd9, 0xf3, 0x9a, 0x62, 0x8a, 0x44, 0x60,
        0xbf, 0x74, 0x0d, 0x08, 0xc1, 0x8a, 0x4f, 0xea,
    };

    return memcmp(uuid, aes_kdf_uuid, sizeof(aes_kdf_uuid)) == 0;
}

static const char *unsupported_kdf_name(const uint8_t uuid[16])
{
    static const uint8_t argon2d_uuid[16] = {
        0xef, 0x63, 0x6d, 0xdf, 0x8c, 0x29, 0x44, 0x4b,
        0x91, 0xf7, 0xa9, 0xa4, 0x03, 0xe3, 0x0a, 0x0c,
    };
    static const uint8_t argon2id_uuid[16] = {
        0x9e, 0x29, 0x8b, 0x19, 0x56, 0xdb, 0x47, 0x73,
        0xb2, 0x3d, 0xfc, 0x3e, 0xc6, 0xf0, 0xa1, 0xe6,
    };

    if (memcmp(uuid, argon2d_uuid, sizeof(argon2d_uuid)) == 0) {
        return "Argon2d";
    }
    if (memcmp(uuid, argon2id_uuid, sizeof(argon2id_uuid)) == 0) {
        return "Argon2id";
    }
    return "unknown";
}

static esp_err_t transform_aes_kdf_blocks(const uint8_t key[32],
                                          uint8_t blocks[32],
                                          uint64_t rounds)
{
#if KDBX_USE_ESP32P4_AES_KDF
    KDBX_LOGD(TAG, "KDBX AES-KDF backend=ESP32-P4 block engine batch_rounds=%u",
              KDBX_AES_KDF_P4_BATCH_ROUNDS);

    uint64_t rounds_left = rounds;
    while (rounds_left > 0) {
        uint32_t batch_rounds = rounds_left > KDBX_AES_KDF_P4_BATCH_ROUNDS
                                    ? KDBX_AES_KDF_P4_BATCH_ROUNDS
                                    : (uint32_t)rounds_left;

        /* The public ECB API configures DMA for every 16-byte block. Keep the
         * P4 block engine configured across a batch instead. */
        esp_aes_acquire_hardware();
        uint8_t key_bytes_in_hardware = aes_hal_setkey(key, 32, ESP_AES_ENCRYPT);
        if (key_bytes_in_hardware != 32) {
            esp_aes_release_hardware();
            ESP_LOGE(TAG, "AES-KDF failed to load the complete key into hardware: %u bytes",
                     (unsigned int)key_bytes_in_hardware);
            return ESP_FAIL;
        }

        for (uint32_t i = 0; i < batch_rounds; i++) {
            aes_hal_transform_block(blocks, blocks);
            aes_hal_transform_block(blocks + 16, blocks + 16);
        }
        esp_aes_release_hardware();

        rounds_left -= batch_rounds;
        kdbx_crypto_cooperate();
    }

    return ESP_OK;
#else
    KDBX_LOGD(TAG, "KDBX AES-KDF backend=mbedTLS ECB");

    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    int aes_ret = mbedtls_aes_setkey_enc(&aes, key, 256);
    if (aes_ret != 0) {
        mbedtls_aes_free(&aes);
        ESP_LOGW(TAG, "AES-KDF setkey failed: %d", aes_ret);
        return ESP_FAIL;
    }

    for (uint64_t i = 0; i < rounds; i++) {
        aes_ret = mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_ENCRYPT, blocks, blocks);
        if (aes_ret == 0) {
            aes_ret = mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_ENCRYPT,
                                            blocks + 16, blocks + 16);
        }
        if (aes_ret != 0) {
            break;
        }
        if ((i & 0x03ff) == 0x03ff) {
            kdbx_crypto_cooperate();
        }
    }
    mbedtls_aes_free(&aes);

    if (aes_ret != 0) {
        ESP_LOGW(TAG, "AES-KDF transform failed: %d", aes_ret);
        return ESP_FAIL;
    }
    return ESP_OK;
#endif
}

esp_err_t kdbx_derive_aes_kdf_master_key(const kdbx_header_t *header,
                                         const char *password,
                                         uint8_t master_key[32],
                                         uint8_t hmac_base_key[64])
{
    int64_t total_start_us = kdbx_now_us();
    if (!header->has_master_seed || !header->has_kdf_uuid) {
        ESP_LOGW(TAG, "Missing KDBX key material: master_seed=%s kdf_uuid=%s",
                 header->has_master_seed ? "yes" : "no",
                 header->has_kdf_uuid ? "yes" : "no");
        kdbx_log_elapsed("AES-KDF failed missing material", total_start_us);
        return ESP_ERR_INVALID_STATE;
    }

    if (!is_aes_kdf_uuid(header->kdf_uuid)) {
        ESP_LOGW(TAG, "Unsupported KDBX KDF: %s (only AES-KDF is implemented)",
                 unsupported_kdf_name(header->kdf_uuid));
        kdbx_log_elapsed("AES-KDF failed unsupported UUID", total_start_us);
        return ESP_ERR_NOT_SUPPORTED;
    }

    if (!header->has_kdf_seed || !header->has_kdf_rounds) {
        ESP_LOGW(TAG, "Missing KDBX AES-KDF parameters: seed=%s rounds=%s",
                 header->has_kdf_seed ? "yes" : "no",
                 header->has_kdf_rounds ? "yes" : "no");
        kdbx_log_elapsed("AES-KDF failed missing parameters", total_start_us);
        return ESP_ERR_INVALID_STATE;
    }

    if (header->kdf_rounds > KDBX_MAX_KDF_ROUNDS) {
        ESP_LOGW(TAG, "KDBX AES-KDF rounds out of range: %llu (max %llu)",
                 (unsigned long long)header->kdf_rounds,
                 (unsigned long long)KDBX_MAX_KDF_ROUNDS);
        kdbx_log_elapsed("AES-KDF failed rounds limit", total_start_us);
        return ESP_ERR_INVALID_SIZE;
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

    uint8_t block[32];
    memcpy(block, composite_key, sizeof(block));
    step_start_us = kdbx_now_us();
    KDBX_LOGD(TAG, "KDBX AES-KDF transform start rounds=%llu", (unsigned long long)header->kdf_rounds);
    ret = transform_aes_kdf_blocks(header->kdf_seed, block, header->kdf_rounds);
    if (ret != ESP_OK) {
        kdbx_log_elapsed("AES-KDF failed transform loop", total_start_us);
        return ret;
    }
    KDBX_LOGD(TAG, "KDBX timing: AES-KDF transform rounds=%llu",
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

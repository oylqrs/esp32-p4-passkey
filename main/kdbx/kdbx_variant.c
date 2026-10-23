#include "kdbx_internal.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "kdbx";

#define KDBX_WDT_RESET_INTERVAL_US  15000
#define KDBX_IDLE_RUN_INTERVAL_US   100000

uint32_t kdbx_read_le32(const uint8_t *buf)
{
    return (uint32_t)buf[0] |
           ((uint32_t)buf[1] << 8) |
           ((uint32_t)buf[2] << 16) |
           ((uint32_t)buf[3] << 24);
}

uint64_t kdbx_read_le64(const uint8_t *buf)
{
    return (uint64_t)buf[0] |
           ((uint64_t)buf[1] << 8) |
           ((uint64_t)buf[2] << 16) |
           ((uint64_t)buf[3] << 24) |
           ((uint64_t)buf[4] << 32) |
           ((uint64_t)buf[5] << 40) |
           ((uint64_t)buf[6] << 48) |
           ((uint64_t)buf[7] << 56);
}

static void write_le64(uint8_t out[8], uint64_t value)
{
    for (int i = 0; i < 8; i++) {
        out[i] = (uint8_t)(value >> (i * 8));
    }
}

void kdbx_crypto_cooperate(void)
{
    static int64_t last_wdt_reset_us;
    static int64_t last_idle_release_us;
    int64_t now_us = esp_timer_get_time();

    if (now_us - last_wdt_reset_us >= KDBX_WDT_RESET_INTERVAL_US) {
        last_wdt_reset_us = now_us;
        if (esp_task_wdt_status(NULL) == ESP_OK) {
            esp_task_wdt_reset();
        }
    }

    if (now_us - last_idle_release_us >= KDBX_IDLE_RUN_INTERVAL_US) {
        last_idle_release_us = now_us;
        /* taskYIELD() cannot run the lower-priority IDLE task watched by TWDT. */
        vTaskDelay(1);
    }
}

void kdbx_log_hex_value(const char *label, const uint8_t *data, size_t len)
{
#if KDBX_VERBOSE_LOG
    char hex[65];
    size_t bytes = len < 16 ? len : 16;
    for (size_t i = 0; i < bytes; i++) {
        snprintf(hex + (i * 2), sizeof(hex) - (i * 2), "%02x", data[i]);
    }
    hex[bytes * 2] = '\0';

    ESP_LOGI(TAG, "%s len=%u first=%s%s",
             label,
             (unsigned int)len,
             hex,
             len > bytes ? "..." : "");
#else
    (void)label;
    (void)data;
    (void)len;
#endif
}

int64_t kdbx_now_us(void)
{
    return esp_timer_get_time();
}

void kdbx_log_elapsed(const char *label, int64_t start_us)
{
    int64_t elapsed_us = esp_timer_get_time() - start_us;
    KDBX_LOGD(TAG, "KDBX timing: %s took %lld us (%lld ms)",
             label,
             (long long)elapsed_us,
             (long long)(elapsed_us / 1000));
}

static esp_err_t digest_bytes(mbedtls_md_type_t type, const uint8_t *data, size_t len, uint8_t *out)
{
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(type);
    if (!info) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);

    int ret = mbedtls_md_setup(&ctx, info, 0);
    if (ret == 0) {
        ret = mbedtls_md_starts(&ctx);
    }

    for (size_t offset = 0; ret == 0 && offset < len;) {
        size_t chunk_len = len - offset;
        if (chunk_len > 4096) {
            chunk_len = 4096;
        }

        ret = mbedtls_md_update(&ctx, data + offset, chunk_len);
        offset += chunk_len;
        if (ret == 0 && offset < len) {
            kdbx_crypto_cooperate();
        }
    }

    if (ret == 0) {
        ret = mbedtls_md_finish(&ctx, out);
    }

    mbedtls_md_free(&ctx);
    return ret == 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t kdbx_sha256_bytes(const uint8_t *data, size_t len, uint8_t out[32])
{
    return digest_bytes(MBEDTLS_MD_SHA256, data, len, out);
}

esp_err_t kdbx_sha512_bytes(const uint8_t *data, size_t len, uint8_t out[64])
{
    return digest_bytes(MBEDTLS_MD_SHA512, data, len, out);
}

esp_err_t kdbx_hmac_sha256(const uint8_t *key, size_t key_len,
                           const uint8_t *data, size_t data_len,
                           uint8_t out[32])
{
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!info) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);

    int ret = mbedtls_md_setup(&ctx, info, 1);
    if (ret == 0) {
        ret = mbedtls_md_hmac_starts(&ctx, key, key_len);
    }

    for (size_t offset = 0; ret == 0 && offset < data_len;) {
        size_t chunk_len = data_len - offset;
        if (chunk_len > 4096) {
            chunk_len = 4096;
        }

        ret = mbedtls_md_hmac_update(&ctx, data + offset, chunk_len);
        offset += chunk_len;
        if (ret == 0 && offset < data_len) {
            kdbx_crypto_cooperate();
        }
    }

    if (ret == 0) {
        ret = mbedtls_md_hmac_finish(&ctx, out);
    }

    mbedtls_md_free(&ctx);
    return ret == 0 ? ESP_OK : ESP_FAIL;
}

static esp_err_t kdbx_hmac_block_key(const uint8_t hmac_base_key[64],
                                     uint64_t block_index,
                                     uint8_t out_key[64])
{
    uint8_t input[72];
    write_le64(input, block_index);
    memcpy(input + 8, hmac_base_key, 64);
    return kdbx_sha512_bytes(input, sizeof(input), out_key);
}

esp_err_t kdbx_header_hmac(const uint8_t hmac_base_key[64],
                           const uint8_t *header_bytes,
                           size_t header_len,
                           uint8_t out_hmac[32])
{
    uint8_t header_key[64];
    esp_err_t ret = kdbx_hmac_block_key(hmac_base_key, UINT64_MAX, header_key);
    if (ret != ESP_OK) {
        return ret;
    }

    return kdbx_hmac_sha256(header_key, sizeof(header_key), header_bytes, header_len, out_hmac);
}

esp_err_t kdbx_parse_kdf_parameters(const uint8_t *data, size_t len, kdbx_header_t *header)
{
    if (len < 2) {
        ESP_LOGW(TAG, "KDF parameters too short: %u", (unsigned int)len);
        return ESP_ERR_INVALID_SIZE;
    }

    uint16_t version = (uint16_t)data[0] | ((uint16_t)data[1] << 8);
    if (version != 0x0100) {
        ESP_LOGW(TAG, "Unsupported KDF parameters version=0x%04x", version);
        return ESP_ERR_NOT_SUPPORTED;
    }

    size_t offset = 2;
    KDBX_LOGD(TAG, "KDF parameters version=0x%04x", version);

    while (offset < len) {
        uint8_t value_type = data[offset++];
        if (value_type == 0) {
            KDBX_LOGD(TAG, "KDF parameters end");
            if (offset != len) {
                ESP_LOGW(TAG, "KDF parameters contain %u trailing bytes",
                         (unsigned int)(len - offset));
                return ESP_ERR_INVALID_SIZE;
            }
            return ESP_OK;
        }

        if (len - offset < 4) {
            ESP_LOGW(TAG, "KDF parameter truncated before key length");
            return ESP_ERR_INVALID_SIZE;
        }
        uint32_t key_len = kdbx_read_le32(data + offset);
        offset += 4;
        if (key_len >= 32 || key_len > len - offset || len - offset - key_len < 4) {
            ESP_LOGW(TAG, "KDF parameter invalid key length=%lu", (unsigned long)key_len);
            return ESP_ERR_INVALID_SIZE;
        }

        char key[32];
        memcpy(key, data + offset, key_len);
        key[key_len] = '\0';
        offset += key_len;

        uint32_t value_len = kdbx_read_le32(data + offset);
        offset += 4;
        if (value_len > len - offset) {
            ESP_LOGW(TAG, "KDF parameter '%s' truncated value length=%lu", key, (unsigned long)value_len);
            return ESP_ERR_INVALID_SIZE;
        }

        const uint8_t *value = data + offset;
        if (strcmp(key, "$UUID") == 0 && value_type == 0x42 && value_len == sizeof(header->kdf_uuid)) {
            memcpy(header->kdf_uuid, value, sizeof(header->kdf_uuid));
            header->has_kdf_uuid = true;
        } else if (strcmp(key, "S") == 0 && value_type == 0x42 && value_len == sizeof(header->kdf_seed)) {
            memcpy(header->kdf_seed, value, sizeof(header->kdf_seed));
            header->has_kdf_seed = true;
        } else if (strcmp(key, "R") == 0 && (value_type == 0x05 || value_type == 0x0d) && value_len == 8) {
            header->kdf_rounds = kdbx_read_le64(value);
            header->has_kdf_rounds = true;
            KDBX_LOGD(TAG, "KDBX AES-KDF rounds=%llu", (unsigned long long)header->kdf_rounds);
        }

#if KDBX_VERBOSE_LOG
        if ((value_type == 0x04 || value_type == 0x0c) && value_len == 4) {
            ESP_LOGI(TAG, "KDF %s type=0x%02x value=%lu",
                     key,
                     value_type,
                     (unsigned long)kdbx_read_le32(value));
        } else if ((value_type == 0x05 || value_type == 0x0d) && value_len == 8) {
            ESP_LOGI(TAG, "KDF %s type=0x%02x value=%llu",
                     key,
                     value_type,
                     (unsigned long long)kdbx_read_le64(value));
        } else if (value_type == 0x08 && value_len == 1) {
            ESP_LOGI(TAG, "KDF %s type=0x%02x value=%u", key, value_type, value[0]);
        } else if (value_type == 0x18) {
            char text[64];
            size_t copy_len = value_len < sizeof(text) - 1 ? value_len : sizeof(text) - 1;
            memcpy(text, value, copy_len);
            text[copy_len] = '\0';
            ESP_LOGI(TAG, "KDF %s type=0x%02x value='%s'%s",
                     key,
                     value_type,
                     text,
                     value_len > copy_len ? "..." : "");
        } else {
            char label[48];
            snprintf(label, sizeof(label), "KDF %s type=0x%02x", key, value_type);
            kdbx_log_hex_value(label, value, value_len);
        }
#endif

        offset += value_len;
    }

    ESP_LOGW(TAG, "KDF parameters missing end marker");
    return ESP_ERR_INVALID_SIZE;
}

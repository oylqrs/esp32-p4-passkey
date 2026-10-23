#include "kdbx_internal.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"

static const char *TAG = "kdbx";
#define KDBX_FILE_VERSION_4_0 0x00040000UL
#define KDBX_FILE_VERSION_4_1 0x00040001UL

static uint16_t s_file_version_major;
static uint16_t s_file_version_minor;
static bool s_file_version_valid;

bool kdbx_get_file_version(uint16_t *major, uint16_t *minor)
{
    if (!s_file_version_valid) {
        return false;
    }

    if (major) {
        *major = s_file_version_major;
    }
    if (minor) {
        *minor = s_file_version_minor;
    }
    return true;
}

static const char *header_field_name(uint8_t field_id)
{
    switch (field_id) {
    case 0:
        return "EndOfHeader";
    case 2:
        return "CipherID";
    case 3:
        return "CompressionFlags";
    case 4:
        return "MasterSeed";
    case 7:
        return "EncryptionIV";
    case 11:
        return "KdfParameters";
    case 12:
        return "PublicCustomData";
    default:
        return "Unknown";
    }
}

static esp_err_t parse_common_header_field(uint8_t field_id,
                                           const uint8_t *data,
                                           size_t len,
                                           kdbx_header_t *header)
{
    switch (field_id) {
    case 2:
        if (len != sizeof(header->cipher_id)) {
            ESP_LOGW(TAG, "Invalid CipherID size=%u", (unsigned int)len);
            return ESP_ERR_INVALID_SIZE;
        }
        memcpy(header->cipher_id, data, sizeof(header->cipher_id));
        header->has_cipher_id = true;
        kdbx_log_hex_value(header_field_name(field_id), data, len);
        break;
    case 4:
        if (len == sizeof(header->master_seed)) {
            memcpy(header->master_seed, data, sizeof(header->master_seed));
            header->has_master_seed = true;
        }
        kdbx_log_hex_value(header_field_name(field_id), data, len);
        break;
    case 7:
        if (len != 12 && len != sizeof(header->encryption_iv)) {
            ESP_LOGW(TAG, "Unsupported EncryptionIV size=%u", (unsigned int)len);
            return ESP_ERR_INVALID_SIZE;
        }
        memcpy(header->encryption_iv, data, len);
        header->encryption_iv_len = len;
        header->has_encryption_iv = true;
        kdbx_log_hex_value(header_field_name(field_id), data, len);
        break;
    case 3:
        if (len == 4) {
            header->compression_flags = kdbx_read_le32(data);
            header->has_compression_flags = true;
            ESP_LOGI(TAG, "CompressionFlags value=%lu", (unsigned long)header->compression_flags);
        } else {
            kdbx_log_hex_value(header_field_name(field_id), data, len);
        }
        break;
    case 11:
        return kdbx_parse_kdf_parameters(data, len, header);
    default:
        break;
    }

    return ESP_OK;
}

static esp_err_t parse_kdbx_4_0_header_field(uint8_t field_id,
                                             const uint8_t *data,
                                             size_t len,
                                             kdbx_header_t *header)
{
    if (field_id == 12) {
        ESP_LOGW(TAG, "PublicCustomData is not valid in KDBX 4.0");
        return ESP_ERR_INVALID_RESPONSE;
    }

    return parse_common_header_field(field_id, data, len, header);
}

static esp_err_t parse_kdbx_4_1_header_field(uint8_t field_id,
                                             const uint8_t *data,
                                             size_t len,
                                             kdbx_header_t *header)
{
    if (field_id == 12) {
        if (len < 3) {
            ESP_LOGW(TAG, "KDBX 4.1 PublicCustomData is too short: %u", (unsigned int)len);
            return ESP_ERR_INVALID_SIZE;
        }

        uint16_t dictionary_version = (uint16_t)data[0] | ((uint16_t)data[1] << 8);
        header->has_public_custom_data = true;
        ESP_LOGI(TAG, "KDBX 4.1 PublicCustomData version=0x%04x size=%u",
                 dictionary_version,
                 (unsigned int)len);
        return ESP_OK;
    }

    return parse_common_header_field(field_id, data, len, header);
}

typedef esp_err_t (*kdbx_header_field_parser_t)(uint8_t field_id,
                                                 const uint8_t *data,
                                                 size_t len,
                                                 kdbx_header_t *header);

esp_err_t kdbx_parse_file(const char *path,
                          const char *password,
                          kdbx_entry_t *entries,
                          size_t *entry_count,
                          size_t max_entries)
{
    s_file_version_valid = false;

    int64_t total_start_us = kdbx_now_us();
    int64_t step_start_us = kdbx_now_us();
    kdbx_header_t header = {0};
    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGW(TAG, "Failed to open KDBX %s: errno=%d", path, errno);
        kdbx_log_elapsed("open KDBX file failed", total_start_us);
        return ESP_ERR_NOT_FOUND;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        ESP_LOGW(TAG, "Failed to determine KDBX file size: %s", path);
        fclose(f);
        return ESP_FAIL;
    }
    long file_size = ftell(f);
    if (file_size <= 0 || (unsigned long)file_size > KDBX_MAX_FILE_SIZE) {
        ESP_LOGW(TAG, "KDBX file size out of range: %ld (max %u)",
                 file_size, (unsigned int)KDBX_MAX_FILE_SIZE);
        fclose(f);
        return ESP_ERR_INVALID_SIZE;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        ESP_LOGW(TAG, "Failed to rewind KDBX file: %s", path);
        fclose(f);
        return ESP_FAIL;
    }
    kdbx_log_elapsed("open KDBX file", step_start_us);

    step_start_us = kdbx_now_us();
    uint8_t fixed[12];
    if (fread(fixed, 1, sizeof(fixed), f) != sizeof(fixed)) {
        ESP_LOGW(TAG, "KDBX header too short: %s", path);
        fclose(f);
        kdbx_log_elapsed("read fixed KDBX header failed", total_start_us);
        return ESP_ERR_INVALID_SIZE;
    }

    uint32_t sig1 = kdbx_read_le32(&fixed[0]);
    uint32_t sig2 = kdbx_read_le32(&fixed[4]);
    uint32_t version = kdbx_read_le32(&fixed[8]);
    uint16_t minor = version & 0xffff;
    uint16_t major = version >> 16;

    if (sig1 != 0x9AA2D903 || sig2 != 0xB54BFB67) {
        ESP_LOGW(TAG, "Invalid KDBX signature: sig1=0x%08lx sig2=0x%08lx",
                 (unsigned long)sig1,
                 (unsigned long)sig2);
        fclose(f);
        kdbx_log_elapsed("read fixed KDBX header invalid signature", total_start_us);
        return ESP_ERR_INVALID_RESPONSE;
    }

    kdbx_header_field_parser_t parse_header_field;
    switch (version) {
    case KDBX_FILE_VERSION_4_0:
        parse_header_field = parse_kdbx_4_0_header_field;
        ESP_LOGI(TAG, "Selected KDBX 4.0 parser");
        break;
    case KDBX_FILE_VERSION_4_1:
        parse_header_field = parse_kdbx_4_1_header_field;
        ESP_LOGI(TAG, "Selected KDBX 4.1 parser");
        break;
    default:
        ESP_LOGW(TAG, "Unsupported KDBX version: major=%u minor=%u", major, minor);
        fclose(f);
        kdbx_log_elapsed("read fixed KDBX header unsupported version", total_start_us);
        return ESP_ERR_NOT_SUPPORTED;
    }
    kdbx_log_elapsed("read fixed KDBX header", step_start_us);

    ESP_LOGI(TAG, "KDBX 4.%u header OK: %s password_len=%u",
             minor,
             path,
             (unsigned int)strlen(password));

    step_start_us = kdbx_now_us();
    unsigned int field_count = 0;
    for (;;) {
        uint8_t field_id;
        uint8_t size_buf[4];
        if (fread(&field_id, 1, 1, f) != 1 || fread(size_buf, 1, sizeof(size_buf), f) != sizeof(size_buf)) {
            ESP_LOGW(TAG, "Unexpected EOF while reading KDBX header fields");
            fclose(f);
            kdbx_log_elapsed("read KDBX header fields failed", total_start_us);
            return ESP_ERR_INVALID_SIZE;
        }
        field_count++;

        uint32_t field_size = kdbx_read_le32(size_buf);
        ESP_LOGI(TAG, "KDBX header field id=%u (%s) size=%lu",
                 field_id,
                 header_field_name(field_id),
                 (unsigned long)field_size);

        if (field_size > KDBX_MAX_HEADER_FIELD_SIZE) {
            ESP_LOGW(TAG, "KDBX header field id=%u is too large: %lu (max %u)",
                     field_id,
                     (unsigned long)field_size,
                     (unsigned int)KDBX_MAX_HEADER_FIELD_SIZE);
            fclose(f);
            return ESP_ERR_INVALID_SIZE;
        }

        uint8_t *field_data = NULL;
        if (field_size > 0) {
            field_data = malloc(field_size);
            if (!field_data) {
                ESP_LOGW(TAG, "No memory for KDBX header field id=%u size=%lu",
                         field_id,
                         (unsigned long)field_size);
                fclose(f);
                kdbx_log_elapsed("read KDBX header fields failed no memory", total_start_us);
                return ESP_ERR_NO_MEM;
            }

            if (fread(field_data, 1, field_size, f) != field_size) {
                ESP_LOGW(TAG, "Unexpected EOF while reading KDBX header field id=%u", field_id);
                free(field_data);
                fclose(f);
                kdbx_log_elapsed("read KDBX header fields failed EOF", total_start_us);
                return ESP_ERR_INVALID_SIZE;
            }

        }

        esp_err_t field_ret = parse_header_field(field_id, field_data, field_size, &header);
        free(field_data);
        if (field_ret != ESP_OK) {
            fclose(f);
            kdbx_log_elapsed("parse KDBX version-specific header field failed", total_start_us);
            return field_ret;
        }

        if (field_id == 0) {
            break;
        }

        if (field_size == 0 && ferror(f)) {
            ESP_LOGW(TAG, "Failed to read KDBX header field id=%u", field_id);
            fclose(f);
            kdbx_log_elapsed("read KDBX header fields failed file error", total_start_us);
            return ESP_FAIL;
        }
    }
    ESP_LOGI(TAG, "KDBX timing: header fields count=%u", field_count);
    kdbx_log_elapsed("read/parse KDBX header fields", step_start_us);

    step_start_us = kdbx_now_us();
    long header_len_long = ftell(f);
    if (header_len_long <= 0) {
        fclose(f);
        kdbx_log_elapsed("prepare KDBX header bytes failed ftell", total_start_us);
        return ESP_FAIL;
    }

    size_t header_len = (size_t)header_len_long;
    uint8_t *header_bytes = malloc(header_len);
    if (!header_bytes) {
        fclose(f);
        kdbx_log_elapsed("read KDBX header bytes failed no memory", total_start_us);
        return ESP_ERR_NO_MEM;
    }

    if (fseek(f, 0, SEEK_SET) != 0 || fread(header_bytes, 1, header_len, f) != header_len) {
        ESP_LOGW(TAG, "Failed to read KDBX header bytes for hash calculation");
        free(header_bytes);
        fclose(f);
        kdbx_log_elapsed("read KDBX header bytes failed", total_start_us);
        return ESP_FAIL;
    }

    if (fseek(f, header_len_long, SEEK_SET) != 0) {
        free(header_bytes);
        fclose(f);
        kdbx_log_elapsed("restore KDBX payload seek failed", total_start_us);
        return ESP_FAIL;
    }
    kdbx_log_elapsed("read KDBX header bytes for hash", step_start_us);

    uint8_t master_key[32];
    uint8_t hmac_base_key[64];
    step_start_us = kdbx_now_us();
    esp_err_t ret = kdbx_derive_aes_kdf_master_key(&header, password, master_key, hmac_base_key);
    kdbx_log_elapsed("derive KDBX master keys", step_start_us);
    if (ret != ESP_OK) {
        free(header_bytes);
        fclose(f);
        kdbx_log_elapsed("parse KDBX file total failed KDF", total_start_us);
        return ret;
    }

    step_start_us = kdbx_now_us();
    ret = kdbx_decrypt_payload(f,
                               &header,
                               master_key,
                               hmac_base_key,
                               header_bytes,
                               header_len,
                               entries,
                               entry_count,
                               max_entries);
    kdbx_log_elapsed("decrypt/parse KDBX payload", step_start_us);
    free(header_bytes);
    fclose(f);
    if (ret != ESP_OK) {
        kdbx_log_elapsed("parse KDBX file total failed payload", total_start_us);
        return ret;
    }

    s_file_version_major = major;
    s_file_version_minor = minor;
    s_file_version_valid = true;
    ESP_LOGI(TAG, "KDBX payload decrypt and entry parse completed");
    kdbx_log_elapsed("parse KDBX file total", total_start_us);
    return ESP_OK;
}

#include "kdbx_internal.h"

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "mbedtls/aes.h"
#include "mbedtls/base64.h"
#include "mbedtls/chacha20.h"
#include "miniz.h"

static const char *TAG = "kdbx";

typedef struct {
    bool ready;
    mbedtls_chacha20_context chacha20;
} kdbx_protected_stream_t;

typedef struct {
    kdbx_entry_t *entries;
    size_t *entry_count;
    size_t max_entries;
} kdbx_entry_sink_t;

typedef struct {
    size_t index;
} kdbx_group_stack_item_t;

static esp_err_t reserve_bytes(uint8_t **buffer, size_t *capacity, size_t needed_len)
{
    if (needed_len > KDBX_MAX_ENCRYPTED_PAYLOAD_SIZE) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (needed_len <= *capacity) {
        return ESP_OK;
    }

    size_t new_capacity = *capacity == 0 ? 1024 : *capacity;
    while (new_capacity < needed_len) {
        if (new_capacity > KDBX_MAX_ENCRYPTED_PAYLOAD_SIZE / 2U) {
            new_capacity = KDBX_MAX_ENCRYPTED_PAYLOAD_SIZE;
            break;
        }
        new_capacity *= 2U;
    }

    uint8_t *new_buffer = realloc(*buffer, new_capacity);
    if (!new_buffer) {
        return ESP_ERR_NO_MEM;
    }

    *buffer = new_buffer;
    *capacity = new_capacity;
    return ESP_OK;
}

static esp_err_t read_hmac_block_stream(FILE *f, uint8_t **out_data, size_t *out_len)
{
    int64_t start_us = kdbx_now_us();
    uint8_t *buffer = NULL;
    size_t len = 0;
    size_t capacity = 0;
    uint32_t data_block_count = 0;

    for (uint32_t block_index = 0;; block_index++) {
        uint8_t hmac[32];
        uint8_t size_buf[4];
        if (fread(hmac, 1, sizeof(hmac), f) != sizeof(hmac) ||
            fread(size_buf, 1, sizeof(size_buf), f) != sizeof(size_buf)) {
            ESP_LOGW(TAG, "Unexpected EOF while reading HMAC block %lu", (unsigned long)block_index);
            free(buffer);
            kdbx_log_elapsed("read HMAC block stream failed EOF before block header", start_us);
            return ESP_ERR_INVALID_SIZE;
        }

        uint32_t block_size = kdbx_read_le32(size_buf);
#if KDBX_VERBOSE_LOG
        ESP_LOGI(TAG, "KDBX HMAC block %lu size=%lu",
                 (unsigned long)block_index,
                 (unsigned long)block_size);
#endif

        if (block_size == 0) {
            break;
        }
        data_block_count++;

        if (block_size > KDBX_MAX_HMAC_BLOCK_SIZE ||
            len > KDBX_MAX_ENCRYPTED_PAYLOAD_SIZE - block_size) {
            ESP_LOGW(TAG, "KDBX HMAC payload is too large: block=%lu accumulated=%u max=%u",
                     (unsigned long)block_size,
                     (unsigned int)len,
                     (unsigned int)KDBX_MAX_ENCRYPTED_PAYLOAD_SIZE);
            free(buffer);
            return ESP_ERR_INVALID_SIZE;
        }

        esp_err_t ret = reserve_bytes(&buffer, &capacity, len + block_size);
        if (ret != ESP_OK) {
            free(buffer);
            kdbx_log_elapsed("read HMAC block stream failed reserve", start_us);
            return ret;
        }

        if (fread(buffer + len, 1, block_size, f) != block_size) {
            ESP_LOGW(TAG, "Unexpected EOF while reading HMAC block data %lu", (unsigned long)block_index);
            free(buffer);
            kdbx_log_elapsed("read HMAC block stream failed EOF in block data", start_us);
            return ESP_ERR_INVALID_SIZE;
        }
        len += block_size;

        kdbx_crypto_cooperate();
    }

    *out_data = buffer;
    *out_len = len;
    KDBX_LOGD(TAG, "KDBX timing: HMAC block stream data_blocks=%lu bytes=%u",
             (unsigned long)data_block_count,
             (unsigned int)len);
    kdbx_log_elapsed("read HMAC block stream", start_us);
    return ESP_OK;
}

static esp_err_t decrypt_aes_cbc_payload(const kdbx_header_t *header,
                                         const uint8_t master_key[32],
                                         const uint8_t *encrypted,
                                         size_t encrypted_len,
                                         uint8_t **out_plain,
                                         size_t *out_plain_len)
{
    int64_t start_us = kdbx_now_us();
    if (!header->has_encryption_iv || header->encryption_iv_len != 16) {
        ESP_LOGW(TAG, "AES-CBC requires a 16-byte IV, got %u",
                 (unsigned int)header->encryption_iv_len);
        kdbx_log_elapsed("AES-CBC payload decrypt failed missing IV", start_us);
        return ESP_ERR_INVALID_STATE;
    }

    if (encrypted_len == 0 || (encrypted_len % 16) != 0) {
        ESP_LOGW(TAG, "Encrypted payload size is not AES block aligned: %u", (unsigned int)encrypted_len);
        kdbx_log_elapsed("AES-CBC payload decrypt failed invalid size", start_us);
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t *plain = malloc(encrypted_len);
    if (!plain) {
        kdbx_log_elapsed("AES-CBC payload decrypt failed no memory", start_us);
        return ESP_ERR_NO_MEM;
    }

    uint8_t iv[16];
    memcpy(iv, header->encryption_iv, sizeof(iv));

    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    int ret = mbedtls_aes_setkey_dec(&aes, master_key, 256);
    for (size_t offset = 0; ret == 0 && offset < encrypted_len;) {
        size_t chunk_len = encrypted_len - offset;
        if (chunk_len > 4096) {
            chunk_len = 4096;
        }
        chunk_len &= ~(size_t)0x0f;
        if (chunk_len == 0) {
            chunk_len = 16;
        }

        ret = mbedtls_aes_crypt_cbc(&aes,
                                    MBEDTLS_AES_DECRYPT,
                                    chunk_len,
                                    iv,
                                    encrypted + offset,
                                    plain + offset);
        offset += chunk_len;
        if (ret == 0 && offset < encrypted_len) {
            kdbx_crypto_cooperate();
        }
    }
    mbedtls_aes_free(&aes);

    if (ret != 0) {
        free(plain);
        ESP_LOGW(TAG, "AES-CBC payload decrypt failed: %d", ret);
        kdbx_log_elapsed("AES-CBC payload decrypt failed mbedtls", start_us);
        return ESP_FAIL;
    }

    size_t plain_len = encrypted_len;
    uint8_t pad = plain[plain_len - 1];
    if (pad == 0 || pad > 16 || pad > plain_len) {
        kdbx_log_hex_value("Decrypted payload first bytes", plain, plain_len);
        free(plain);
        ESP_LOGW(TAG, "Invalid PKCS#7 padding: %u", pad);
        kdbx_log_elapsed("AES-CBC payload decrypt failed padding", start_us);
        return ESP_ERR_INVALID_RESPONSE;
    }

    for (uint8_t i = 0; i < pad; i++) {
        if (plain[plain_len - 1 - i] != pad) {
            free(plain);
            ESP_LOGW(TAG, "Invalid PKCS#7 padding byte at %u", i);
            kdbx_log_elapsed("AES-CBC payload decrypt failed padding byte", start_us);
            return ESP_ERR_INVALID_RESPONSE;
        }
    }
    plain_len -= pad;
    plain[plain_len] = '\0';

    *out_plain = plain;
    *out_plain_len = plain_len;
    KDBX_LOGD(TAG, "KDBX timing: AES-CBC encrypted=%u plain=%u padding=%u",
             (unsigned int)encrypted_len,
             (unsigned int)plain_len,
             pad);
    kdbx_log_elapsed("AES-CBC payload decrypt", start_us);
    return ESP_OK;
}

static esp_err_t decrypt_chacha20_payload(const kdbx_header_t *header,
                                          const uint8_t master_key[32],
                                          const uint8_t *encrypted,
                                          size_t encrypted_len,
                                          uint8_t **out_plain,
                                          size_t *out_plain_len)
{
    int64_t start_us = kdbx_now_us();
    if (!header->has_encryption_iv || header->encryption_iv_len != 12) {
        ESP_LOGW(TAG, "ChaCha20 requires a 12-byte nonce, got %u",
                 (unsigned int)header->encryption_iv_len);
        return ESP_ERR_INVALID_STATE;
    }
    if (encrypted_len == 0) {
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t *plain = malloc(encrypted_len + 1);
    if (!plain) {
        return ESP_ERR_NO_MEM;
    }

    mbedtls_chacha20_context chacha20;
    mbedtls_chacha20_init(&chacha20);
    int ret = mbedtls_chacha20_setkey(&chacha20, master_key);
    if (ret == 0) {
        ret = mbedtls_chacha20_starts(&chacha20, header->encryption_iv, 0);
    }

    for (size_t offset = 0; ret == 0 && offset < encrypted_len;) {
        size_t chunk_len = encrypted_len - offset;
        if (chunk_len > 4096) {
            chunk_len = 4096;
        }
        ret = mbedtls_chacha20_update(&chacha20,
                                      chunk_len,
                                      encrypted + offset,
                                      plain + offset);
        offset += chunk_len;
        if (ret == 0 && offset < encrypted_len) {
            kdbx_crypto_cooperate();
        }
    }
    mbedtls_chacha20_free(&chacha20);

    if (ret != 0) {
        free(plain);
        ESP_LOGW(TAG, "ChaCha20 payload decrypt failed: %d", ret);
        return ESP_FAIL;
    }

    plain[encrypted_len] = '\0';
    *out_plain = plain;
    *out_plain_len = encrypted_len;
    KDBX_LOGD(TAG, "KDBX timing: ChaCha20 encrypted=%u plain=%u",
              (unsigned int)encrypted_len,
              (unsigned int)encrypted_len);
    kdbx_log_elapsed("ChaCha20 payload decrypt", start_us);
    return ESP_OK;
}

static bool cipher_id_equals(const kdbx_header_t *header, const uint8_t expected[16])
{
    return header->has_cipher_id &&
           memcmp(header->cipher_id, expected, sizeof(header->cipher_id)) == 0;
}

static esp_err_t decrypt_payload_bytes(const kdbx_header_t *header,
                                       const uint8_t master_key[32],
                                       const uint8_t *encrypted,
                                       size_t encrypted_len,
                                       uint8_t **out_plain,
                                       size_t *out_plain_len)
{
    static const uint8_t aes_cipher_id[16] = {
        0x31, 0xc1, 0xf2, 0xe6, 0xbf, 0x71, 0x43, 0x50,
        0xbe, 0x58, 0x05, 0x21, 0x6a, 0xfc, 0x5a, 0xff,
    };
    static const uint8_t chacha20_cipher_id[16] = {
        0xd6, 0x03, 0x8a, 0x2b, 0x8b, 0x6f, 0x4c, 0xb5,
        0xa5, 0x24, 0x33, 0x9a, 0x31, 0xdb, 0xb5, 0x9a,
    };

    if (cipher_id_equals(header, aes_cipher_id)) {
        KDBX_LOGD(TAG, "KDBX outer cipher: AES-256-CBC");
        return decrypt_aes_cbc_payload(header,
                                       master_key,
                                       encrypted,
                                       encrypted_len,
                                       out_plain,
                                       out_plain_len);
    }
    if (cipher_id_equals(header, chacha20_cipher_id)) {
        KDBX_LOGD(TAG, "KDBX outer cipher: ChaCha20");
        return decrypt_chacha20_payload(header,
                                        master_key,
                                        encrypted,
                                        encrypted_len,
                                        out_plain,
                                        out_plain_len);
    }

    ESP_LOGW(TAG, "Unsupported or missing KDBX outer cipher");
    return ESP_ERR_NOT_SUPPORTED;
}

static esp_err_t protected_stream_init(kdbx_protected_stream_t *stream, const kdbx_inner_header_t *inner)
{
    memset(stream, 0, sizeof(*stream));

    if (inner->random_stream_id != 3 || !inner->has_random_stream_key || inner->random_stream_key_len == 0) {
        ESP_LOGW(TAG, "Unsupported or missing protected stream: id=%lu key_len=%u",
                 (unsigned long)inner->random_stream_id,
                 (unsigned int)inner->random_stream_key_len);
        return ESP_ERR_NOT_SUPPORTED;
    }

    uint8_t key_material[64];
    esp_err_t ret = kdbx_sha512_bytes(inner->random_stream_key, inner->random_stream_key_len, key_material);
    if (ret != ESP_OK) {
        return ret;
    }

    mbedtls_chacha20_init(&stream->chacha20);
    int chacha_ret = mbedtls_chacha20_setkey(&stream->chacha20, key_material);
    if (chacha_ret == 0) {
        chacha_ret = mbedtls_chacha20_starts(&stream->chacha20, key_material + 32, 0);
    }

    if (chacha_ret != 0) {
        mbedtls_chacha20_free(&stream->chacha20);
        ESP_LOGW(TAG, "Failed to init ChaCha20 protected stream: %d", chacha_ret);
        return ESP_FAIL;
    }

    stream->ready = true;
    KDBX_LOGD(TAG, "KDBX protected stream initialized: ChaCha20");
    return ESP_OK;
}

static void protected_stream_free(kdbx_protected_stream_t *stream)
{
    if (stream->ready) {
        mbedtls_chacha20_free(&stream->chacha20);
        stream->ready = false;
    }
}

static esp_err_t protected_stream_xor(kdbx_protected_stream_t *stream, uint8_t *data, size_t len)
{
    if (!stream->ready) {
        return ESP_ERR_INVALID_STATE;
    }

    int ret = mbedtls_chacha20_update(&stream->chacha20, len, data, data);
    return ret == 0 ? ESP_OK : ESP_FAIL;
}

static esp_err_t decode_protected_value(kdbx_protected_stream_t *stream, char *value, size_t value_len)
{
    size_t encoded_len = strlen(value);
    size_t decoded_len = 0;
    int ret = mbedtls_base64_decode(NULL, 0, &decoded_len, (const unsigned char *)value, encoded_len);
    if (ret != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL || decoded_len == 0) {
        ESP_LOGW(TAG, "Failed to size protected base64 value: %d", ret);
        return ESP_FAIL;
    }

    uint8_t *decoded = malloc(decoded_len + 1);
    if (!decoded) {
        return ESP_ERR_NO_MEM;
    }

    ret = mbedtls_base64_decode(decoded, decoded_len, &decoded_len, (const unsigned char *)value, encoded_len);
    if (ret != 0) {
        ESP_LOGW(TAG, "Failed to decode protected base64 value: %d", ret);
        free(decoded);
        return ESP_FAIL;
    }

    esp_err_t esp_ret = protected_stream_xor(stream, decoded, decoded_len);
    if (esp_ret != ESP_OK) {
        free(decoded);
        return esp_ret;
    }

    size_t copy_len = decoded_len < value_len - 1 ? decoded_len : value_len - 1;
    memcpy(value, decoded, copy_len);
    value[copy_len] = '\0';
    free(decoded);
    return ESP_OK;
}

static esp_err_t parse_inner_header(const uint8_t *plain,
                                    size_t plain_len,
                                    size_t *out_xml_offset,
                                    kdbx_inner_header_t *inner)
{
    memset(inner, 0, sizeof(*inner));
    size_t offset = 0;
    while (offset + 5 <= plain_len) {
        uint8_t field_id = plain[offset++];
        uint32_t field_size = kdbx_read_le32(plain + offset);
        offset += 4;

        if (offset + field_size > plain_len) {
            ESP_LOGW(TAG, "Inner header field id=%u truncated size=%lu", field_id, (unsigned long)field_size);
            return ESP_ERR_INVALID_SIZE;
        }

        KDBX_LOGD(TAG, "KDBX inner header field id=%u size=%lu",
                 field_id,
                 (unsigned long)field_size);

        if (field_id == 0) {
            *out_xml_offset = offset + field_size;
            return ESP_OK;
        }

        if (field_id == 1 && field_size == 4) {
            inner->random_stream_id = kdbx_read_le32(plain + offset);
            KDBX_LOGD(TAG, "InnerRandomStreamID=%lu", (unsigned long)inner->random_stream_id);
        } else if (field_id == 2) {
            size_t copy_len = field_size < sizeof(inner->random_stream_key) ? field_size : sizeof(inner->random_stream_key);
            memcpy(inner->random_stream_key, plain + offset, copy_len);
            inner->random_stream_key_len = copy_len;
            inner->has_random_stream_key = true;
            kdbx_log_hex_value("InnerRandomStreamKey", plain + offset, field_size);
        } else if (field_id == 3) {
            KDBX_LOGD(TAG, "InnerBinary len=%lu", (unsigned long)field_size);
        }

        offset += field_size;
    }

    return ESP_ERR_INVALID_RESPONSE;
}

static esp_err_t parse_gzip_deflate_range(const uint8_t *gzip,
                                          size_t gzip_len,
                                          const uint8_t **out_deflate,
                                          size_t *out_deflate_len)
{
    if (gzip_len < 18 || gzip[0] != 0x1f || gzip[1] != 0x8b || gzip[2] != 8) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    uint8_t flags = gzip[3];
    size_t offset = 10;

    if (flags & 0x04) {
        if (offset + 2 > gzip_len) {
            return ESP_ERR_INVALID_SIZE;
        }
        uint16_t extra_len = (uint16_t)gzip[offset] | ((uint16_t)gzip[offset + 1] << 8);
        offset += 2 + extra_len;
    }

    if (flags & 0x08) {
        while (offset < gzip_len && gzip[offset++] != 0) {
        }
    }

    if (flags & 0x10) {
        while (offset < gzip_len && gzip[offset++] != 0) {
        }
    }

    if (flags & 0x02) {
        offset += 2;
    }

    if (offset + 8 > gzip_len) {
        return ESP_ERR_INVALID_SIZE;
    }

    *out_deflate = gzip + offset;
    *out_deflate_len = gzip_len - offset - 8;
    return ESP_OK;
}

static esp_err_t inflate_gzip_payload(const uint8_t *gzip,
                                      size_t gzip_len,
                                      uint8_t **out_xml,
                                      size_t *out_xml_len)
{
    int64_t start_us = kdbx_now_us();
    const uint8_t *deflate = NULL;
    size_t deflate_len = 0;
    esp_err_t ret = parse_gzip_deflate_range(gzip, gzip_len, &deflate, &deflate_len);
    if (ret != ESP_OK) {
        kdbx_log_elapsed("inflate gzip payload failed parsing gzip header", start_us);
        return ret;
    }

    uint32_t expected_len = kdbx_read_le32(gzip + gzip_len - 4);
    if (expected_len > 0 && expected_len <= KDBX_MAX_XML_SIZE) {
        uint8_t *xml = malloc((size_t)expected_len + 1);
        if (!xml) {
            kdbx_log_elapsed("inflate gzip payload failed no memory", start_us);
            return ESP_ERR_NO_MEM;
        }

        size_t written = tinfl_decompress_mem_to_mem(xml,
                                                     expected_len,
                                                     deflate,
                                                     deflate_len,
                                                     TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
        if (written != TINFL_DECOMPRESS_MEM_TO_MEM_FAILED) {
            xml[written] = '\0';
            *out_xml = xml;
            *out_xml_len = written;
            KDBX_LOGD(TAG, "KDBX timing: gzip compressed=%u deflate=%u inflated=%u expected=%u",
                     (unsigned int)gzip_len,
                     (unsigned int)deflate_len,
                     (unsigned int)written,
                     (unsigned int)expected_len);
            kdbx_log_elapsed("inflate gzip payload", start_us);
            return ESP_OK;
        }

        free(xml);
    }

    for (size_t capacity = 8192; capacity <= KDBX_MAX_XML_SIZE; capacity *= 2) {
        uint8_t *xml = malloc(capacity + 1);
        if (!xml) {
            kdbx_log_elapsed("inflate gzip payload failed no memory", start_us);
            return ESP_ERR_NO_MEM;
        }

        size_t written = tinfl_decompress_mem_to_mem(xml,
                                                     capacity,
                                                     deflate,
                                                     deflate_len,
                                                     TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
        if (written != TINFL_DECOMPRESS_MEM_TO_MEM_FAILED) {
            xml[written] = '\0';
            *out_xml = xml;
            *out_xml_len = written;
            KDBX_LOGD(TAG, "KDBX timing: gzip compressed=%u deflate=%u inflated=%u capacity=%u",
                     (unsigned int)gzip_len,
                     (unsigned int)deflate_len,
                     (unsigned int)written,
                     (unsigned int)capacity);
            kdbx_log_elapsed("inflate gzip payload", start_us);
            return ESP_OK;
        }

        free(xml);
    }

    kdbx_log_elapsed("inflate gzip payload failed capacity limit", start_us);
    return ESP_ERR_INVALID_SIZE;
}

#if KDBX_VERBOSE_LOG
static size_t count_substrings(const char *text, const char *needle)
{
    size_t count = 0;
    const char *cursor = text;
    while ((cursor = strstr(cursor, needle)) != NULL) {
        count++;
        cursor += strlen(needle);
    }
    return count;
}

static bool looks_like_utf16le_xml(const uint8_t *xml, size_t xml_len)
{
    if (xml_len >= 2 && xml[0] == 0xff && xml[1] == 0xfe) {
        return true;
    }

    return xml_len >= 10 &&
           xml[0] == '<' && xml[1] == 0 &&
           xml[2] == '?' && xml[3] == 0 &&
           xml[4] == 'x' && xml[5] == 0;
}

static size_t count_utf16le_entry_tags(const uint8_t *xml, size_t xml_len)
{
    static const char *needle = "<Entry>";
    size_t count = 0;
    size_t needle_len = strlen(needle);

    for (size_t i = 0; i + needle_len * 2 <= xml_len; i += 2) {
        bool match = true;
        for (size_t j = 0; j < needle_len; j++) {
            if (xml[i + j * 2] != (uint8_t)needle[j] || xml[i + j * 2 + 1] != 0) {
                match = false;
                break;
            }
        }
        if (match) {
            count++;
        }
    }

    return count;
}
#endif

static void log_xml_summary(const uint8_t *xml, size_t xml_len)
{
#if KDBX_VERBOSE_LOG
    kdbx_log_hex_value("KDBX XML first bytes", xml, xml_len);

    if (looks_like_utf16le_xml(xml, xml_len)) {
        size_t preview_units = xml_len / 2 < 160 ? xml_len / 2 : 160;
        char preview[161];
        size_t out = 0;
        size_t offset = (xml_len >= 2 && xml[0] == 0xff && xml[1] == 0xfe) ? 2 : 0;
        for (size_t i = offset; i + 1 < xml_len && out < preview_units; i += 2) {
            char ch = (char)xml[i];
            preview[out++] = (ch == '\r' || ch == '\n' || ch == '\t') ? ' ' : ch;
        }
        preview[out] = '\0';

        ESP_LOGI(TAG, "KDBX XML len=%u encoding=UTF-16LE entries=%u preview='%s'%s",
                 (unsigned int)xml_len,
                 (unsigned int)count_utf16le_entry_tags(xml, xml_len),
                 preview,
                 xml_len / 2 > out ? "..." : "");
        return;
    }

    size_t preview_len = xml_len < 160 ? xml_len : 160;
    char preview[161];
    memcpy(preview, xml, preview_len);
    preview[preview_len] = '\0';
    for (size_t i = 0; i < preview_len; i++) {
        if (preview[i] == '\r' || preview[i] == '\n' || preview[i] == '\t') {
            preview[i] = ' ';
        }
    }

    ESP_LOGI(TAG, "KDBX XML len=%u entries=%u preview='%s'%s",
             (unsigned int)xml_len,
             (unsigned int)count_substrings((const char *)xml, "<Entry>"),
             preview,
             xml_len > preview_len ? "..." : "");
#else
    (void)xml;
    (void)xml_len;
#endif
}

static void log_decrypted_xml(const uint8_t *xml, size_t xml_len)
{
#if KDBX_DUMP_DECRYPTED_XML
    enum { XML_LOG_CHUNK_SIZE = 240 };
    char chunk[XML_LOG_CHUNK_SIZE + 1];

    ESP_LOGW(TAG, "KDBX XML dump enabled; output may contain sensitive data");
    ESP_LOGI(TAG, "========== KDBX XML BEGIN len=%u ==========", (unsigned)xml_len);
    for (size_t offset = 0; offset < xml_len; offset += XML_LOG_CHUNK_SIZE) {
        size_t chunk_len = xml_len - offset;
        if (chunk_len > XML_LOG_CHUNK_SIZE) chunk_len = XML_LOG_CHUNK_SIZE;
        memcpy(chunk, xml + offset, chunk_len);
        chunk[chunk_len] = '\0';

        /* Keep every serial record on one line so offsets remain searchable. */
        for (size_t i = 0; i < chunk_len; i++) {
            if (chunk[i] == '\r' || chunk[i] == '\n' || chunk[i] == '\t') {
                chunk[i] = ' ';
            }
        }
        ESP_LOGI(TAG, "KDBX_XML[%06u]: %s", (unsigned)offset, chunk);
        if (((offset / XML_LOG_CHUNK_SIZE) & 0x07U) == 0x07U) {
            kdbx_crypto_cooperate();
        }
    }
    ESP_LOGI(TAG, "========== KDBX XML END ==========");
#else
    (void)xml;
    (void)xml_len;
#endif
}

static bool copy_between(const char *start,
                         const char *end,
                         const char *open_tag,
                         const char *close_tag,
                         char *out,
                         size_t out_len)
{
    const char *open = strstr(start, open_tag);
    if (!open || open >= end) {
        return false;
    }

    open += strlen(open_tag);
    const char *close = strstr(open, close_tag);
    if (!close || close > end) {
        return false;
    }

    size_t len = (size_t)(close - open);
    if (len >= out_len) {
        len = out_len - 1;
    }

    memcpy(out, open, len);
    out[len] = '\0';
    return true;
}

static bool range_contains(const char *start, const char *end, const char *needle)
{
    size_t range_len = (size_t)(end - start);
    size_t needle_len = strlen(needle);
    if (needle_len == 0 || needle_len > range_len) {
        return false;
    }

    const char *last = end - needle_len;
    for (const char *cursor = start; cursor <= last; cursor++) {
        if (memcmp(cursor, needle, needle_len) == 0) {
            return true;
        }
    }
    return false;
}

static bool copy_xml_value_text(const char *string_start,
                                const char *string_end,
                                char *out,
                                size_t out_len,
                                bool *out_protected)
{
    const char *value_open = strstr(string_start, "<Value");
    if (!value_open || value_open >= string_end) {
        return false;
    }

    const char *value_text = strchr(value_open, '>');
    if (!value_text || value_text >= string_end) {
        return false;
    }

    *out_protected = range_contains(value_open, value_text, "Protected=\"True\"") ||
                     range_contains(value_open, value_text, "Protected=\"true\"") ||
                     range_contains(value_open, value_text, "Protected='True'") ||
                     range_contains(value_open, value_text, "Protected='true'");

    value_text++;
    const char *value_close = strstr(value_text, "</Value>");
    if (!value_close || value_close > string_end) {
        return false;
    }

    size_t len = (size_t)(value_close - value_text);
    if (len >= out_len) {
        len = out_len - 1;
    }

    memcpy(out, value_text, len);
    out[len] = '\0';
    return true;
}

static void xml_unescape_in_place(char *text)
{
    struct {
        const char *escaped;
        char plain;
    } entities[] = {
        {"&amp;", '&'},
        {"&lt;", '<'},
        {"&gt;", '>'},
        {"&quot;", '"'},
        {"&apos;", '\''},
    };

    char *read = text;
    char *write = text;
    while (*read) {
        bool replaced = false;
        for (size_t i = 0; i < sizeof(entities) / sizeof(entities[0]); i++) {
            size_t len = strlen(entities[i].escaped);
            if (strncmp(read, entities[i].escaped, len) == 0) {
                *write++ = entities[i].plain;
                read += len;
                replaced = true;
                break;
            }
        }

        if (!replaced) {
            *write++ = *read++;
        }
    }
    *write = '\0';
}

static const char *find_matching_entry_end(const char *entry_start)
{
    size_t depth = 0;
    const char *cursor = entry_start;
    while ((cursor = strchr(cursor, '<')) != NULL) {
        if (strncmp(cursor, "<Entry>", 7) == 0) {
            depth++;
            cursor += 7;
        } else if (strncmp(cursor, "</Entry>", 8) == 0) {
            if (depth == 0) return NULL;
            depth--;
            if (depth == 0) return cursor;
            cursor += 8;
        } else {
            cursor++;
        }
    }
    return NULL;
}

static void parse_totp_settings(const char *value, uint16_t *period, uint8_t *digits)
{
    char *end = NULL;
    unsigned long parsed_period = strtoul(value, &end, 10);
    if (end == value || (*end != ';' && *end != ',')) return;
    const char *digits_text = end + 1;
    unsigned long parsed_digits = strtoul(digits_text, &end, 10);
    if (end == digits_text || parsed_period == 0 || parsed_period > UINT16_MAX ||
        parsed_digits < 6 || parsed_digits > 8) {
        return;
    }
    *period = (uint16_t)parsed_period;
    *digits = (uint8_t)parsed_digits;
}

static esp_err_t parse_entry_string_fields(const char *entry_start,
                                           const char *entry_end,
                                           unsigned int entry_index,
                                           const char *group_name,
                                           size_t group_index,
                                           kdbx_protected_stream_t *protected_stream,
                                           kdbx_entry_sink_t *sink)
{
#if !KDBX_VERBOSE_LOG
    (void)entry_index;
#endif
    char title[96] = "";
    char username[96] = "";
    char url[128] = "";
    char password[128] = "";
    char otp_uri[256] = "";
    char totp_secret[160] = "";
    char totp_algorithm[16] = "";
    uint16_t totp_period = 0;
    uint8_t totp_digits = 0;
    kdbx_totp_secret_encoding_t totp_secret_encoding = KDBX_TOTP_SECRET_BASE32;
    uint32_t icon_id = 0;
    bool password_protected = false;

    const char *history = strstr(entry_start, "<History>");
    const char *fields_end = history && history < entry_end ? history : entry_end;
    char icon_id_text[16];
    if (copy_between(entry_start, fields_end, "<IconID>", "</IconID>",
                     icon_id_text, sizeof(icon_id_text))) {
        char *icon_id_end = NULL;
        unsigned long parsed_icon_id = strtoul(icon_id_text, &icon_id_end, 10);
        if (icon_id_end != icon_id_text && *icon_id_end == '\0' &&
            parsed_icon_id <= UINT32_MAX) {
            icon_id = (uint32_t)parsed_icon_id;
        }
    }
    const char *cursor = entry_start;
    while ((cursor = strstr(cursor, "<String>")) != NULL && cursor < entry_end) {
        const char *string_end = strstr(cursor, "</String>");
        if (!string_end || string_end > entry_end) {
            break;
        }

        char key[32];
        char value[256];
        bool value_protected = false;
        if (copy_between(cursor, string_end, "<Key>", "</Key>", key, sizeof(key)) &&
            copy_xml_value_text(cursor, string_end, value, sizeof(value), &value_protected)) {
            bool protected_decoded = false;
            if (value_protected) {
                esp_err_t ret = decode_protected_value(protected_stream, value, sizeof(value));
                if (ret == ESP_OK) {
                    protected_decoded = true;
                } else {
                    ESP_LOGW(TAG, "Failed to decode protected value for key '%s': %s", key, esp_err_to_name(ret));
                }
            }

            if (!value_protected || !protected_decoded) {
                xml_unescape_in_place(value);
            }

            if (cursor >= fields_end) {
                cursor = string_end + strlen("</String>");
                continue;
            } else if (strcmp(key, "Title") == 0) {
                strlcpy(title, value, sizeof(title));
            } else if (strcmp(key, "UserName") == 0) {
                strlcpy(username, value, sizeof(username));
            } else if (strcmp(key, "URL") == 0) {
                strlcpy(url, value, sizeof(url));
            } else if (strcmp(key, "Password") == 0) {
                strlcpy(password, value, sizeof(password));
                password_protected = value_protected && !protected_decoded;
            } else if ((!value_protected || protected_decoded) &&
                       (strcasecmp(key, "otp") == 0 || strcasecmp(key, "TOTP") == 0)) {
                if (strncasecmp(value, "otpauth://", 10) == 0) {
                    strlcpy(otp_uri, value, sizeof(otp_uri));
                } else {
                    strlcpy(totp_secret, value, sizeof(totp_secret));
                    totp_secret_encoding = KDBX_TOTP_SECRET_BASE32;
                }
            } else if ((!value_protected || protected_decoded) &&
                       strcmp(key, "TimeOtp-Secret-Base32") == 0) {
                strlcpy(totp_secret, value, sizeof(totp_secret));
                totp_secret_encoding = KDBX_TOTP_SECRET_BASE32;
            } else if ((!value_protected || protected_decoded) &&
                       strcmp(key, "TimeOtp-Secret-Base64") == 0) {
                strlcpy(totp_secret, value, sizeof(totp_secret));
                totp_secret_encoding = KDBX_TOTP_SECRET_BASE64;
            } else if ((!value_protected || protected_decoded) &&
                       strcmp(key, "TimeOtp-Secret-Hex") == 0) {
                strlcpy(totp_secret, value, sizeof(totp_secret));
                totp_secret_encoding = KDBX_TOTP_SECRET_HEX;
            } else if ((!value_protected || protected_decoded) &&
                       (strcasecmp(key, "TOTP Seed") == 0 ||
                        strcasecmp(key, "TOTP Secret") == 0)) {
                strlcpy(totp_secret, value, sizeof(totp_secret));
                totp_secret_encoding = KDBX_TOTP_SECRET_BASE32;
            } else if (strcmp(key, "TimeOtp-Algorithm") == 0) {
                strlcpy(totp_algorithm, value, sizeof(totp_algorithm));
            } else if (strcmp(key, "TimeOtp-Period") == 0) {
                unsigned long parsed = strtoul(value, NULL, 10);
                if (parsed > 0 && parsed <= UINT16_MAX) totp_period = (uint16_t)parsed;
            } else if (strcmp(key, "TimeOtp-Length") == 0) {
                unsigned long parsed = strtoul(value, NULL, 10);
                if (parsed >= 6 && parsed <= 8) totp_digits = (uint8_t)parsed;
            } else if (strcasecmp(key, "TOTP Settings") == 0) {
                parse_totp_settings(value, &totp_period, &totp_digits);
            }
        }

        cursor = string_end + strlen("</String>");
    }

#if KDBX_VERBOSE_LOG
    ESP_LOGI(TAG,
             "KDBX entry %u title='%s' username='%s' url='%s' password_%s_len=%u",
             entry_index,
             title,
             username,
             url,
             password_protected ? "protected" : "plain",
             (unsigned int)strlen(password));
#endif

    if (!kdbx_try_increment_group_entry_count(group_index)) {
        ESP_LOGW(TAG, "KDBX group '%s' exceeds %u entries",
                 group_name ? group_name : "",
                 (unsigned)KDBX_MAX_ENTRIES_PER_GROUP);
        return ESP_ERR_INVALID_SIZE;
    }

    if (*sink->entry_count < sink->max_entries) {
        size_t stored_index = *sink->entry_count;
        kdbx_entry_t *entry = &sink->entries[stored_index];
        const char *entry_title = title[0] ? title : (username[0] ? username : "(no title)");
        memset(entry, 0, sizeof(*entry));
        strlcpy(entry->title, entry_title, sizeof(entry->title));
        strlcpy(entry->username, username, sizeof(entry->username));
        strlcpy(entry->url, url, sizeof(entry->url));
        strlcpy(entry->password, password, sizeof(entry->password));
        strlcpy(entry->group, group_name ? group_name : "", sizeof(entry->group));
        strlcpy(entry->otp_uri, otp_uri, sizeof(entry->otp_uri));
        strlcpy(entry->totp_secret, totp_secret, sizeof(entry->totp_secret));
        strlcpy(entry->totp_algorithm, totp_algorithm, sizeof(entry->totp_algorithm));
        entry->totp_period = totp_period;
        entry->totp_digits = totp_digits;
        entry->totp_secret_encoding = totp_secret_encoding;
        entry->icon_id = icon_id;
        entry->group_index = group_index;
        entry->password_protected = password_protected;
        *sink->entry_count = stored_index + 1;
        kdbx_publish_entry_count(*sink->entry_count);
        if (entry->otp_uri[0] || entry->totp_secret[0]) {
            const char *source = entry->otp_uri[0] ? "otpauth URI" :
                                 entry->totp_secret_encoding == KDBX_TOTP_SECRET_BASE64 ? "Base64" :
                                 entry->totp_secret_encoding == KDBX_TOTP_SECRET_HEX ? "Hex" : "Base32";
            ESP_LOGI(TAG, "KDBX entry %u contains TOTP (%s)", entry_index, source);
        }
        return ESP_OK;
    }

    ESP_LOGW(TAG, "KDBX database exceeds device entry capacity: %u",
             (unsigned)sink->max_entries);
    return ESP_ERR_INVALID_SIZE;
}

static void parse_group_name(const char *group_start, const char *next_child, char *name, size_t name_len)
{
    name[0] = '\0';
    if (copy_between(group_start, next_child, "<Name>", "</Name>", name, name_len)) {
        xml_unescape_in_place(name);
    }
}

static esp_err_t parse_kdbx_entries(const uint8_t *xml,
                                     size_t xml_len,
                                     const kdbx_inner_header_t *inner,
                                     kdbx_entry_t *entries,
                                     size_t *entry_count,
                                     size_t max_entries)
{
    int64_t start_us = kdbx_now_us();
    const char *text = (const char *)xml;
    *entry_count = 0;

    kdbx_entry_sink_t sink = {
        .entries = entries,
        .entry_count = entry_count,
        .max_entries = max_entries,
    };

    kdbx_protected_stream_t protected_stream;
    esp_err_t stream_ret = protected_stream_init(&protected_stream, inner);
    if (stream_ret != ESP_OK) {
        memset(&protected_stream, 0, sizeof(protected_stream));
    }

    unsigned int entry_index = 0;
    size_t parsed_group_count = 0;
    kdbx_group_stack_item_t group_stack[KDBX_MAX_GROUPS] = {0};
    size_t group_depth = 0;
    const char *cursor = text;
    while ((cursor = strchr(cursor, '<')) != NULL) {
        if (strncmp(cursor, "<Group>", 7) == 0) {
            parsed_group_count++;
            if (parsed_group_count > KDBX_MAX_GROUPS) {
                ESP_LOGW(TAG, "KDBX database exceeds group limit: %u",
                         (unsigned)KDBX_MAX_GROUPS);
                protected_stream_free(&protected_stream);
                return ESP_ERR_INVALID_SIZE;
            }
            const char *next_group = strstr(cursor + 7, "<Group>");
            const char *next_entry = strstr(cursor + 7, "<Entry>");
            const char *group_end = strstr(cursor + 7, "</Group>");
            const char *next_child = group_end;
            if (next_group && (!next_child || next_group < next_child)) next_child = next_group;
            if (next_entry && (!next_child || next_entry < next_child)) next_child = next_entry;
            if (!next_child) break;

            if (group_depth >= sizeof(group_stack) / sizeof(group_stack[0])) {
                ESP_LOGW(TAG, "KDBX group nesting exceeds limit: %u",
                         (unsigned)KDBX_MAX_GROUPS);
                protected_stream_free(&protected_stream);
                return ESP_ERR_INVALID_SIZE;
            }

            char group_name[64];
            parse_group_name(cursor, next_child, group_name, sizeof(group_name));
            size_t parent_index = group_depth ? group_stack[group_depth - 1].index : SIZE_MAX;
            group_stack[group_depth].index = kdbx_add_group(group_name,
                                                            (uint8_t)group_depth,
                                                            parent_index);
            group_depth++;
            cursor += 7;
        } else if (strncmp(cursor, "</Group>", 8) == 0) {
            if (group_depth > 0) group_depth--;
            cursor += 8;
        } else if (strncmp(cursor, "<Entry>", 7) == 0) {
            const char *entry_end = find_matching_entry_end(cursor);
            if (!entry_end) break;
            entry_index++;
            size_t group_index = group_depth ? group_stack[group_depth - 1].index : SIZE_MAX;
            const kdbx_group_t *group = kdbx_get_group(group_index);
            const char *group_name = group ? group->name : "";
            esp_err_t entry_ret = parse_entry_string_fields(cursor, entry_end, entry_index,
                                                            group_name, group_index,
                                                            &protected_stream, &sink);
            if (entry_ret != ESP_OK) {
                protected_stream_free(&protected_stream);
                return entry_ret;
            }
            cursor = entry_end + strlen("</Entry>");
        } else {
            cursor++;
        }
        if ((entry_index & 0x03) == 0) {
            kdbx_crypto_cooperate();
        }
    }

    size_t totp_count = 0;
    for (size_t i = 0; i < *entry_count; i++) {
        if (entries[i].otp_uri[0] || entries[i].totp_secret[0]) totp_count++;
    }
    ESP_LOGI(TAG, "KDBX parsed entries=%u stored=%u TOTP=%u",
             entry_index, (unsigned)*entry_count, (unsigned)totp_count);
    if (entry_index > *entry_count) {
        ESP_LOGW(TAG, "KDBX entry list truncated: parsed=%u capacity=%u",
                 entry_index, (unsigned)max_entries);
    }
    protected_stream_free(&protected_stream);
    KDBX_LOGD(TAG, "KDBX timing: parsed_xml_len=%u stored_entries=%u max_entries=%u",
             (unsigned int)xml_len,
             (unsigned int)*entry_count,
             (unsigned int)max_entries);
    kdbx_log_elapsed("parse KDBX XML entries", start_us);
    if (*entry_count == 0 && kdbx_get_group_count() == 0) {
        ESP_LOGW(TAG, "KDBX XML contains no recognized groups or entries (len=%u)",
                 (unsigned)xml_len);
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}

static void parse_payload_body(const kdbx_header_t *header,
                               uint8_t *plain,
                               size_t plain_len,
                               kdbx_entry_t *entries,
                               size_t *entry_count,
                               size_t max_entries,
                               esp_err_t *ret)
{
    int64_t start_us = kdbx_now_us();
    size_t xml_offset = 0;
    kdbx_inner_header_t inner;
    *ret = parse_inner_header(plain, plain_len, &xml_offset, &inner);
    kdbx_log_elapsed("parse KDBX inner header", start_us);
    if (*ret == ESP_OK && xml_offset <= plain_len) {
        size_t xml_len = plain_len - xml_offset;
        KDBX_LOGD(TAG, "KDBX payload data after inner header len=%u compression=%lu",
                 (unsigned int)xml_len,
                 (unsigned long)header->compression_flags);
        kdbx_log_hex_value("KDBX payload data", plain + xml_offset, xml_len);
        log_xml_summary(plain + xml_offset, xml_len);
        log_decrypted_xml(plain + xml_offset, xml_len);
        start_us = kdbx_now_us();
        *ret = parse_kdbx_entries(plain + xml_offset, xml_len, &inner,
                                  entries, entry_count, max_entries);
        kdbx_log_elapsed("parse payload XML body", start_us);
    }
}

esp_err_t kdbx_decrypt_payload(FILE *f,
                               const kdbx_header_t *header,
                               const uint8_t master_key[32],
                               const uint8_t hmac_base_key[64],
                               const uint8_t *header_bytes,
                               size_t header_len,
                               kdbx_entry_t *entries,
                               size_t *entry_count,
                               size_t max_entries)
{
    int64_t total_start_us = kdbx_now_us();
    int64_t step_start_us = kdbx_now_us();
    uint8_t header_hash[32];
    uint8_t header_hmac[32];
    if (fread(header_hash, 1, sizeof(header_hash), f) != sizeof(header_hash) ||
        fread(header_hmac, 1, sizeof(header_hmac), f) != sizeof(header_hmac)) {
        ESP_LOGW(TAG, "Unexpected EOF while reading KDBX4 header hash/HMAC");
        kdbx_log_elapsed("decrypt payload failed reading header hash/HMAC", total_start_us);
        return ESP_ERR_INVALID_SIZE;
    }
    kdbx_log_elapsed("read KDBX header hash/HMAC", step_start_us);

    step_start_us = kdbx_now_us();
    uint8_t calculated_header_hash[32];
    uint8_t calculated_header_hmac[32];
    esp_err_t ret = kdbx_sha256_bytes(header_bytes, header_len, calculated_header_hash);
    if (ret != ESP_OK) {
        kdbx_log_elapsed("decrypt payload failed calculating header hash", total_start_us);
        return ret;
    }
    ret = kdbx_header_hmac(hmac_base_key, header_bytes, header_len, calculated_header_hmac);
    if (ret != ESP_OK) {
        kdbx_log_elapsed("decrypt payload failed calculating header HMAC", total_start_us);
        return ret;
    }
    kdbx_log_elapsed("calculate KDBX header hash/HMAC", step_start_us);

    kdbx_log_hex_value("KDBX header SHA256 stored", header_hash, sizeof(header_hash));
    kdbx_log_hex_value("KDBX header SHA256 calculated", calculated_header_hash, sizeof(calculated_header_hash));
    if (memcmp(header_hash, calculated_header_hash, sizeof(header_hash)) != 0) {
        ESP_LOGW(TAG, "KDBX header SHA256 mismatch; database header is corrupted");
        kdbx_log_elapsed("decrypt payload failed header hash", total_start_us);
        return ESP_ERR_INVALID_CRC;
    }
    KDBX_LOGD(TAG, "KDBX header SHA256 match=yes");

    kdbx_log_hex_value("KDBX header HMAC stored", header_hmac, sizeof(header_hmac));
    kdbx_log_hex_value("KDBX header HMAC calculated", calculated_header_hmac, sizeof(calculated_header_hmac));
    if (memcmp(header_hmac, calculated_header_hmac, sizeof(header_hmac)) != 0) {
        ESP_LOGW(TAG, "KDBX authentication failed: password is incorrect or an additional key file is required");
        kdbx_log_elapsed("decrypt payload failed header HMAC", total_start_us);
        return ESP_ERR_INVALID_CRC;
    }
    KDBX_LOGD(TAG, "KDBX header HMAC match=yes");

    uint8_t *encrypted = NULL;
    size_t encrypted_len = 0;
    step_start_us = kdbx_now_us();
    ret = read_hmac_block_stream(f, &encrypted, &encrypted_len);
    kdbx_log_elapsed("collect encrypted payload blocks", step_start_us);
    if (ret != ESP_OK) {
        kdbx_log_elapsed("decrypt payload total failed reading blocks", total_start_us);
        return ret;
    }

    KDBX_LOGD(TAG, "KDBX encrypted payload len=%u", (unsigned int)encrypted_len);

    uint8_t *plain = NULL;
    size_t plain_len = 0;
    step_start_us = kdbx_now_us();
    ret = decrypt_payload_bytes(header, master_key, encrypted, encrypted_len, &plain, &plain_len);
    kdbx_log_elapsed("decrypt encrypted payload bytes", step_start_us);
    free(encrypted);
    if (ret != ESP_OK) {
        kdbx_log_elapsed("decrypt payload total failed outer cipher", total_start_us);
        return ret;
    }

    kdbx_log_hex_value("KDBX decrypted payload", plain, plain_len);

    if (plain_len >= 2 && plain[0] == 0x1f && plain[1] == 0x8b) {
        uint8_t *inflated = NULL;
        size_t inflated_len = 0;
        step_start_us = kdbx_now_us();
        ret = inflate_gzip_payload(plain, plain_len, &inflated, &inflated_len);
        kdbx_log_elapsed("inflate decrypted gzip payload", step_start_us);
        if (ret == ESP_OK) {
            kdbx_log_hex_value("KDBX inflated payload first bytes", inflated, inflated_len);
            step_start_us = kdbx_now_us();
            parse_payload_body(header, inflated, inflated_len, entries, entry_count, max_entries, &ret);
            kdbx_log_elapsed("parse inflated payload body", step_start_us);
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "Failed to parse KDBX inner header after inflate: %s", esp_err_to_name(ret));
            }
            free(inflated);
        } else {
            ESP_LOGW(TAG, "Failed to inflate KDBX gzip payload: %s", esp_err_to_name(ret));
        }
        free(plain);
        kdbx_log_elapsed(ret == ESP_OK ? "decrypt payload total" : "decrypt payload total failed gzip path", total_start_us);
        return ret;
    }

    step_start_us = kdbx_now_us();
    parse_payload_body(header, plain, plain_len, entries, entry_count, max_entries, &ret);
    kdbx_log_elapsed("parse plain payload body", step_start_us);
    free(plain);
    kdbx_log_elapsed(ret == ESP_OK ? "decrypt payload total" : "decrypt payload total failed plain path", total_start_us);
    return ret;
}

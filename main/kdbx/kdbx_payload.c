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

static esp_err_t reserve_bytes(uint8_t **buffer, size_t *capacity, size_t needed_len)
{
    if (needed_len <= *capacity) {
        return ESP_OK;
    }

    size_t new_capacity = *capacity == 0 ? 1024 : *capacity;
    while (new_capacity < needed_len) {
        new_capacity *= 2;
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
    ESP_LOGI(TAG, "KDBX timing: HMAC block stream data_blocks=%lu bytes=%u",
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
    if (!header->has_encryption_iv) {
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

    *out_plain = plain;
    *out_plain_len = plain_len;
    ESP_LOGI(TAG, "KDBX timing: AES-CBC encrypted=%u plain=%u padding=%u",
             (unsigned int)encrypted_len,
             (unsigned int)plain_len,
             pad);
    kdbx_log_elapsed("AES-CBC payload decrypt", start_us);
    return ESP_OK;
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
    ESP_LOGI(TAG, "KDBX protected stream initialized: ChaCha20");
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

        ESP_LOGI(TAG, "KDBX inner header field id=%u size=%lu",
                 field_id,
                 (unsigned long)field_size);

        if (field_id == 0) {
            *out_xml_offset = offset + field_size;
            return ESP_OK;
        }

        if (field_id == 1 && field_size == 4) {
            inner->random_stream_id = kdbx_read_le32(plain + offset);
            ESP_LOGI(TAG, "InnerRandomStreamID=%lu", (unsigned long)inner->random_stream_id);
        } else if (field_id == 2) {
            size_t copy_len = field_size < sizeof(inner->random_stream_key) ? field_size : sizeof(inner->random_stream_key);
            memcpy(inner->random_stream_key, plain + offset, copy_len);
            inner->random_stream_key_len = copy_len;
            inner->has_random_stream_key = true;
            kdbx_log_hex_value("InnerRandomStreamKey", plain + offset, field_size);
        } else if (field_id == 3) {
            ESP_LOGI(TAG, "InnerBinary len=%lu", (unsigned long)field_size);
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
    if (expected_len > 0 && expected_len <= 262144) {
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
            ESP_LOGI(TAG, "KDBX timing: gzip compressed=%u deflate=%u inflated=%u expected=%u",
                     (unsigned int)gzip_len,
                     (unsigned int)deflate_len,
                     (unsigned int)written,
                     (unsigned int)expected_len);
            kdbx_log_elapsed("inflate gzip payload", start_us);
            return ESP_OK;
        }

        free(xml);
    }

    for (size_t capacity = 8192; capacity <= 262144; capacity *= 2) {
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
            ESP_LOGI(TAG, "KDBX timing: gzip compressed=%u deflate=%u inflated=%u capacity=%u",
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

    *out_protected = strstr(value_open, "Protected=\"True\"") != NULL ||
                     strstr(value_open, "Protected=\"true\"") != NULL;

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

static void parse_entry_string_fields(const char *entry_start,
                                      const char *entry_end,
                                      unsigned int entry_index,
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
    bool password_protected = false;

    const char *cursor = entry_start;
    while ((cursor = strstr(cursor, "<String>")) != NULL && cursor < entry_end) {
        const char *string_end = strstr(cursor, "</String>");
        if (!string_end || string_end > entry_end) {
            break;
        }

        char key[32];
        char value[128];
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

            if (strcmp(key, "Title") == 0) {
                strlcpy(title, value, sizeof(title));
            } else if (strcmp(key, "UserName") == 0) {
                strlcpy(username, value, sizeof(username));
            } else if (strcmp(key, "URL") == 0) {
                strlcpy(url, value, sizeof(url));
            } else if (strcmp(key, "Password") == 0) {
                strlcpy(password, value, sizeof(password));
                password_protected = value_protected && !protected_decoded;
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

    if (*sink->entry_count < sink->max_entries) {
        kdbx_entry_t *entry = &sink->entries[(*sink->entry_count)++];
        const char *entry_title = title[0] ? title : (username[0] ? username : "(no title)");
        strlcpy(entry->title, entry_title, sizeof(entry->title));
        strlcpy(entry->username, username, sizeof(entry->username));
        strlcpy(entry->url, url, sizeof(entry->url));
        strlcpy(entry->password, password, sizeof(entry->password));
        entry->password_protected = password_protected;
    }
}

static void parse_kdbx_entries(const uint8_t *xml,
                               size_t xml_len,
                               const kdbx_inner_header_t *inner,
                               kdbx_entry_t *entries,
                               size_t *entry_count,
                               size_t max_entries)
{
    int64_t start_us = kdbx_now_us();
    char *text = malloc(xml_len + 1);
    if (!text) {
        ESP_LOGW(TAG, "No memory to parse KDBX entries");
        kdbx_log_elapsed("parse KDBX XML entries failed no memory", start_us);
        return;
    }

    memcpy(text, xml, xml_len);
    text[xml_len] = '\0';

    *entry_count = 0;
    memset(entries, 0, sizeof(entries[0]) * max_entries);

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
    const char *cursor = text;
    while ((cursor = strstr(cursor, "<Entry>")) != NULL) {
        const char *entry_end = strstr(cursor, "</Entry>");
        if (!entry_end) {
            break;
        }

        entry_index++;
        parse_entry_string_fields(cursor, entry_end, entry_index, &protected_stream, &sink);
        cursor = entry_end + strlen("</Entry>");
        if (*entry_count >= max_entries) {
            break;
        }
        if ((entry_index & 0x03) == 0) {
            kdbx_crypto_cooperate();
        }
    }

    ESP_LOGI(TAG, "KDBX parsed entries=%u", entry_index);
    protected_stream_free(&protected_stream);
    free(text);
    ESP_LOGI(TAG, "KDBX timing: parsed_xml_len=%u stored_entries=%u max_entries=%u",
             (unsigned int)xml_len,
             (unsigned int)*entry_count,
             (unsigned int)max_entries);
    kdbx_log_elapsed("parse KDBX XML entries", start_us);
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
        ESP_LOGI(TAG, "KDBX payload data after inner header len=%u compression=%lu",
                 (unsigned int)xml_len,
                 (unsigned long)header->compression_flags);
        kdbx_log_hex_value("KDBX payload data", plain + xml_offset, xml_len);
        log_xml_summary(plain + xml_offset, xml_len);
        start_us = kdbx_now_us();
        parse_kdbx_entries(plain + xml_offset, xml_len, &inner, entries, entry_count, max_entries);
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
    ESP_LOGI(TAG, "KDBX header SHA256 match=%s",
             memcmp(header_hash, calculated_header_hash, sizeof(header_hash)) == 0 ? "yes" : "no");

    kdbx_log_hex_value("KDBX header HMAC stored", header_hmac, sizeof(header_hmac));
    kdbx_log_hex_value("KDBX header HMAC calculated", calculated_header_hmac, sizeof(calculated_header_hmac));
    ESP_LOGI(TAG, "KDBX header HMAC match=%s",
             memcmp(header_hmac, calculated_header_hmac, sizeof(header_hmac)) == 0 ? "yes" : "no");

    uint8_t *encrypted = NULL;
    size_t encrypted_len = 0;
    step_start_us = kdbx_now_us();
    ret = read_hmac_block_stream(f, &encrypted, &encrypted_len);
    kdbx_log_elapsed("collect encrypted payload blocks", step_start_us);
    if (ret != ESP_OK) {
        kdbx_log_elapsed("decrypt payload total failed reading blocks", total_start_us);
        return ret;
    }

    ESP_LOGI(TAG, "KDBX encrypted payload len=%u", (unsigned int)encrypted_len);

    uint8_t *plain = NULL;
    size_t plain_len = 0;
    step_start_us = kdbx_now_us();
    ret = decrypt_aes_cbc_payload(header, master_key, encrypted, encrypted_len, &plain, &plain_len);
    kdbx_log_elapsed("decrypt encrypted payload bytes", step_start_us);
    free(encrypted);
    if (ret != ESP_OK) {
        kdbx_log_elapsed("decrypt payload total failed AES-CBC", total_start_us);
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

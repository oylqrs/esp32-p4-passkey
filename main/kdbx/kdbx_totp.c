#include "kdbx_totp.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mbedtls/base64.h"
#include "mbedtls/md.h"

#define TOTP_DEFAULT_PERIOD 30U
#define TOTP_DEFAULT_DIGITS 6U
#define TOTP_MAX_SECRET_BYTES 128U

typedef struct {
    char secret[160];
    char algorithm[16];
    uint32_t period;
    uint8_t digits;
    kdbx_totp_secret_encoding_t secret_encoding;
} totp_parameters_t;

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void uri_decode(char *text)
{
    char *read = text;
    char *write = text;
    while (*read) {
        if (*read == '%' && hex_value(read[1]) >= 0 && hex_value(read[2]) >= 0) {
            *write++ = (char)((hex_value(read[1]) << 4) | hex_value(read[2]));
            read += 3;
        } else {
            *write++ = *read == '+' ? ' ' : *read;
            read++;
        }
    }
    *write = '\0';
}

static bool copy_query_value(const char *uri, const char *name, char *out, size_t out_size)
{
    const char *query = strchr(uri, '?');
    if (!query || out_size == 0) return false;
    query++;

    size_t name_len = strlen(name);
    while (*query) {
        const char *end = strchr(query, '&');
        if (!end) end = query + strlen(query);
        const char *equals = memchr(query, '=', (size_t)(end - query));
        if (equals && (size_t)(equals - query) == name_len &&
            strncasecmp(query, name, name_len) == 0) {
            size_t len = (size_t)(end - equals - 1);
            if (len >= out_size) len = out_size - 1;
            memcpy(out, equals + 1, len);
            out[len] = '\0';
            uri_decode(out);
            return true;
        }
        if (!*end) break;
        query = end + 1;
    }
    return false;
}

static bool parse_uint(const char *text, uint32_t minimum, uint32_t maximum, uint32_t *value)
{
    if (!text || !text[0]) return false;
    char *end = NULL;
    unsigned long parsed = strtoul(text, &end, 10);
    if (*end != '\0' || parsed < minimum || parsed > maximum) return false;
    *value = (uint32_t)parsed;
    return true;
}

static bool get_parameters(const kdbx_entry_t *entry, totp_parameters_t *parameters)
{
    memset(parameters, 0, sizeof(*parameters));
    parameters->period = entry->totp_period ? entry->totp_period : TOTP_DEFAULT_PERIOD;
    parameters->digits = entry->totp_digits ? entry->totp_digits : TOTP_DEFAULT_DIGITS;
    parameters->secret_encoding = entry->totp_secret_encoding;
    strlcpy(parameters->algorithm,
            entry->totp_algorithm[0] ? entry->totp_algorithm : "SHA1",
            sizeof(parameters->algorithm));

    if (entry->otp_uri[0]) {
        if (strncasecmp(entry->otp_uri, "otpauth://totp/", 15) != 0 ||
            !copy_query_value(entry->otp_uri, "secret", parameters->secret,
                              sizeof(parameters->secret))) {
            return false;
        }
        parameters->secret_encoding = KDBX_TOTP_SECRET_BASE32;

        char value[24];
        uint32_t parsed;
        if (copy_query_value(entry->otp_uri, "period", value, sizeof(value)) &&
            parse_uint(value, 1, 3600, &parsed)) {
            parameters->period = parsed;
        }
        if (copy_query_value(entry->otp_uri, "digits", value, sizeof(value)) &&
            parse_uint(value, 6, 8, &parsed)) {
            parameters->digits = (uint8_t)parsed;
        }
        if (copy_query_value(entry->otp_uri, "algorithm", value, sizeof(value))) {
            strlcpy(parameters->algorithm, value, sizeof(parameters->algorithm));
        }
    } else {
        strlcpy(parameters->secret, entry->totp_secret, sizeof(parameters->secret));
    }
    return parameters->secret[0] != '\0';
}

static esp_err_t base32_decode(const char *encoded, uint8_t *decoded, size_t *decoded_len)
{
    uint32_t accumulator = 0;
    unsigned bits = 0;
    size_t output_len = 0;

    for (const char *p = encoded; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '=' || isspace(c) || c == '-') continue;
        c = (unsigned char)toupper(c);
        int value = c >= 'A' && c <= 'Z' ? c - 'A' :
                    c >= '2' && c <= '7' ? c - '2' + 26 : -1;
        if (value < 0) return ESP_ERR_INVALID_ARG;

        accumulator = (accumulator << 5) | (uint32_t)value;
        bits += 5;
        if (bits >= 8) {
            bits -= 8;
            if (output_len >= *decoded_len) return ESP_ERR_INVALID_SIZE;
            decoded[output_len++] = (uint8_t)(accumulator >> bits);
            accumulator &= bits ? (1U << bits) - 1U : 0U;
        }
    }
    if (output_len == 0) return ESP_ERR_INVALID_ARG;
    *decoded_len = output_len;
    return ESP_OK;
}

static esp_err_t base64_decode(const char *encoded, uint8_t *decoded, size_t *decoded_len)
{
    size_t output_len = 0;
    int ret = mbedtls_base64_decode(decoded, *decoded_len, &output_len,
                                    (const uint8_t *)encoded, strlen(encoded));
    if (ret != 0 || output_len == 0) return ESP_ERR_INVALID_ARG;
    *decoded_len = output_len;
    return ESP_OK;
}

static esp_err_t hex_decode(const char *encoded, uint8_t *decoded, size_t *decoded_len)
{
    size_t output_len = 0;
    int high_nibble = -1;
    for (const char *p = encoded; *p; p++) {
        if (isspace((unsigned char)*p) || *p == '-' || *p == ':') continue;
        int value = hex_value(*p);
        if (value < 0) return ESP_ERR_INVALID_ARG;
        if (high_nibble < 0) {
            high_nibble = value;
        } else {
            if (output_len >= *decoded_len) return ESP_ERR_INVALID_SIZE;
            decoded[output_len++] = (uint8_t)((high_nibble << 4) | value);
            high_nibble = -1;
        }
    }
    if (high_nibble >= 0 || output_len == 0) return ESP_ERR_INVALID_ARG;
    *decoded_len = output_len;
    return ESP_OK;
}

static esp_err_t decode_secret(const totp_parameters_t *parameters,
                               uint8_t *decoded,
                               size_t *decoded_len)
{
    switch (parameters->secret_encoding) {
    case KDBX_TOTP_SECRET_BASE64:
        return base64_decode(parameters->secret, decoded, decoded_len);
    case KDBX_TOTP_SECRET_HEX:
        return hex_decode(parameters->secret, decoded, decoded_len);
    case KDBX_TOTP_SECRET_BASE32:
    default:
        return base32_decode(parameters->secret, decoded, decoded_len);
    }
}

static mbedtls_md_type_t algorithm_type(const char *algorithm)
{
    if (strcasecmp(algorithm, "SHA256") == 0 || strcasecmp(algorithm, "HMAC-SHA-256") == 0) {
        return MBEDTLS_MD_SHA256;
    }
    if (strcasecmp(algorithm, "SHA512") == 0 || strcasecmp(algorithm, "HMAC-SHA-512") == 0) {
        return MBEDTLS_MD_SHA512;
    }
    if (strcasecmp(algorithm, "SHA1") == 0 || strcasecmp(algorithm, "HMAC-SHA-1") == 0) {
        return MBEDTLS_MD_SHA1;
    }
    return MBEDTLS_MD_NONE;
}

bool kdbx_totp_is_configured(const kdbx_entry_t *entry)
{
    return entry && (entry->otp_uri[0] || entry->totp_secret[0]);
}

esp_err_t kdbx_totp_generate_at(const kdbx_entry_t *entry,
                                uint64_t unix_time,
                                char *code,
                                size_t code_size,
                                uint32_t *seconds_remaining)
{
    if (!entry || !code) return ESP_ERR_INVALID_ARG;

    totp_parameters_t parameters;
    if (!get_parameters(entry, &parameters) || parameters.digits > 8 ||
        code_size <= parameters.digits) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t secret[TOTP_MAX_SECRET_BYTES];
    size_t secret_len = sizeof(secret);
    esp_err_t ret = decode_secret(&parameters, secret, &secret_len);
    if (ret != ESP_OK) return ret;

    uint64_t counter = unix_time / parameters.period;
    uint8_t message[8];
    for (int i = 7; i >= 0; i--) {
        message[i] = (uint8_t)counter;
        counter >>= 8;
    }

    mbedtls_md_type_t type = algorithm_type(parameters.algorithm);
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(type);
    if (!info) {
        memset(secret, 0, sizeof(secret));
        return ESP_ERR_NOT_SUPPORTED;
    }

    uint8_t digest[MBEDTLS_MD_MAX_SIZE];
    int md_ret = mbedtls_md_hmac(info, secret, secret_len, message, sizeof(message), digest);
    memset(secret, 0, sizeof(secret));
    if (md_ret != 0) return ESP_FAIL;

    size_t digest_len = mbedtls_md_get_size(info);
    unsigned offset = digest[digest_len - 1] & 0x0f;
    if (offset + 4 > digest_len) return ESP_FAIL;
    uint32_t binary = ((uint32_t)(digest[offset] & 0x7f) << 24) |
                      ((uint32_t)digest[offset + 1] << 16) |
                      ((uint32_t)digest[offset + 2] << 8) |
                      digest[offset + 3];
    static const uint32_t powers_of_ten[] = {
        1U, 10U, 100U, 1000U, 10000U, 100000U, 1000000U, 10000000U, 100000000U,
    };
    snprintf(code, code_size, "%0*u", parameters.digits,
             (unsigned)(binary % powers_of_ten[parameters.digits]));
    if (seconds_remaining) {
        *seconds_remaining = parameters.period - (uint32_t)(unix_time % parameters.period);
    }
    return ESP_OK;
}

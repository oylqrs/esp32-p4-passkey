#include "browser_autofill/keepass_http_crypto.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "esp_random.h"
#include "mbedtls/aes.h"
#include "mbedtls/base64.h"
#include "mbedtls/platform_util.h"

#define AES_BLOCK_SIZE 16

static esp_err_t decode_nonce(const char *encoded,
                              uint8_t nonce[KEEPASS_HTTP_NONCE_SIZE])
{
    if (!encoded) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t decoded = 0;
    int ret = mbedtls_base64_decode(nonce, KEEPASS_HTTP_NONCE_SIZE, &decoded,
                                    (const unsigned char *)encoded,
                                    strlen(encoded));
    return ret == 0 && decoded == KEEPASS_HTTP_NONCE_SIZE
               ? ESP_OK
               : ESP_ERR_INVALID_ARG;
}

esp_err_t keepass_http_crypto_decode_key(
    const char *encoded, uint8_t key[KEEPASS_HTTP_KEY_SIZE])
{
    if (!encoded || !key) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t decoded = 0;
    int ret = mbedtls_base64_decode(key, KEEPASS_HTTP_KEY_SIZE, &decoded,
                                    (const unsigned char *)encoded,
                                    strlen(encoded));
    return ret == 0 && decoded == KEEPASS_HTTP_KEY_SIZE
               ? ESP_OK
               : ESP_ERR_INVALID_ARG;
}

esp_err_t keepass_http_crypto_random_base64(size_t byte_count,
                                             char *encoded,
                                             size_t encoded_size)
{
    if (byte_count == 0 || !encoded) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t *random = malloc(byte_count);
    if (!random) {
        return ESP_ERR_NO_MEM;
    }
    esp_fill_random(random, byte_count);

    size_t written = 0;
    int ret = mbedtls_base64_encode((unsigned char *)encoded, encoded_size,
                                    &written, random, byte_count);
    mbedtls_platform_zeroize(random, byte_count);
    free(random);
    if (ret != 0 || written >= encoded_size) {
        return ESP_ERR_INVALID_SIZE;
    }
    encoded[written] = '\0';
    return ESP_OK;
}

esp_err_t keepass_http_crypto_encrypt(
    const char *plain,
    const uint8_t key[KEEPASS_HTTP_KEY_SIZE],
    const char *nonce,
    char *encoded,
    size_t encoded_size)
{
    if (!plain || !key || !nonce || !encoded) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t iv[KEEPASS_HTTP_NONCE_SIZE];
    esp_err_t ret = decode_nonce(nonce, iv);
    if (ret != ESP_OK) {
        return ret;
    }

    size_t plain_len = strlen(plain);
    size_t padding = AES_BLOCK_SIZE - (plain_len % AES_BLOCK_SIZE);
    size_t encrypted_len = plain_len + padding;
    size_t required = 4 * ((encrypted_len + 2) / 3) + 1;
    if (required > encoded_size) {
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t *buffer = malloc(encrypted_len);
    if (!buffer) {
        return ESP_ERR_NO_MEM;
    }
    memcpy(buffer, plain, plain_len);
    memset(buffer + plain_len, (int)padding, padding);

    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    int crypto_ret = mbedtls_aes_setkey_enc(&aes, key,
                                             KEEPASS_HTTP_KEY_SIZE * 8);
    if (crypto_ret == 0) {
        crypto_ret = mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_ENCRYPT,
                                           encrypted_len, iv, buffer, buffer);
    }
    mbedtls_aes_free(&aes);
    if (crypto_ret != 0) {
        mbedtls_platform_zeroize(buffer, encrypted_len);
        free(buffer);
        return ESP_FAIL;
    }

    size_t written = 0;
    crypto_ret = mbedtls_base64_encode((unsigned char *)encoded, encoded_size,
                                       &written, buffer, encrypted_len);
    mbedtls_platform_zeroize(buffer, encrypted_len);
    free(buffer);
    if (crypto_ret != 0 || written >= encoded_size) {
        return ESP_ERR_INVALID_SIZE;
    }
    encoded[written] = '\0';
    return ESP_OK;
}

esp_err_t keepass_http_crypto_decrypt(
    const char *encoded,
    const uint8_t key[KEEPASS_HTTP_KEY_SIZE],
    const char *nonce,
    char *plain,
    size_t plain_size)
{
    if (!encoded || !key || !nonce || !plain || plain_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    plain[0] = '\0';

    uint8_t iv[KEEPASS_HTTP_NONCE_SIZE];
    esp_err_t ret = decode_nonce(nonce, iv);
    if (ret != ESP_OK) {
        return ret;
    }

    size_t encoded_len = strlen(encoded);
    size_t capacity = (encoded_len / 4) * 3 + 3;
    uint8_t *buffer = malloc(capacity);
    if (!buffer) {
        return ESP_ERR_NO_MEM;
    }

    size_t encrypted_len = 0;
    int crypto_ret = mbedtls_base64_decode(buffer, capacity, &encrypted_len,
                                           (const unsigned char *)encoded,
                                           encoded_len);
    if (crypto_ret != 0 || encrypted_len == 0 ||
        (encrypted_len % AES_BLOCK_SIZE) != 0) {
        mbedtls_platform_zeroize(buffer, capacity);
        free(buffer);
        return ESP_ERR_INVALID_ARG;
    }

    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    crypto_ret = mbedtls_aes_setkey_dec(&aes, key,
                                         KEEPASS_HTTP_KEY_SIZE * 8);
    if (crypto_ret == 0) {
        crypto_ret = mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_DECRYPT,
                                           encrypted_len, iv, buffer, buffer);
    }
    mbedtls_aes_free(&aes);
    if (crypto_ret != 0) {
        mbedtls_platform_zeroize(buffer, capacity);
        free(buffer);
        return ESP_FAIL;
    }

    uint8_t padding = buffer[encrypted_len - 1];
    if (padding == 0 || padding > AES_BLOCK_SIZE || padding > encrypted_len) {
        mbedtls_platform_zeroize(buffer, capacity);
        free(buffer);
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t padding_error = 0;
    for (size_t i = 0; i < padding; i++) {
        padding_error |= buffer[encrypted_len - 1 - i] ^ padding;
    }
    size_t plain_len = encrypted_len - padding;
    if (padding_error != 0 || plain_len >= plain_size) {
        mbedtls_platform_zeroize(buffer, capacity);
        free(buffer);
        return ESP_ERR_INVALID_SIZE;
    }

    memcpy(plain, buffer, plain_len);
    plain[plain_len] = '\0';
    mbedtls_platform_zeroize(buffer, capacity);
    free(buffer);
    return ESP_OK;
}

esp_err_t keepass_http_crypto_make_verifier(
    const uint8_t key[KEEPASS_HTTP_KEY_SIZE],
    const char *nonce,
    char *verifier,
    size_t verifier_size)
{
    return keepass_http_crypto_encrypt(nonce, key, nonce,
                                       verifier, verifier_size);
}

bool keepass_http_crypto_verify(
    const uint8_t key[KEEPASS_HTTP_KEY_SIZE],
    const char *nonce,
    const char *verifier)
{
    if (!key || !nonce || !verifier) {
        return false;
    }

    char decoded[KEEPASS_HTTP_NONCE_BASE64_SIZE] = {0};
    if (keepass_http_crypto_decrypt(verifier, key, nonce,
                                     decoded, sizeof(decoded)) != ESP_OK) {
        return false;
    }

    size_t nonce_len = strlen(nonce);
    size_t decoded_len = strlen(decoded);
    unsigned int difference = (unsigned int)(nonce_len ^ decoded_len);
    size_t compare_len = nonce_len < decoded_len ? nonce_len : decoded_len;
    for (size_t i = 0; i < compare_len; i++) {
        difference |= (unsigned char)nonce[i] ^ (unsigned char)decoded[i];
    }
    mbedtls_platform_zeroize(decoded, sizeof(decoded));
    return difference == 0;
}

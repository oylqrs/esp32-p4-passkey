#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define KEEPASS_HTTP_KEY_SIZE 32
#define KEEPASS_HTTP_NONCE_SIZE 16
#define KEEPASS_HTTP_NONCE_BASE64_SIZE 25

esp_err_t keepass_http_crypto_decode_key(
    const char *encoded, uint8_t key[KEEPASS_HTTP_KEY_SIZE]);
esp_err_t keepass_http_crypto_random_base64(
    size_t byte_count, char *encoded, size_t encoded_size);
esp_err_t keepass_http_crypto_encrypt(
    const char *plain,
    const uint8_t key[KEEPASS_HTTP_KEY_SIZE],
    const char *nonce,
    char *encoded,
    size_t encoded_size);
esp_err_t keepass_http_crypto_decrypt(
    const char *encoded,
    const uint8_t key[KEEPASS_HTTP_KEY_SIZE],
    const char *nonce,
    char *plain,
    size_t plain_size);
esp_err_t keepass_http_crypto_make_verifier(
    const uint8_t key[KEEPASS_HTTP_KEY_SIZE],
    const char *nonce,
    char *verifier,
    size_t verifier_size);
bool keepass_http_crypto_verify(
    const uint8_t key[KEEPASS_HTTP_KEY_SIZE],
    const char *nonce,
    const char *verifier);

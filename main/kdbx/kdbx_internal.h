#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "esp_err.h"
#include "kdbx_loader.h"
#include "mbedtls/md.h"

#define KDBX_MAX_ENTRIES 24
#define KDBX_VERBOSE_LOG 0

typedef struct {
    uint8_t cipher_id[16];
    bool has_cipher_id;
    uint32_t compression_flags;
    bool has_compression_flags;
    uint8_t master_seed[32];
    bool has_master_seed;
    uint8_t encryption_iv[16];
    bool has_encryption_iv;
    uint8_t kdf_uuid[16];
    bool has_kdf_uuid;
    uint8_t kdf_seed[32];
    bool has_kdf_seed;
    uint64_t kdf_rounds;
    bool has_kdf_rounds;
} kdbx_header_t;

typedef struct {
    uint32_t random_stream_id;
    uint8_t random_stream_key[64];
    size_t random_stream_key_len;
    bool has_random_stream_key;
} kdbx_inner_header_t;

uint32_t kdbx_read_le32(const uint8_t *buf);
uint64_t kdbx_read_le64(const uint8_t *buf);
void kdbx_log_hex_value(const char *label, const uint8_t *data, size_t len);
int64_t kdbx_now_us(void);
void kdbx_log_elapsed(const char *label, int64_t start_us);
void kdbx_crypto_cooperate(void);

esp_err_t kdbx_sha256_bytes(const uint8_t *data, size_t len, uint8_t out[32]);
esp_err_t kdbx_sha512_bytes(const uint8_t *data, size_t len, uint8_t out[64]);
esp_err_t kdbx_hmac_sha256(const uint8_t *key, size_t key_len,
                           const uint8_t *data, size_t data_len,
                           uint8_t out[32]);
esp_err_t kdbx_header_hmac(const uint8_t hmac_base_key[64],
                           const uint8_t *header_bytes,
                           size_t header_len,
                           uint8_t out_hmac[32]);

void kdbx_parse_kdf_parameters(const uint8_t *data, size_t len, kdbx_header_t *header);
esp_err_t kdbx_derive_aes_kdf_master_key(const kdbx_header_t *header,
                                         const char *password,
                                         uint8_t master_key[32],
                                         uint8_t hmac_base_key[64]);

esp_err_t kdbx_decrypt_payload(FILE *f,
                               const kdbx_header_t *header,
                               const uint8_t master_key[32],
                               const uint8_t hmac_base_key[64],
                               const uint8_t *header_bytes,
                               size_t header_len,
                               kdbx_entry_t *entries,
                               size_t *entry_count,
                               size_t max_entries);

esp_err_t kdbx_parse_file(const char *path,
                          const char *password,
                          kdbx_entry_t *entries,
                          size_t *entry_count,
                          size_t max_entries);

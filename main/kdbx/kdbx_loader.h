#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define KDBX_ERR_DATABASE_NOT_FOUND ((esp_err_t)0x7101)

typedef enum {
    KDBX_TOTP_SECRET_BASE32 = 0,
    KDBX_TOTP_SECRET_BASE64,
    KDBX_TOTP_SECRET_HEX,
} kdbx_totp_secret_encoding_t;

typedef struct {
    char title[96];
    char username[96];
    char url[128];
    char password[128];
    char group[64];
    char otp_uri[256];
    char totp_secret[160];
    char totp_algorithm[16];
    uint16_t totp_period;
    uint8_t totp_digits;
    kdbx_totp_secret_encoding_t totp_secret_encoding;
    uint32_t icon_id;
    size_t group_index;
    bool password_protected;
} kdbx_entry_t;

typedef struct {
    char name[64];
    uint8_t depth;
    size_t parent_index;
    size_t entry_count;
} kdbx_group_t;

esp_err_t kdbx_load_first_database_from_data(void);
esp_err_t kdbx_load_emergency_database_from_data(void);
esp_err_t kdbx_load_duress_database_from_data(void);
size_t kdbx_get_entry_count(void);
const kdbx_entry_t *kdbx_get_entry(size_t index);
esp_err_t kdbx_create_entry_snapshot(kdbx_entry_t **entries, size_t *count);
void kdbx_free_entry_snapshot(kdbx_entry_t *entries, size_t count);
size_t kdbx_get_group_count(void);
const kdbx_group_t *kdbx_get_group(size_t index);
bool kdbx_get_file_version(uint16_t *major, uint16_t *minor);

#ifdef __cplusplus
}
#endif

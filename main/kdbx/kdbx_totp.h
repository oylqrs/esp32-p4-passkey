#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "kdbx_loader.h"

#ifdef __cplusplus
extern "C" {
#endif

bool kdbx_totp_is_configured(const kdbx_entry_t *entry);
esp_err_t kdbx_totp_generate_at(const kdbx_entry_t *entry,
                                uint64_t unix_time,
                                char *code,
                                size_t code_size,
                                uint32_t *seconds_remaining);

#ifdef __cplusplus
}
#endif

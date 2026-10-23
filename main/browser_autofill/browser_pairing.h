#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "browser_autofill/keepass_http_crypto.h"
#include "esp_err.h"

#define BROWSER_PAIRING_ID_MAX 32

bool browser_pairing_exists(void);
bool browser_pairing_trust_enabled(void);
esp_err_t browser_pairing_store(const char *encoded_key,
                                char *id,
                                size_t id_size);
esp_err_t browser_pairing_get_key(
    const char *id, uint8_t key[KEEPASS_HTTP_KEY_SIZE]);
esp_err_t browser_pairing_clear(void);


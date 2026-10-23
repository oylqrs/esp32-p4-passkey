#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define WALLET_STORAGE_MNEMONIC_MAX_LEN 256
#define WALLET_STORAGE_PIN_MIN_LEN      6
#define WALLET_STORAGE_PIN_MAX_LEN      12
#define WALLET_STORAGE_NETWORK_COUNT    2

typedef struct {
    uint32_t account;
    uint32_t receive_index[WALLET_STORAGE_NETWORK_COUNT];
    uint32_t change_index[WALLET_STORAGE_NETWORK_COUNT];
} wallet_address_state_t;

esp_err_t wallet_storage_init(void);
bool wallet_storage_exists(void);
esp_err_t wallet_storage_store(const char *mnemonic, const char *pin);
esp_err_t wallet_storage_load(const char *pin, char *mnemonic, size_t mnemonic_size);
esp_err_t wallet_storage_load_address_state(wallet_address_state_t *state);
esp_err_t wallet_storage_save_address_state(const wallet_address_state_t *state);
esp_err_t wallet_storage_reset_address_state(void);
esp_err_t wallet_storage_erase(void);
bool wallet_storage_pin_is_valid(const char *pin);

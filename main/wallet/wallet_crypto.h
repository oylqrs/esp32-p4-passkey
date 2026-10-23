#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum {
    WALLET_NETWORK_BITCOIN = 0,
    WALLET_NETWORK_TESTNET,
} wallet_network_t;

#define WALLET_XPUB_MAX_LEN 128
#define WALLET_PSBT_BASE64_MAX_LEN 8192
#define WALLET_PSBT_MAX_EXTERNAL_OUTPUTS 8
#define WALLET_PSBT_ADDRESS_MAX_LEN 96

typedef struct {
    uint64_t amount;
    char address[WALLET_PSBT_ADDRESS_MAX_LEN];
} wallet_psbt_output_summary_t;

typedef struct {
    size_t input_count;
    size_t output_count;
    size_t signing_input_count;
    size_t external_output_count;
    uint64_t input_amount;
    uint64_t external_amount;
    uint64_t change_amount;
    uint64_t fee_amount;
    wallet_psbt_output_summary_t external_outputs[WALLET_PSBT_MAX_EXTERNAL_OUTPUTS];
} wallet_psbt_summary_t;

esp_err_t wallet_crypto_init(void);
esp_err_t wallet_crypto_self_test(void);
esp_err_t wallet_crypto_validate_mnemonic(const char *mnemonic);
esp_err_t wallet_crypto_generate_mnemonic(size_t word_count,
                                          char *mnemonic,
                                          size_t mnemonic_size);
esp_err_t wallet_crypto_derive_bip84_address(const char *mnemonic,
                                             const char *passphrase,
                                             wallet_network_t network,
                                             uint32_t account,
                                             uint32_t change,
                                             uint32_t index,
                                             char *address,
                                             size_t address_size);
esp_err_t wallet_crypto_get_bip84_xpub(const char *mnemonic,
                                       wallet_network_t network,
                                       uint32_t account,
                                       char *xpub,
                                       size_t xpub_size);
esp_err_t wallet_crypto_inspect_psbt_base64(const char *mnemonic,
                                            wallet_network_t network,
                                            uint32_t account,
                                            const char *base64,
                                            wallet_psbt_summary_t *summary);
esp_err_t wallet_crypto_sign_psbt_base64(const char *mnemonic,
                                         wallet_network_t network,
                                         uint32_t account,
                                         const char *base64,
                                         wallet_psbt_summary_t *summary,
                                         char *signed_base64,
                                         size_t signed_base64_size);
esp_err_t wallet_crypto_validate_psbt(const uint8_t *psbt, size_t psbt_size);

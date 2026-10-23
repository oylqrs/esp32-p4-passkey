#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "wallet_crypto.h"
#include "wallet_storage.h"

#define WALLET_ADDRESS_INDEX_MAX UINT32_C(0x7fffffff)

typedef enum {
    WALLET_STATUS_UNAVAILABLE = 0,
    WALLET_STATUS_UNINITIALIZED,
    WALLET_STATUS_LOCKED,
    WALLET_STATUS_READY,
} wallet_status_t;

typedef enum {
    WALLET_ACTION_INITIALIZE = 0,
    WALLET_ACTION_CONNECT,
    WALLET_ACTION_SESSION,
    WALLET_ACTION_SCAN_QR,
    WALLET_ACTION_QR_MODE,
    WALLET_ACTION_SETTINGS,
    WALLET_ACTION_EXPORT_XPUB,
    WALLET_ACTION_SIGN_PSBT,
    WALLET_ACTION_USB_JADE,
    WALLET_ACTION_COUNT,
} wallet_action_t;

typedef struct {
    bool bip39;
    bool bip32;
    bool encrypted_storage;
    bool receive_address;
    bool qr_scan;
    bool psbt;
} wallet_capabilities_t;

esp_err_t wallet_service_init(void);
esp_err_t wallet_service_enter(void);
void wallet_service_leave(void);
wallet_status_t wallet_service_get_status(void);
wallet_capabilities_t wallet_service_get_capabilities(void);
esp_err_t wallet_service_generate_mnemonic(size_t word_count, char *mnemonic,
                                           size_t mnemonic_size);
esp_err_t wallet_service_create(const char *mnemonic, const char *pin);
esp_err_t wallet_service_unlock(const char *pin);
void wallet_service_lock(void);
esp_err_t wallet_service_change_pin(const char *new_pin);
esp_err_t wallet_service_delete(void);
esp_err_t wallet_service_get_receive_address(wallet_network_t network,
                                             uint32_t index,
                                             char *address,
                                             size_t address_size);
esp_err_t wallet_service_get_address_state(wallet_network_t network,
                                           uint32_t *account,
                                           uint32_t *receive_index,
                                           uint32_t *change_index);
esp_err_t wallet_service_get_address(wallet_network_t network,
                                     uint32_t account,
                                     uint32_t change,
                                     uint32_t index,
                                     char *address,
                                     size_t address_size);
esp_err_t wallet_service_advance_receive_index(wallet_network_t network,
                                               uint32_t *new_index);
esp_err_t wallet_service_get_bip84_xpub(wallet_network_t network,
                                        char *xpub,
                                        size_t xpub_size);
esp_err_t wallet_service_inspect_psbt_base64(wallet_network_t network,
                                             const char *base64,
                                             wallet_psbt_summary_t *summary);
esp_err_t wallet_service_sign_psbt_base64(wallet_network_t network,
                                          const char *base64,
                                          wallet_psbt_summary_t *summary,
                                          char *signed_base64,
                                          size_t signed_base64_size);

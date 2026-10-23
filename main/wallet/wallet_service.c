#include "wallet_service.h"

#include <string.h>

#include "esp_log.h"
#include "mbedtls/platform_util.h"
#include "wallet_crypto.h"
#include "wallet_storage.h"

static const char *TAG = "wallet_service";
static bool s_entered;
static bool s_initialized;
static bool s_backend_checked;
static bool s_backend_ready;
static bool s_wallet_exists;
static char s_mnemonic[WALLET_STORAGE_MNEMONIC_MAX_LEN];

esp_err_t wallet_service_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    esp_err_t ret = wallet_crypto_init();
    if (ret != ESP_OK) {
        return ret;
    }
    ret = wallet_storage_init();
    if (ret != ESP_OK) {
        return ret;
    }
    s_wallet_exists = wallet_storage_exists();
    s_initialized = true;
    return ESP_OK;
}

esp_err_t wallet_service_enter(void)
{
    esp_err_t init_ret = wallet_service_init();
    if (init_ret != ESP_OK) {
        return init_ret;
    }
    if (!s_backend_checked || !s_backend_ready) {
        esp_err_t ret = wallet_crypto_self_test();
        s_backend_checked = true;
        s_backend_ready = ret == ESP_OK;
        if (!s_backend_ready) {
            ESP_LOGE(TAG, "Wallet crypto self-test failed: %s", esp_err_to_name(ret));
            return ret;
        }
        ESP_LOGI(TAG, "Wallet crypto self-test passed");
    }
    s_entered = true;
    return ESP_OK;
}

void wallet_service_leave(void)
{
    wallet_service_lock();
    s_entered = false;
}

wallet_status_t wallet_service_get_status(void)
{
    if (!s_backend_ready) {
        return WALLET_STATUS_UNAVAILABLE;
    }
    if (!s_wallet_exists) {
        return WALLET_STATUS_UNINITIALIZED;
    }
    return s_mnemonic[0] == '\0' ? WALLET_STATUS_LOCKED : WALLET_STATUS_READY;
}

wallet_capabilities_t wallet_service_get_capabilities(void)
{
    return (wallet_capabilities_t){
        .bip39 = s_backend_ready,
        .bip32 = s_backend_ready,
        .encrypted_storage = s_backend_ready,
        .receive_address = s_backend_ready,
        .qr_scan = false,
        .psbt = s_backend_ready,
    };
}

esp_err_t wallet_service_generate_mnemonic(size_t word_count, char *mnemonic,
                                           size_t mnemonic_size)
{
    if (!s_backend_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    return wallet_crypto_generate_mnemonic(word_count, mnemonic, mnemonic_size);
}

esp_err_t wallet_service_create(const char *mnemonic, const char *pin)
{
    if (!s_entered || s_wallet_exists || !wallet_storage_pin_is_valid(pin)) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = wallet_crypto_validate_mnemonic(mnemonic);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = wallet_storage_store(mnemonic, pin);
    if (ret == ESP_OK) {
        ret = wallet_storage_reset_address_state();
        if (ret != ESP_OK) {
            wallet_storage_erase();
        }
    }
    if (ret == ESP_OK) {
        size_t length = strlen(mnemonic);
        if (length >= sizeof(s_mnemonic)) {
            return ESP_ERR_INVALID_SIZE;
        }
        memcpy(s_mnemonic, mnemonic, length + 1);
        s_wallet_exists = true;
    }
    return ret;
}

esp_err_t wallet_service_unlock(const char *pin)
{
    if (!s_entered || !s_backend_ready || !s_wallet_exists) {
        return ESP_ERR_INVALID_STATE;
    }

    wallet_service_lock();
    esp_err_t ret = wallet_storage_load(pin, s_mnemonic, sizeof(s_mnemonic));
    if (ret == ESP_OK) {
        ret = wallet_crypto_validate_mnemonic(s_mnemonic);
    }
    if (ret != ESP_OK) {
        wallet_service_lock();
    }
    return ret;
}

void wallet_service_lock(void)
{
    mbedtls_platform_zeroize(s_mnemonic, sizeof(s_mnemonic));
}

esp_err_t wallet_service_change_pin(const char *new_pin)
{
    if (!s_entered || s_mnemonic[0] == '\0' ||
        !wallet_storage_pin_is_valid(new_pin)) {
        return ESP_ERR_INVALID_STATE;
    }
    return wallet_storage_store(s_mnemonic, new_pin);
}

esp_err_t wallet_service_delete(void)
{
    if (!s_entered || !s_wallet_exists) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = wallet_storage_erase();
    if (ret == ESP_OK) {
        wallet_service_lock();
        s_wallet_exists = false;
    }
    return ret;
}

esp_err_t wallet_service_get_receive_address(wallet_network_t network,
                                             uint32_t index,
                                             char *address,
                                             size_t address_size)
{
    if (!s_entered || s_mnemonic[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }
    uint32_t account = 0;
    esp_err_t ret = wallet_service_get_address_state(network, &account, NULL, NULL);
    if (ret != ESP_OK) {
        return ret;
    }
    return wallet_service_get_address(network, account, 0, index,
                                      address, address_size);
}

static bool network_is_valid(wallet_network_t network)
{
    return network == WALLET_NETWORK_BITCOIN || network == WALLET_NETWORK_TESTNET;
}

esp_err_t wallet_service_get_address_state(wallet_network_t network,
                                           uint32_t *account,
                                           uint32_t *receive_index,
                                           uint32_t *change_index)
{
    if (!s_entered || s_mnemonic[0] == '\0' || !network_is_valid(network)) {
        return ESP_ERR_INVALID_STATE;
    }

    wallet_address_state_t state;
    esp_err_t ret = wallet_storage_load_address_state(&state);
    if (ret != ESP_OK) {
        return ret;
    }
    uint32_t network_index = (uint32_t)network;
    if (state.account > WALLET_ADDRESS_INDEX_MAX ||
        state.receive_index[network_index] > WALLET_ADDRESS_INDEX_MAX ||
        state.change_index[network_index] > WALLET_ADDRESS_INDEX_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (account) {
        *account = state.account;
    }
    if (receive_index) {
        *receive_index = state.receive_index[network_index];
    }
    if (change_index) {
        *change_index = state.change_index[network_index];
    }
    return ESP_OK;
}

esp_err_t wallet_service_get_address(wallet_network_t network,
                                     uint32_t account,
                                     uint32_t change,
                                     uint32_t index,
                                     char *address,
                                     size_t address_size)
{
    if (!s_entered || s_mnemonic[0] == '\0' || !network_is_valid(network) ||
        account > WALLET_ADDRESS_INDEX_MAX || index > WALLET_ADDRESS_INDEX_MAX ||
        change > 1U) {
        return ESP_ERR_INVALID_STATE;
    }
    return wallet_crypto_derive_bip84_address(s_mnemonic, "", network,
                                              account, change, index,
                                              address, address_size);
}

esp_err_t wallet_service_advance_receive_index(wallet_network_t network,
                                               uint32_t *new_index)
{
    if (!s_entered || s_mnemonic[0] == '\0' || !network_is_valid(network)) {
        return ESP_ERR_INVALID_STATE;
    }

    wallet_address_state_t state;
    esp_err_t ret = wallet_storage_load_address_state(&state);
    if (ret != ESP_OK) {
        return ret;
    }
    uint32_t network_index = (uint32_t)network;
    if (state.account > WALLET_ADDRESS_INDEX_MAX ||
        state.receive_index[network_index] >= WALLET_ADDRESS_INDEX_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }

    state.receive_index[network_index]++;
    ret = wallet_storage_save_address_state(&state);
    if (ret == ESP_OK && new_index) {
        *new_index = state.receive_index[network_index];
    }
    return ret;
}

esp_err_t wallet_service_get_bip84_xpub(wallet_network_t network,
                                        char *xpub,
                                        size_t xpub_size)
{
    uint32_t account = 0;
    esp_err_t ret = wallet_service_get_address_state(network, &account, NULL, NULL);
    if (ret != ESP_OK) {
        return ret;
    }
    return wallet_crypto_get_bip84_xpub(s_mnemonic, network, account,
                                        xpub, xpub_size);
}

esp_err_t wallet_service_inspect_psbt_base64(wallet_network_t network,
                                             const char *base64,
                                             wallet_psbt_summary_t *summary)
{
    uint32_t account = 0;
    esp_err_t ret = wallet_service_get_address_state(network, &account, NULL, NULL);
    if (ret != ESP_OK) {
        return ret;
    }
    return wallet_crypto_inspect_psbt_base64(s_mnemonic, network, account,
                                             base64, summary);
}

esp_err_t wallet_service_sign_psbt_base64(wallet_network_t network,
                                          const char *base64,
                                          wallet_psbt_summary_t *summary,
                                          char *signed_base64,
                                          size_t signed_base64_size)
{
    uint32_t account = 0;
    esp_err_t ret = wallet_service_get_address_state(network, &account, NULL, NULL);
    if (ret != ESP_OK) {
        return ret;
    }
    return wallet_crypto_sign_psbt_base64(s_mnemonic, network, account,
                                          base64, summary, signed_base64,
                                          signed_base64_size);
}

#include "wallet_crypto.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_random.h"
#include "mbedtls/platform_util.h"
#include "wally_address.h"
#include "wally_bip32.h"
#include "wally_bip39.h"
#include "wally_core.h"
#include "wally_map.h"
#include "wally_psbt.h"
#include "wally_psbt_members.h"
#include "wally_script.h"
#include "wally_transaction.h"

static const char *TAG = "wallet_crypto";
static bool s_initialized;

static esp_err_t wally_result(int result)
{
    if (result == WALLY_OK) {
        return ESP_OK;
    }
    if (result == WALLY_ENOMEM) {
        return ESP_ERR_NO_MEM;
    }
    if (result == WALLY_EINVAL) {
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_FAIL;
}

static bool network_is_valid(wallet_network_t network)
{
    return network == WALLET_NETWORK_BITCOIN || network == WALLET_NETWORK_TESTNET;
}

static esp_err_t derive_root_key(const char *mnemonic, wallet_network_t network,
                                 struct ext_key *root)
{
    if (!mnemonic || !root || !network_is_valid(network)) {
        return ESP_ERR_INVALID_ARG;
    }

    unsigned char seed[BIP39_SEED_LEN_512] = {0};
    int wret = bip39_mnemonic_to_seed512(mnemonic, "", seed, sizeof(seed));
    if (wret == WALLY_OK) {
        const uint32_t version = network == WALLET_NETWORK_BITCOIN
                                     ? BIP32_VER_MAIN_PRIVATE
                                     : BIP32_VER_TEST_PRIVATE;
        wret = bip32_key_from_seed(seed, sizeof(seed), version, 0, root);
    }
    mbedtls_platform_zeroize(seed, sizeof(seed));
    return wally_result(wret);
}

esp_err_t wallet_crypto_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    esp_err_t ret = wally_result(wally_init(0));
    if (ret == ESP_OK) {
        s_initialized = true;
    }
    return ret;
}

esp_err_t wallet_crypto_validate_mnemonic(const char *mnemonic)
{
    if (!mnemonic || mnemonic[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = wallet_crypto_init();
    if (ret != ESP_OK) {
        return ret;
    }
    return wally_result(bip39_mnemonic_validate(NULL, mnemonic));
}

esp_err_t wallet_crypto_generate_mnemonic(size_t word_count,
                                          char *mnemonic,
                                          size_t mnemonic_size)
{
    if (!mnemonic || mnemonic_size == 0 || (word_count != 12 && word_count != 24)) {
        return ESP_ERR_INVALID_ARG;
    }

    mnemonic[0] = '\0';
    esp_err_t ret = wallet_crypto_init();
    if (ret != ESP_OK) {
        return ret;
    }

    unsigned char entropy[BIP39_ENTROPY_LEN_256] = {0};
    size_t entropy_size = word_count == 12 ? BIP39_ENTROPY_LEN_128 : BIP39_ENTROPY_LEN_256;
    char *generated = NULL;
    esp_fill_random(entropy, entropy_size);

    int wret = bip39_mnemonic_from_bytes(NULL, entropy, entropy_size, &generated);
    if (wret == WALLY_OK) {
        size_t length = strlen(generated);
        if (length >= mnemonic_size) {
            ret = ESP_ERR_INVALID_SIZE;
        } else {
            memcpy(mnemonic, generated, length + 1);
            ret = ESP_OK;
        }
    } else {
        ret = wally_result(wret);
    }

    if (generated) {
        wally_free_string(generated);
    }
    mbedtls_platform_zeroize(entropy, sizeof(entropy));
    return ret;
}

esp_err_t wallet_crypto_derive_bip84_address(const char *mnemonic,
                                             const char *passphrase,
                                             wallet_network_t network,
                                             uint32_t account,
                                             uint32_t change,
                                             uint32_t index,
                                             char *address,
                                             size_t address_size)
{
    if (!mnemonic || !address || address_size == 0 || account >= BIP32_INITIAL_HARDENED_CHILD ||
        change > 1 || index >= BIP32_INITIAL_HARDENED_CHILD ||
        (network != WALLET_NETWORK_BITCOIN && network != WALLET_NETWORK_TESTNET)) {
        return ESP_ERR_INVALID_ARG;
    }

    address[0] = '\0';
    esp_err_t ret = wallet_crypto_validate_mnemonic(mnemonic);
    if (ret != ESP_OK) {
        return ret;
    }

    unsigned char seed[BIP39_SEED_LEN_512] = {0};
    struct ext_key root = {0};
    struct ext_key child = {0};
    char *derived_address = NULL;
    const uint32_t coin_type = network == WALLET_NETWORK_BITCOIN ? 0U : 1U;
    const uint32_t path[] = {
        BIP32_INITIAL_HARDENED_CHILD + 84U,
        BIP32_INITIAL_HARDENED_CHILD + coin_type,
        BIP32_INITIAL_HARDENED_CHILD + account,
        change,
        index,
    };

    int wret = bip39_mnemonic_to_seed512(mnemonic, passphrase ? passphrase : "",
                                          seed, sizeof(seed));
    if (wret == WALLY_OK) {
        uint32_t version = network == WALLET_NETWORK_BITCOIN
                               ? BIP32_VER_MAIN_PRIVATE
                               : BIP32_VER_TEST_PRIVATE;
        wret = bip32_key_from_seed(seed, sizeof(seed), version, 0, &root);
    }
    if (wret == WALLY_OK) {
        wret = bip32_key_from_parent_path(&root, path, sizeof(path) / sizeof(path[0]),
                                          BIP32_FLAG_KEY_PRIVATE, &child);
    }
    if (wret == WALLY_OK) {
        wret = wally_bip32_key_to_addr_segwit(&child,
                                               network == WALLET_NETWORK_BITCOIN ? "bc" : "tb",
                                               0, &derived_address);
    }
    if (wret == WALLY_OK) {
        size_t length = strlen(derived_address);
        if (length >= address_size) {
            ret = ESP_ERR_INVALID_SIZE;
        } else {
            memcpy(address, derived_address, length + 1);
            ret = ESP_OK;
        }
    } else {
        ret = wally_result(wret);
    }

    if (derived_address) {
        wally_free_string(derived_address);
    }
    mbedtls_platform_zeroize(&child, sizeof(child));
    mbedtls_platform_zeroize(&root, sizeof(root));
    mbedtls_platform_zeroize(seed, sizeof(seed));
    return ret;
}

esp_err_t wallet_crypto_get_bip84_xpub(const char *mnemonic,
                                       wallet_network_t network,
                                       uint32_t account,
                                       char *xpub,
                                       size_t xpub_size)
{
    if (!mnemonic || !xpub || xpub_size == 0 || !network_is_valid(network) ||
        account >= BIP32_INITIAL_HARDENED_CHILD) {
        return ESP_ERR_INVALID_ARG;
    }
    xpub[0] = '\0';

    struct ext_key root = {0};
    struct ext_key derived = {0};
    char *encoded = NULL;
    esp_err_t ret = derive_root_key(mnemonic, network, &root);
    const uint32_t coin_type = network == WALLET_NETWORK_BITCOIN ? 0U : 1U;
    const uint32_t path[] = {
        BIP32_INITIAL_HARDENED_CHILD + 84U,
        BIP32_INITIAL_HARDENED_CHILD + coin_type,
        BIP32_INITIAL_HARDENED_CHILD + account,
    };
    if (ret == ESP_OK) {
        ret = wally_result(bip32_key_from_parent_path(
            &root, path, sizeof(path) / sizeof(path[0]),
            BIP32_FLAG_KEY_PRIVATE, &derived));
    }
    if (ret == ESP_OK) {
        ret = wally_result(bip32_key_to_base58(&derived, BIP32_FLAG_KEY_PUBLIC,
                                                &encoded));
    }
    if (ret == ESP_OK) {
        size_t length = strlen(encoded);
        if (length >= xpub_size) {
            ret = ESP_ERR_INVALID_SIZE;
        } else {
            memcpy(xpub, encoded, length + 1);
        }
    }

    if (encoded) {
        wally_free_string(encoded);
    }
    if (ret != ESP_OK) {
        xpub[0] = '\0';
    }
    mbedtls_platform_zeroize(&derived, sizeof(derived));
    mbedtls_platform_zeroize(&root, sizeof(root));
    return ret;
}

static bool bip84_path_is_valid(const uint32_t path[5], wallet_network_t network,
                                uint32_t account)
{
    const uint32_t coin_type = network == WALLET_NETWORK_BITCOIN ? 0U : 1U;
    return path[0] == BIP32_INITIAL_HARDENED_CHILD + 84U &&
           path[1] == BIP32_INITIAL_HARDENED_CHILD + coin_type &&
           path[2] == BIP32_INITIAL_HARDENED_CHILD + account &&
           path[3] <= 1U && path[4] < BIP32_INITIAL_HARDENED_CHILD;
}

static esp_err_t expected_bip84_script(const struct ext_key *child,
                                       uint8_t script[WALLY_SCRIPTPUBKEY_P2WPKH_LEN])
{
    size_t written = 0;
    int wret = wally_witness_program_from_bytes(
        child->pub_key, sizeof(child->pub_key), WALLY_SCRIPT_HASH160,
        script, WALLY_SCRIPTPUBKEY_P2WPKH_LEN, &written);
    return wret == WALLY_OK && written == WALLY_SCRIPTPUBKEY_P2WPKH_LEN
               ? ESP_OK
               : ESP_FAIL;
}

static esp_err_t inspect_keypath_item(const struct wally_map *keypaths,
                                      size_t item_index,
                                      const struct ext_key *root,
                                      wallet_network_t network,
                                      uint32_t account,
                                      bool *belongs_to_wallet,
                                      uint32_t path_out[5],
                                      struct ext_key *child_out)
{
    uint8_t fingerprint[BIP32_KEY_FINGERPRINT_LEN];
    *belongs_to_wallet = false;
    if (wally_map_keypath_get_item_fingerprint(keypaths, item_index,
                                                fingerprint,
                                                sizeof(fingerprint)) != WALLY_OK) {
        return ESP_ERR_INVALID_ARG;
    }
    if (memcmp(fingerprint, root->hash160, sizeof(fingerprint)) != 0) {
        return ESP_OK;
    }

    size_t path_len = 0;
    if (wally_map_keypath_get_item_path(keypaths, item_index, path_out, 5,
                                        &path_len) != WALLY_OK ||
        path_len != 5 || !bip84_path_is_valid(path_out, network, account)) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (bip32_key_from_parent_path(root, path_out, path_len,
                                   BIP32_FLAG_KEY_PRIVATE, child_out) != WALLY_OK) {
        return ESP_FAIL;
    }

    uint8_t public_key[65];
    size_t public_key_len = 0;
    if (wally_map_get_item_key(keypaths, item_index, public_key,
                               sizeof(public_key), &public_key_len) != WALLY_OK ||
        public_key_len != sizeof(child_out->pub_key) ||
        memcmp(public_key, child_out->pub_key, public_key_len) != 0) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    *belongs_to_wallet = true;
    return ESP_OK;
}

static esp_err_t script_to_address(const uint8_t *script, size_t script_len,
                                   wallet_network_t network, char *address,
                                   size_t address_size)
{
    size_t script_type = WALLY_SCRIPT_TYPE_UNKNOWN;
    if (wally_scriptpubkey_get_type(script, script_len, &script_type) != WALLY_OK) {
        return ESP_ERR_INVALID_ARG;
    }
    if (script_type == WALLY_SCRIPT_TYPE_OP_RETURN) {
        snprintf(address, address_size, "OP_RETURN");
        return ESP_OK;
    }

    char *encoded = NULL;
    int wret;
    if (script_type == WALLY_SCRIPT_TYPE_P2WPKH ||
        script_type == WALLY_SCRIPT_TYPE_P2WSH ||
        script_type == WALLY_SCRIPT_TYPE_P2TR) {
        wret = wally_addr_segwit_from_bytes(
            script, script_len,
            network == WALLET_NETWORK_BITCOIN ? "bc" : "tb", 0, &encoded);
    } else if (script_type == WALLY_SCRIPT_TYPE_P2PKH ||
               script_type == WALLY_SCRIPT_TYPE_P2SH) {
        wret = wally_scriptpubkey_to_address(
            script, script_len,
            network == WALLET_NETWORK_BITCOIN
                ? WALLY_NETWORK_BITCOIN_MAINNET
                : WALLY_NETWORK_BITCOIN_TESTNET,
            &encoded);
    } else {
        return ESP_ERR_NOT_SUPPORTED;
    }

    esp_err_t ret = wally_result(wret);
    if (ret == ESP_OK) {
        size_t length = strlen(encoded);
        if (length >= address_size) {
            ret = ESP_ERR_INVALID_SIZE;
        } else {
            memcpy(address, encoded, length + 1);
        }
    }
    if (encoded) {
        wally_free_string(encoded);
    }
    return ret;
}

static esp_err_t add_amount(uint64_t *total, uint64_t amount)
{
    if (UINT64_MAX - *total < amount) {
        return ESP_ERR_INVALID_SIZE;
    }
    *total += amount;
    return ESP_OK;
}

static esp_err_t inspect_psbt(const struct wally_psbt *psbt,
                              const struct ext_key *root,
                              wallet_network_t network,
                              uint32_t account,
                              wallet_psbt_summary_t *summary)
{
    static const uint8_t psbt_magic[5] = {'p', 's', 'b', 't', 0xff};
    if (memcmp(psbt->magic, psbt_magic, sizeof(psbt_magic)) != 0 ||
        psbt->num_inputs == 0 || psbt->num_outputs == 0) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    memset(summary, 0, sizeof(*summary));
    summary->input_count = psbt->num_inputs;
    summary->output_count = psbt->num_outputs;

    for (size_t input_index = 0; input_index < psbt->num_inputs; ++input_index) {
        const struct wally_psbt_input *input = &psbt->inputs[input_index];
        const struct wally_tx_output *utxo = NULL;
        if (wally_psbt_get_input_best_utxo(psbt, input_index, &utxo) != WALLY_OK ||
            !utxo || add_amount(&summary->input_amount, utxo->satoshi) != ESP_OK) {
            return ESP_ERR_INVALID_ARG;
        }

        size_t wallet_key_count = 0;
        for (size_t item = 0; item < input->keypaths.num_items; ++item) {
            bool belongs = false;
            uint32_t path[5];
            struct ext_key child = {0};
            esp_err_t ret = inspect_keypath_item(&input->keypaths, item, root,
                                                  network, account, &belongs,
                                                  path, &child);
            if (ret != ESP_OK) {
                mbedtls_platform_zeroize(&child, sizeof(child));
                return ret;
            }
            if (belongs) {
                uint8_t expected[WALLY_SCRIPTPUBKEY_P2WPKH_LEN];
                ret = expected_bip84_script(&child, expected);
                if (ret != ESP_OK || utxo->script_len != sizeof(expected) ||
                    memcmp(utxo->script, expected, sizeof(expected)) != 0) {
                    mbedtls_platform_zeroize(&child, sizeof(child));
                    return ESP_ERR_INVALID_RESPONSE;
                }
                wallet_key_count++;
            }
            mbedtls_platform_zeroize(&child, sizeof(child));
        }
        if (wallet_key_count) {
            uint32_t signature_type = 0;
            if (wallet_key_count != 1 || input->keypaths.num_items != 1 ||
                input->taproot_leaf_paths.num_items != 0 ||
                (input->sighash != 0 && input->sighash != WALLY_SIGHASH_ALL) ||
                wally_psbt_get_input_signature_type(psbt, input_index,
                                                     &signature_type) != WALLY_OK ||
                signature_type != WALLY_SIGTYPE_SW_V0) {
                return ESP_ERR_NOT_SUPPORTED;
            }
            summary->signing_input_count++;
        }
    }
    if (summary->signing_input_count == 0) {
        return ESP_ERR_NOT_FOUND;
    }

    struct wally_tx *allocated_tx = NULL;
    const struct wally_tx *tx = psbt->tx;
    if (psbt->version != WALLY_PSBT_VERSION_0) {
        if (wally_psbt_extract(psbt, WALLY_PSBT_EXTRACT_NON_FINAL,
                               &allocated_tx) != WALLY_OK) {
            return ESP_ERR_INVALID_ARG;
        }
        tx = allocated_tx;
    }
    if (!tx || tx->num_outputs != psbt->num_outputs) {
        if (allocated_tx) {
            wally_tx_free(allocated_tx);
        }
        return ESP_ERR_INVALID_ARG;
    }

    uint64_t output_amount = 0;
    esp_err_t result = ESP_OK;
    for (size_t output_index = 0; output_index < psbt->num_outputs; ++output_index) {
        const struct wally_psbt_output *psbt_output = &psbt->outputs[output_index];
        const struct wally_tx_output *tx_output = &tx->outputs[output_index];
        if (add_amount(&output_amount, tx_output->satoshi) != ESP_OK) {
            result = ESP_ERR_INVALID_SIZE;
            break;
        }

        bool is_change = false;
        size_t wallet_key_count = 0;
        for (size_t item = 0; item < psbt_output->keypaths.num_items; ++item) {
            bool belongs = false;
            uint32_t path[5];
            struct ext_key child = {0};
            result = inspect_keypath_item(&psbt_output->keypaths, item, root,
                                          network, account, &belongs, path,
                                          &child);
            if (result == ESP_OK && belongs) {
                uint8_t expected[WALLY_SCRIPTPUBKEY_P2WPKH_LEN];
                result = expected_bip84_script(&child, expected);
                if (result == ESP_OK &&
                    (tx_output->script_len != sizeof(expected) ||
                     memcmp(tx_output->script, expected, sizeof(expected)) != 0)) {
                    result = ESP_ERR_INVALID_RESPONSE;
                }
                wallet_key_count++;
                is_change = path[3] == 1U;
            }
            mbedtls_platform_zeroize(&child, sizeof(child));
            if (result != ESP_OK) {
                break;
            }
        }
        if (result != ESP_OK) {
            break;
        }
        if (wallet_key_count > 1) {
            result = ESP_ERR_NOT_SUPPORTED;
            break;
        }
        if (is_change) {
            result = add_amount(&summary->change_amount, tx_output->satoshi);
            if (result != ESP_OK) {
                break;
            }
            continue;
        }
        if (summary->external_output_count >= WALLET_PSBT_MAX_EXTERNAL_OUTPUTS) {
            result = ESP_ERR_INVALID_SIZE;
            break;
        }
        wallet_psbt_output_summary_t *external =
            &summary->external_outputs[summary->external_output_count];
        external->amount = tx_output->satoshi;
        result = script_to_address(tx_output->script, tx_output->script_len,
                                   network, external->address,
                                   sizeof(external->address));
        if (result != ESP_OK ||
            add_amount(&summary->external_amount, tx_output->satoshi) != ESP_OK) {
            if (result == ESP_OK) {
                result = ESP_ERR_INVALID_SIZE;
            }
            break;
        }
        summary->external_output_count++;
    }
    if (allocated_tx) {
        wally_tx_free(allocated_tx);
    }
    if (result != ESP_OK) {
        return result;
    }
    if (output_amount > summary->input_amount) {
        return ESP_ERR_INVALID_ARG;
    }
    summary->fee_amount = summary->input_amount - output_amount;
    return ESP_OK;
}

static esp_err_t process_psbt_base64(const char *mnemonic,
                                     wallet_network_t network,
                                     uint32_t account,
                                     const char *base64,
                                     bool sign,
                                     wallet_psbt_summary_t *summary,
                                     char *signed_base64,
                                     size_t signed_base64_size)
{
    if (summary) {
        memset(summary, 0, sizeof(*summary));
    }
    if (sign && signed_base64 && signed_base64_size > 0) {
        signed_base64[0] = '\0';
    }
    if (!mnemonic || !base64 || !summary || !network_is_valid(network) ||
        account >= BIP32_INITIAL_HARDENED_CHILD ||
        (sign && (!signed_base64 || signed_base64_size == 0))) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t input_len = strlen(base64);
    if (input_len == 0 || input_len >= WALLET_PSBT_BASE64_MAX_LEN) {
        return ESP_ERR_INVALID_SIZE;
    }
    char *normalized = malloc(input_len + 1);
    if (!normalized) {
        return ESP_ERR_NO_MEM;
    }
    size_t normalized_len = 0;
    for (size_t i = 0; i < input_len; ++i) {
        if (!isspace((unsigned char)base64[i])) {
            normalized[normalized_len++] = base64[i];
        }
    }
    normalized[normalized_len] = '\0';

    struct wally_psbt *psbt = NULL;
    struct ext_key root = {0};
    char *encoded = NULL;
    esp_err_t ret = derive_root_key(mnemonic, network, &root);
    if (ret == ESP_OK) {
        ret = wally_result(wally_psbt_from_base64_n(
            normalized, normalized_len,
            WALLY_PSBT_PARSE_FLAG_STRICT | WALLY_PSBT_PARSE_FLAG_COMPLETE,
            &psbt));
    }
    if (ret == ESP_OK) {
        ret = inspect_psbt(psbt, &root, network, account, summary);
    }
    if (ret == ESP_OK && sign) {
        ret = wally_result(wally_psbt_signing_cache_enable(psbt, 0));
    }
    if (ret == ESP_OK && sign) {
        ret = wally_result(wally_psbt_sign_bip32(psbt, &root, EC_FLAG_GRIND_R));
    }
    if (ret == ESP_OK && sign) {
        ret = wally_result(wally_psbt_to_base64(psbt, 0, &encoded));
    }
    if (ret == ESP_OK && sign) {
        size_t encoded_len = strlen(encoded);
        if (encoded_len >= signed_base64_size) {
            ret = ESP_ERR_INVALID_SIZE;
        } else {
            memcpy(signed_base64, encoded, encoded_len + 1);
        }
    }

    if (encoded) {
        wally_free_string(encoded);
    }
    if (psbt) {
        wally_psbt_free(psbt);
    }
    mbedtls_platform_zeroize(&root, sizeof(root));
    mbedtls_platform_zeroize(normalized, input_len + 1);
    free(normalized);
    if (ret != ESP_OK) {
        memset(summary, 0, sizeof(*summary));
        if (sign) {
            signed_base64[0] = '\0';
        }
    }
    return ret;
}

esp_err_t wallet_crypto_inspect_psbt_base64(const char *mnemonic,
                                            wallet_network_t network,
                                            uint32_t account,
                                            const char *base64,
                                            wallet_psbt_summary_t *summary)
{
    return process_psbt_base64(mnemonic, network, account, base64, false,
                               summary, NULL, 0);
}

esp_err_t wallet_crypto_sign_psbt_base64(const char *mnemonic,
                                         wallet_network_t network,
                                         uint32_t account,
                                         const char *base64,
                                         wallet_psbt_summary_t *summary,
                                         char *signed_base64,
                                         size_t signed_base64_size)
{
    return process_psbt_base64(mnemonic, network, account, base64, true,
                               summary, signed_base64, signed_base64_size);
}

esp_err_t wallet_crypto_self_test(void)
{
    static const char mnemonic[] =
        "abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon about";
    static const char expected_address[] = "bc1qcr8te4kr609gcawutmrza0j4xv80jy8z306fyu";
    char address[96];

    esp_err_t ret = wallet_crypto_derive_bip84_address(mnemonic, "",
                                                       WALLET_NETWORK_BITCOIN,
                                                       0, 0, 0,
                                                       address, sizeof(address));
    if (ret == ESP_OK && strcmp(address, expected_address) != 0) {
        ESP_LOGE(TAG, "BIP84 self-test mismatch");
        ret = ESP_ERR_INVALID_RESPONSE;
    }
    mbedtls_platform_zeroize(address, sizeof(address));
    return ret;
}

esp_err_t wallet_crypto_validate_psbt(const uint8_t *psbt, size_t psbt_size)
{
    if (!psbt || psbt_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = wallet_crypto_init();
    if (ret != ESP_OK) {
        return ret;
    }

    struct wally_psbt *parsed = NULL;
    int wret = wally_psbt_from_bytes(psbt, psbt_size,
                                     WALLY_PSBT_PARSE_FLAG_STRICT |
                                         WALLY_PSBT_PARSE_FLAG_COMPLETE,
                                     &parsed);
    ret = wally_result(wret);
    if (parsed) {
        wally_psbt_free(parsed);
    }
    return ret;
}

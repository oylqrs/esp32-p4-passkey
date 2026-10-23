#include "wallet_storage.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "esp_log.h"
#include "esp_random.h"
#include "mbedtls/gcm.h"
#include "mbedtls/md.h"
#include "mbedtls/pkcs5.h"
#include "mbedtls/platform_util.h"
#include "nvs.h"

#define WALLET_NVS_NAMESPACE "jade_wallet"
#define WALLET_NVS_BLOB_KEY  "seed_blob"
#define WALLET_NVS_ADDRESS_KEY "addr_state"

#define WALLET_BLOB_VERSION      1U
#define WALLET_KDF_ITERATIONS    100000U
#define WALLET_SALT_LEN          16U
#define WALLET_NONCE_LEN         12U
#define WALLET_KEY_LEN           32U
#define WALLET_TAG_LEN           16U
#define WALLET_PAYLOAD_MAGIC_LEN 4U
#define WALLET_PAYLOAD_LEN       (WALLET_STORAGE_MNEMONIC_MAX_LEN + 8U)
#define WALLET_ADDRESS_STATE_VERSION 1U

typedef struct __attribute__((packed)) {
    uint8_t magic[4];
    uint8_t version;
    uint8_t reserved[3];
    uint32_t iterations;
    uint8_t salt[WALLET_SALT_LEN];
    uint8_t nonce[WALLET_NONCE_LEN];
    uint8_t ciphertext[WALLET_PAYLOAD_LEN];
    uint8_t tag[WALLET_TAG_LEN];
} wallet_blob_t;

typedef struct __attribute__((packed)) {
    uint8_t magic[4];
    uint8_t version;
    uint8_t reserved[3];
    wallet_address_state_t state;
} wallet_address_state_blob_t;

static const char *TAG = "wallet_storage";
static const uint8_t s_blob_magic[4] = {'J', 'W', 'L', 'T'};
static const uint8_t s_payload_magic[WALLET_PAYLOAD_MAGIC_LEN] = {'S', 'E', 'E', 'D'};
static const uint8_t s_address_magic[4] = {'J', 'A', 'D', 'R'};

static esp_err_t mbedtls_result(int result)
{
    return result == 0 ? ESP_OK : ESP_FAIL;
}

bool wallet_storage_pin_is_valid(const char *pin)
{
    if (!pin) {
        return false;
    }

    size_t length = strlen(pin);
    if (length < WALLET_STORAGE_PIN_MIN_LEN || length > WALLET_STORAGE_PIN_MAX_LEN) {
        return false;
    }
    for (size_t i = 0; i < length; ++i) {
        if (pin[i] < '0' || pin[i] > '9') {
            return false;
        }
    }
    return true;
}

static esp_err_t derive_key(const char *pin, const wallet_blob_t *blob,
                            uint8_t key[WALLET_KEY_LEN])
{
    if (!wallet_storage_pin_is_valid(pin) || !blob ||
        blob->iterations < 10000U || blob->iterations > 1000000U) {
        return ESP_ERR_INVALID_ARG;
    }

    int result = mbedtls_pkcs5_pbkdf2_hmac_ext(
        MBEDTLS_MD_SHA256, (const unsigned char *)pin, strlen(pin),
        blob->salt, sizeof(blob->salt), blob->iterations, WALLET_KEY_LEN, key);
    return mbedtls_result(result);
}

static size_t blob_aad_len(void)
{
    return offsetof(wallet_blob_t, ciphertext);
}

esp_err_t wallet_storage_init(void)
{
    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(WALLET_NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (ret == ESP_OK) {
        nvs_close(nvs);
    }
    return ret;
}

bool wallet_storage_exists(void)
{
    nvs_handle_t nvs;
    if (nvs_open(WALLET_NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        return false;
    }

    size_t size = 0;
    esp_err_t ret = nvs_get_blob(nvs, WALLET_NVS_BLOB_KEY, NULL, &size);
    nvs_close(nvs);
    return ret == ESP_OK && size == sizeof(wallet_blob_t);
}

esp_err_t wallet_storage_store(const char *mnemonic, const char *pin)
{
    if (!mnemonic || !wallet_storage_pin_is_valid(pin)) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t mnemonic_len = strlen(mnemonic);
    if (mnemonic_len == 0 || mnemonic_len >= WALLET_STORAGE_MNEMONIC_MAX_LEN) {
        return ESP_ERR_INVALID_SIZE;
    }

    wallet_blob_t blob = {0};
    uint8_t plaintext[WALLET_PAYLOAD_LEN];
    uint8_t key[WALLET_KEY_LEN] = {0};
    esp_fill_random(plaintext, sizeof(plaintext));
    memcpy(blob.magic, s_blob_magic, sizeof(blob.magic));
    blob.version = WALLET_BLOB_VERSION;
    blob.iterations = WALLET_KDF_ITERATIONS;
    esp_fill_random(blob.salt, sizeof(blob.salt));
    esp_fill_random(blob.nonce, sizeof(blob.nonce));
    memcpy(plaintext, s_payload_magic, sizeof(s_payload_magic));
    plaintext[4] = (uint8_t)(mnemonic_len & 0xffU);
    plaintext[5] = (uint8_t)(mnemonic_len >> 8U);
    memcpy(plaintext + 6, mnemonic, mnemonic_len);
    plaintext[6 + mnemonic_len] = '\0';

    esp_err_t ret = derive_key(pin, &blob, key);
    mbedtls_gcm_context gcm;
    mbedtls_gcm_init(&gcm);
    if (ret == ESP_OK) {
        ret = mbedtls_result(mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES,
                                                key, WALLET_KEY_LEN * 8U));
    }
    if (ret == ESP_OK) {
        ret = mbedtls_result(mbedtls_gcm_crypt_and_tag(
            &gcm, MBEDTLS_GCM_ENCRYPT, sizeof(plaintext),
            blob.nonce, sizeof(blob.nonce), (const uint8_t *)&blob, blob_aad_len(),
            plaintext, blob.ciphertext, sizeof(blob.tag), blob.tag));
    }

    if (ret == ESP_OK) {
        nvs_handle_t nvs;
        ret = nvs_open(WALLET_NVS_NAMESPACE, NVS_READWRITE, &nvs);
        if (ret == ESP_OK) {
            ret = nvs_set_blob(nvs, WALLET_NVS_BLOB_KEY, &blob, sizeof(blob));
            if (ret == ESP_OK) {
                ret = nvs_commit(nvs);
            }
            nvs_close(nvs);
        }
    }

    mbedtls_gcm_free(&gcm);
    mbedtls_platform_zeroize(key, sizeof(key));
    mbedtls_platform_zeroize(plaintext, sizeof(plaintext));
    mbedtls_platform_zeroize(&blob, sizeof(blob));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to store encrypted wallet: %s", esp_err_to_name(ret));
    }
    return ret;
}

esp_err_t wallet_storage_load(const char *pin, char *mnemonic, size_t mnemonic_size)
{
    if (!wallet_storage_pin_is_valid(pin) || !mnemonic || mnemonic_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    mnemonic[0] = '\0';

    wallet_blob_t blob = {0};
    uint8_t plaintext[WALLET_PAYLOAD_LEN] = {0};
    uint8_t key[WALLET_KEY_LEN] = {0};
    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(WALLET_NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (ret == ESP_OK) {
        size_t size = sizeof(blob);
        ret = nvs_get_blob(nvs, WALLET_NVS_BLOB_KEY, &blob, &size);
        nvs_close(nvs);
        if (ret == ESP_OK && size != sizeof(blob)) {
            ret = ESP_ERR_INVALID_SIZE;
        }
    }

    if (ret == ESP_OK &&
        (memcmp(blob.magic, s_blob_magic, sizeof(blob.magic)) != 0 ||
         blob.version != WALLET_BLOB_VERSION)) {
        ret = ESP_ERR_INVALID_VERSION;
    }
    if (ret == ESP_OK) {
        ret = derive_key(pin, &blob, key);
    }

    mbedtls_gcm_context gcm;
    mbedtls_gcm_init(&gcm);
    if (ret == ESP_OK) {
        ret = mbedtls_result(mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES,
                                                key, WALLET_KEY_LEN * 8U));
    }
    if (ret == ESP_OK) {
        int result = mbedtls_gcm_auth_decrypt(
            &gcm, sizeof(blob.ciphertext), blob.nonce, sizeof(blob.nonce),
            (const uint8_t *)&blob, blob_aad_len(), blob.tag, sizeof(blob.tag),
            blob.ciphertext, plaintext);
        ret = result == 0 ? ESP_OK : ESP_ERR_INVALID_CRC;
    }

    if (ret == ESP_OK && memcmp(plaintext, s_payload_magic, sizeof(s_payload_magic)) != 0) {
        ret = ESP_ERR_INVALID_RESPONSE;
    }
    if (ret == ESP_OK) {
        size_t length = (size_t)plaintext[4] | ((size_t)plaintext[5] << 8U);
        if (length == 0 || length >= WALLET_STORAGE_MNEMONIC_MAX_LEN ||
            length >= mnemonic_size || plaintext[6 + length] != '\0') {
            ret = ESP_ERR_INVALID_SIZE;
        } else {
            memcpy(mnemonic, plaintext + 6, length + 1);
        }
    }

    if (ret != ESP_OK) {
        mnemonic[0] = '\0';
    }
    mbedtls_gcm_free(&gcm);
    mbedtls_platform_zeroize(key, sizeof(key));
    mbedtls_platform_zeroize(plaintext, sizeof(plaintext));
    mbedtls_platform_zeroize(&blob, sizeof(blob));
    return ret;
}

esp_err_t wallet_storage_load_address_state(wallet_address_state_t *state)
{
    if (!state) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(state, 0, sizeof(*state));

    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(WALLET_NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (ret != ESP_OK) {
        return ret;
    }

    wallet_address_state_blob_t blob = {0};
    size_t size = sizeof(blob);
    ret = nvs_get_blob(nvs, WALLET_NVS_ADDRESS_KEY, &blob, &size);
    nvs_close(nvs);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (ret != ESP_OK) {
        return ret;
    }
    if (size != sizeof(blob) ||
        memcmp(blob.magic, s_address_magic, sizeof(blob.magic)) != 0 ||
        blob.version != WALLET_ADDRESS_STATE_VERSION) {
        return ESP_ERR_INVALID_VERSION;
    }

    *state = blob.state;
    return ESP_OK;
}

esp_err_t wallet_storage_save_address_state(const wallet_address_state_t *state)
{
    if (!state) {
        return ESP_ERR_INVALID_ARG;
    }

    wallet_address_state_blob_t blob = {
        .version = WALLET_ADDRESS_STATE_VERSION,
        .state = *state,
    };
    memcpy(blob.magic, s_address_magic, sizeof(blob.magic));

    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(WALLET_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (ret == ESP_OK) {
        ret = nvs_set_blob(nvs, WALLET_NVS_ADDRESS_KEY, &blob, sizeof(blob));
        if (ret == ESP_OK) {
            ret = nvs_commit(nvs);
        }
        nvs_close(nvs);
    }
    return ret;
}

esp_err_t wallet_storage_reset_address_state(void)
{
    const wallet_address_state_t state = {0};
    return wallet_storage_save_address_state(&state);
}

esp_err_t wallet_storage_erase(void)
{
    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(WALLET_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (ret != ESP_OK) {
        return ret == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : ret;
    }

    esp_err_t seed_ret = nvs_erase_key(nvs, WALLET_NVS_BLOB_KEY);
    esp_err_t address_ret = nvs_erase_key(nvs, WALLET_NVS_ADDRESS_KEY);
    if (seed_ret == ESP_ERR_NVS_NOT_FOUND) {
        seed_ret = ESP_OK;
    }
    if (address_ret == ESP_ERR_NVS_NOT_FOUND) {
        address_ret = ESP_OK;
    }
    ret = seed_ret != ESP_OK ? seed_ret : address_ret;
    if (ret == ESP_OK) {
        ret = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return ret;
}

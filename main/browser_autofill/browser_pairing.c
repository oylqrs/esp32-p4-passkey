#include "browser_autofill/browser_pairing.h"

#include <string.h>

#include "mbedtls/platform_util.h"
#include "nvs.h"

#define PAIRING_NVS_NAMESPACE "browser_pair"
#define PAIRING_NVS_ID_KEY "id"
#define PAIRING_NVS_SECRET_KEY "secret"
#define CONNECT_NVS_NAMESPACE "connect_ui"
#define CONNECT_TRUST_KEY "trust_browser"

bool browser_pairing_exists(void)
{
    nvs_handle_t nvs;
    if (nvs_open(PAIRING_NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        return false;
    }

    size_t id_size = 0;
    size_t key_size = 0;
    bool exists = nvs_get_str(nvs, PAIRING_NVS_ID_KEY, NULL, &id_size) == ESP_OK &&
                  id_size > 1 &&
                  nvs_get_blob(nvs, PAIRING_NVS_SECRET_KEY, NULL, &key_size) == ESP_OK &&
                  key_size == KEEPASS_HTTP_KEY_SIZE;
    nvs_close(nvs);
    return exists;
}

bool browser_pairing_trust_enabled(void)
{
    nvs_handle_t nvs;
    if (nvs_open(CONNECT_NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        return false;
    }
    uint8_t enabled = 0;
    (void)nvs_get_u8(nvs, CONNECT_TRUST_KEY, &enabled);
    nvs_close(nvs);
    return enabled != 0;
}

esp_err_t browser_pairing_store(const char *encoded_key,
                                char *id,
                                size_t id_size)
{
    if (!encoded_key || !id || id_size < BROWSER_PAIRING_ID_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t key[KEEPASS_HTTP_KEY_SIZE];
    esp_err_t ret = keepass_http_crypto_decode_key(encoded_key, key);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = keepass_http_crypto_random_base64(16, id, id_size);
    if (ret != ESP_OK) {
        mbedtls_platform_zeroize(key, sizeof(key));
        return ret;
    }

    nvs_handle_t nvs;
    ret = nvs_open(PAIRING_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (ret == ESP_OK) {
        ret = nvs_set_str(nvs, PAIRING_NVS_ID_KEY, id);
        if (ret == ESP_OK) {
            ret = nvs_set_blob(nvs, PAIRING_NVS_SECRET_KEY, key, sizeof(key));
        }
        if (ret == ESP_OK) {
            ret = nvs_commit(nvs);
        }
        nvs_close(nvs);
    }
    mbedtls_platform_zeroize(key, sizeof(key));
    return ret;
}

esp_err_t browser_pairing_get_key(
    const char *id, uint8_t key[KEEPASS_HTTP_KEY_SIZE])
{
    if (!id || !key) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(key, 0, KEEPASS_HTTP_KEY_SIZE);

    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(PAIRING_NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (ret != ESP_OK) {
        return ret;
    }

    char stored_id[BROWSER_PAIRING_ID_MAX];
    size_t id_size = sizeof(stored_id);
    ret = nvs_get_str(nvs, PAIRING_NVS_ID_KEY, stored_id, &id_size);
    if (ret == ESP_OK &&
        (strlen(id) != strlen(stored_id) ||
         memcmp(id, stored_id, strlen(stored_id)) != 0)) {
        ret = ESP_ERR_NOT_FOUND;
    }
    if (ret == ESP_OK) {
        size_t key_size = KEEPASS_HTTP_KEY_SIZE;
        ret = nvs_get_blob(nvs, PAIRING_NVS_SECRET_KEY, key, &key_size);
        if (ret == ESP_OK && key_size != KEEPASS_HTTP_KEY_SIZE) {
            ret = ESP_ERR_INVALID_SIZE;
        }
    }
    nvs_close(nvs);
    if (ret != ESP_OK) {
        mbedtls_platform_zeroize(key, KEEPASS_HTTP_KEY_SIZE);
    }
    return ret;
}

esp_err_t browser_pairing_clear(void)
{
    esp_err_t result = ESP_OK;
    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(PAIRING_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (ret == ESP_OK) {
        ret = nvs_erase_all(nvs);
        if (ret == ESP_OK) {
            ret = nvs_commit(nvs);
        }
        nvs_close(nvs);
    } else if (ret != ESP_ERR_NVS_NOT_FOUND) {
        result = ret;
    }

    ret = nvs_open(CONNECT_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (ret == ESP_OK) {
        ret = nvs_set_u8(nvs, CONNECT_TRUST_KEY, 0);
        if (ret == ESP_OK) {
            ret = nvs_commit(nvs);
        }
        nvs_close(nvs);
    }
    return result == ESP_OK ? ret : result;
}

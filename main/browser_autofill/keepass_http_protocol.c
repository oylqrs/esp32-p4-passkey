#include "browser_autofill/keepass_http_protocol.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "browser_autofill/browser_confirm.h"
#include "browser_autofill/browser_pairing.h"
#include "browser_autofill/credential_matcher.h"
#include "browser_autofill/keepass_http_crypto.h"
#include "cJSON.h"
#include "esp_log.h"
#include "mbedtls/platform_util.h"

#define CONFIRM_TIMEOUT_MS 9000
#define DECRYPTED_URL_MAX 512
#define ENCRYPTED_FIELD_MAX 256
#define VERIFIER_MAX 128

static const char *TAG = "keepass_protocol";

static const char *json_string(const cJSON *object, const char *name)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    return cJSON_IsString(item) && item->valuestring ? item->valuestring : NULL;
}

static cJSON *create_response(const char *request_type, bool success)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return NULL;
    }
    cJSON_AddStringToObject(root, "RequestType",
                            request_type ? request_type : "");
    cJSON_AddBoolToObject(root, "Success", success);
    cJSON_AddStringToObject(root, "Version", "1.8.4.2");
    cJSON_AddStringToObject(root, "Hash", "ESP32-P4");
    return root;
}

static void set_error(cJSON *response, const char *message)
{
    if (!response) {
        return;
    }
    cJSON_ReplaceItemInObject(response, "Success", cJSON_CreateFalse());
    cJSON_AddStringToObject(response, "Error", message ? message : "Error");
}

static esp_err_t load_authenticated_key(const cJSON *request,
                                        uint8_t key[KEEPASS_HTTP_KEY_SIZE])
{
    memset(key, 0, KEEPASS_HTTP_KEY_SIZE);
    const char *id = json_string(request, "Id");
    const char *nonce = json_string(request, "Nonce");
    const char *verifier = json_string(request, "Verifier");
    if (!id || !nonce || !verifier) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = browser_pairing_get_key(id, key);
    if (ret != ESP_OK) {
        return ret;
    }
    if (!keepass_http_crypto_verify(key, nonce, verifier)) {
        mbedtls_platform_zeroize(key, KEEPASS_HTTP_KEY_SIZE);
        return ESP_ERR_INVALID_CRC;
    }
    return ESP_OK;
}

static esp_err_t add_response_crypto(cJSON *response,
                                     const uint8_t key[KEEPASS_HTTP_KEY_SIZE],
                                     char nonce[KEEPASS_HTTP_NONCE_BASE64_SIZE])
{
    esp_err_t ret = keepass_http_crypto_random_base64(
        KEEPASS_HTTP_NONCE_SIZE, nonce, KEEPASS_HTTP_NONCE_BASE64_SIZE);
    if (ret != ESP_OK) {
        return ret;
    }

    char verifier[VERIFIER_MAX];
    ret = keepass_http_crypto_make_verifier(key, nonce,
                                             verifier, sizeof(verifier));
    if (ret != ESP_OK) {
        return ret;
    }
    cJSON_AddStringToObject(response, "Nonce", nonce);
    cJSON_AddStringToObject(response, "Verifier", verifier);
    return ESP_OK;
}

static esp_err_t encrypt_entry_field(
    cJSON *entry,
    const char *name,
    const char *plain,
    const uint8_t key[KEEPASS_HTTP_KEY_SIZE],
    const char *nonce)
{
    char encrypted[ENCRYPTED_FIELD_MAX];
    esp_err_t ret = keepass_http_crypto_encrypt(
        plain ? plain : "", key, nonce, encrypted, sizeof(encrypted));
    if (ret == ESP_OK &&
        !cJSON_AddStringToObject(entry, name, encrypted)) {
        ret = ESP_ERR_NO_MEM;
    }
    mbedtls_platform_zeroize(encrypted, sizeof(encrypted));
    return ret;
}

static void handle_associate(const cJSON *request, cJSON *response)
{
    const char *encoded_key = json_string(request, "Key");
    const char *nonce = json_string(request, "Nonce");
    const char *verifier = json_string(request, "Verifier");
    uint8_t key[KEEPASS_HTTP_KEY_SIZE];
    if (!encoded_key || !nonce || !verifier ||
        keepass_http_crypto_decode_key(encoded_key, key) != ESP_OK ||
        !keepass_http_crypto_verify(key, nonce, verifier)) {
        mbedtls_platform_zeroize(key, sizeof(key));
        set_error(response, "Invalid association request");
        return;
    }
    mbedtls_platform_zeroize(key, sizeof(key));

    if (!browser_confirm_request(BROWSER_CONFIRM_PAIR,
                                 "Allow this browser to access the password database?",
                                 CONFIRM_TIMEOUT_MS)) {
        set_error(response, "Association denied");
        return;
    }

    char id[BROWSER_PAIRING_ID_MAX];
    esp_err_t ret = browser_pairing_store(encoded_key, id, sizeof(id));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to store browser pairing: %s",
                 esp_err_to_name(ret));
        set_error(response, "Failed to store association");
        return;
    }
    cJSON_ReplaceItemInObject(response, "Success", cJSON_CreateTrue());
    cJSON_AddStringToObject(response, "Id", id);
}

static void handle_authenticated(const cJSON *request,
                                 cJSON *response,
                                 const char *request_type)
{
    uint8_t key[KEEPASS_HTTP_KEY_SIZE];
    esp_err_t ret = load_authenticated_key(request, key);
    if (ret != ESP_OK) {
        set_error(response, "Not associated");
        return;
    }

    const char *id = json_string(request, "Id");
    cJSON_AddStringToObject(response, "Id", id);
    char response_nonce[KEEPASS_HTTP_NONCE_BASE64_SIZE];
    ret = add_response_crypto(response, key, response_nonce);
    if (ret != ESP_OK) {
        set_error(response, "Failed to authenticate response");
        mbedtls_platform_zeroize(key, sizeof(key));
        return;
    }
    if (strcmp(request_type, "test-associate") == 0) {
        cJSON_ReplaceItemInObject(response, "Success", cJSON_CreateTrue());
        mbedtls_platform_zeroize(key, sizeof(key));
        return;
    }

    if (strcmp(request_type, "get-logins") != 0 &&
        strcmp(request_type, "get-logins-count") != 0) {
        set_error(response, "Request type not supported");
        mbedtls_platform_zeroize(key, sizeof(key));
        return;
    }

    const char *encrypted_url = json_string(request, "Url");
    const char *request_nonce = json_string(request, "Nonce");
    char url[DECRYPTED_URL_MAX];
    if (!encrypted_url ||
        keepass_http_crypto_decrypt(encrypted_url, key, request_nonce,
                                     url, sizeof(url)) != ESP_OK) {
        set_error(response, "Invalid URL");
        mbedtls_platform_zeroize(key, sizeof(key));
        return;
    }

    if (!browser_pairing_trust_enabled() &&
        !browser_confirm_request(BROWSER_CONFIRM_CREDENTIAL_ACCESS,
                                 url, CONFIRM_TIMEOUT_MS)) {
        set_error(response, "Credential access denied");
        mbedtls_platform_zeroize(url, sizeof(url));
        mbedtls_platform_zeroize(key, sizeof(key));
        return;
    }

    browser_credential_t credentials[BROWSER_AUTOFILL_MAX_RESULTS];
    size_t count = 0;
    ret = credential_matcher_find(url, credentials,
                                  BROWSER_AUTOFILL_MAX_RESULTS, &count);
    mbedtls_platform_zeroize(url, sizeof(url));
    if (ret != ESP_OK) {
        set_error(response, "Invalid login URL");
        mbedtls_platform_zeroize(key, sizeof(key));
        return;
    }
    cJSON_AddNumberToObject(response, "Count", (double)count);

    if (strcmp(request_type, "get-logins") == 0) {
        cJSON *entries = cJSON_AddArrayToObject(response, "Entries");
        if (!entries) {
            ret = ESP_ERR_NO_MEM;
        }

        for (size_t i = 0; ret == ESP_OK && i < count; i++) {
            cJSON *entry = cJSON_CreateObject();
            if (!entry || !cJSON_AddItemToArray(entries, entry)) {
                cJSON_Delete(entry);
                ret = ESP_ERR_NO_MEM;
                break;
            }
            ret = encrypt_entry_field(entry, "Login", credentials[i].username,
                                      key, response_nonce);
            if (ret == ESP_OK) {
                ret = encrypt_entry_field(entry, "Name", credentials[i].title,
                                          key, response_nonce);
            }
            if (ret == ESP_OK) {
                ret = encrypt_entry_field(entry, "Password",
                                          credentials[i].password,
                                          key, response_nonce);
            }
            if (ret == ESP_OK) {
                ret = encrypt_entry_field(entry, "Uuid", credentials[i].uuid,
                                          key, response_nonce);
            }
            if (ret == ESP_OK) {
                ret = encrypt_entry_field(entry, "StringFields", "",
                                          key, response_nonce);
            }
        }
    }

    mbedtls_platform_zeroize(credentials, sizeof(credentials));
    mbedtls_platform_zeroize(key, sizeof(key));
    if (ret != ESP_OK) {
        set_error(response, "Failed to encrypt response");
        return;
    }
    cJSON_ReplaceItemInObject(response, "Success", cJSON_CreateTrue());
}

esp_err_t keepass_http_protocol_handle(const char *request_text,
                                       char **response_text)
{
    if (!request_text || !response_text) {
        return ESP_ERR_INVALID_ARG;
    }
    *response_text = NULL;

    cJSON *request = cJSON_Parse(request_text);
    const char *request_type = request ? json_string(request, "RequestType")
                                       : NULL;
    cJSON *response = create_response(request_type, false);
    if (!response) {
        cJSON_Delete(request);
        return ESP_ERR_NO_MEM;
    }

    if (!request || !request_type) {
        set_error(response, "Invalid JSON request");
    } else if (strcmp(request_type, "associate") == 0) {
        handle_associate(request, response);
    } else {
        handle_authenticated(request, response, request_type);
    }

    *response_text = cJSON_PrintUnformatted(response);
    cJSON *association_key = request
                                 ? cJSON_GetObjectItemCaseSensitive(request, "Key")
                                 : NULL;
    if (cJSON_IsString(association_key) && association_key->valuestring) {
        mbedtls_platform_zeroize(association_key->valuestring,
                                 strlen(association_key->valuestring));
    }
    cJSON_Delete(response);
    cJSON_Delete(request);
    return *response_text ? ESP_OK : ESP_ERR_NO_MEM;
}

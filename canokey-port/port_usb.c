#include <stdbool.h>
#include <string.h>

#include "usb_device.h"
#include "usbd_ctaphid.h"

#include "canokey_esp32p4.h"
#include "canokey_nfc.h"
#include "class/hid/hid_device.h"
#include "ctaphid.h"
#include "device.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "tusb.h"

#define CANOKEY_CTAPHID_TINYUSB_ITF 1
#define CANOKEY_CTAPHID_REPORT_SIZE 64
#define CANOKEY_CTAPHID_EVENT_QUEUE_DEPTH 32
#define CANOKEY_CTAPHID_IN_TIMEOUT_MS 2000
#define CANOKEY_CTAPHID_DEBUG_CBOR_BUFFER_SIZE 1024

USBD_HandleTypeDef usb_device;

static volatile CTAPHID_StateTypeDef s_ctaphid_state = CTAPHID_IDLE;
static const char *TAG = "canokey_usb";
static bool s_ctaphid_initialized;
static volatile uint8_t s_current_ctap_cbor_cmd;

typedef struct {
    bool active;
    uint16_t total_len;
    uint16_t received_len;
    uint8_t data[CANOKEY_CTAPHID_DEBUG_CBOR_BUFFER_SIZE];
} ctaphid_debug_cbor_tx_t;

static ctaphid_debug_cbor_tx_t s_debug_cbor_tx;
static ctaphid_debug_cbor_tx_t s_debug_cbor_rx;
static ctaphid_debug_cbor_tx_t s_make_credential_rx;
static uint8_t s_make_credential_cid[4];
static bool s_debug_client_data_hash_valid;
static uint8_t s_debug_client_data_hash_cmd;
static uint8_t s_debug_client_data_hash[32];

typedef enum {
    CANOKEY_CTAPHID_EVENT_OUT,
} canokey_ctaphid_event_type_t;

typedef struct {
    canokey_ctaphid_event_type_t type;
    uint8_t report[CANOKEY_CTAPHID_REPORT_SIZE];
    uint16_t len;
} canokey_ctaphid_event_t;

static QueueHandle_t s_ctaphid_event_queue;

static uint8_t ctaphid_cmd_code(uint8_t raw_cmd)
{
    return raw_cmd & 0x7F;
}

static const char *ctaphid_cmd_name(uint8_t cmd_code)
{
    switch (cmd_code) {
    case 0x01:
        return "CTAPHID_PING";
    case 0x03:
        return "CTAPHID_MSG";
    case 0x06:
        return "CTAPHID_INIT";
    case 0x08:
        return "CTAPHID_WINK";
    case 0x10:
        return "CTAPHID_CBOR";
    case 0x11:
        return "CTAPHID_CANCEL";
    case 0x3B:
        return "CTAPHID_KEEPALIVE";
    case 0x3F:
        return "CTAPHID_ERROR";
    default:
        return "CTAPHID_UNKNOWN";
    }
}

static const char *ctaphid_keepalive_status_name(uint8_t status)
{
    switch (status) {
    case 1:
        return "PROCESSING";
    case 2:
        return "UPNEEDED";
    default:
        return "UNKNOWN";
    }
}

static const char *ctap_cbor_cmd_name(uint8_t cmd)
{
    switch (cmd) {
    case 0x01:
        return "authenticatorMakeCredential";
    case 0x02:
        return "authenticatorGetAssertion";
    case 0x04:
        return "authenticatorGetInfo";
    case 0x06:
        return "authenticatorClientPIN";
    case 0x08:
        return "authenticatorGetNextAssertion";
    case 0x0A:
        return "authenticatorSelection";
    case 0x0B:
        return "authenticatorLargeBlobs";
    case 0x0C:
        return "authenticatorConfig";
    case 0x0D:
        return "authenticatorBioEnrollment";
    case 0x0E:
        return "authenticatorCredentialManagement";
    default:
        return "unknown";
    }
}

static const char *ctaphid_state_name(void)
{
    return s_ctaphid_state == CTAPHID_IDLE ? "IDLE" : "BUSY";
}

uint8_t canokey_esp32p4_get_current_ctap_cbor_cmd(void)
{
    return s_current_ctap_cbor_cmd;
}

static bool debug_cbor_read_ai(const uint8_t *buf, size_t len, size_t *offset, uint8_t expected_major, uint64_t *value)
{
    if (*offset >= len) {
        return false;
    }

    uint8_t initial = buf[(*offset)++];
    uint8_t major = initial >> 5;
    uint8_t ai = initial & 0x1F;
    if (major != expected_major) {
        return false;
    }

    if (ai < 24) {
        *value = ai;
        return true;
    }

    if (ai == 24) {
        if (*offset + 1 > len) {
            return false;
        }
        *value = buf[*offset];
        *offset += 1;
        return true;
    }

    if (ai == 25) {
        if (*offset + 2 > len) {
            return false;
        }
        *value = ((uint64_t)buf[*offset] << 8) | buf[*offset + 1];
        *offset += 2;
        return true;
    }

    if (ai == 26) {
        if (*offset + 4 > len) {
            return false;
        }
        *value = ((uint64_t)buf[*offset] << 24) |
                 ((uint64_t)buf[*offset + 1] << 16) |
                 ((uint64_t)buf[*offset + 2] << 8) |
                 buf[*offset + 3];
        *offset += 4;
        return true;
    }

    return false;
}

static bool debug_cbor_skip(const uint8_t *buf, size_t len, size_t *offset)
{
    if (*offset >= len) {
        return false;
    }

    uint8_t initial = buf[*offset];
    uint8_t major = initial >> 5;
    uint64_t value = 0;

    switch (major) {
    case 0:
    case 1:
        return debug_cbor_read_ai(buf, len, offset, major, &value);
    case 2:
    case 3:
        if (!debug_cbor_read_ai(buf, len, offset, major, &value) || value > len - *offset) {
            return false;
        }
        *offset += (size_t)value;
        return true;
    case 4:
        if (!debug_cbor_read_ai(buf, len, offset, major, &value)) {
            return false;
        }
        for (uint64_t i = 0; i < value; i++) {
            if (!debug_cbor_skip(buf, len, offset)) {
                return false;
            }
        }
        return true;
    case 5:
        if (!debug_cbor_read_ai(buf, len, offset, major, &value)) {
            return false;
        }
        for (uint64_t i = 0; i < value; i++) {
            if (!debug_cbor_skip(buf, len, offset) || !debug_cbor_skip(buf, len, offset)) {
                return false;
            }
        }
        return true;
    case 6:
        if (!debug_cbor_read_ai(buf, len, offset, major, &value)) {
            return false;
        }
        return debug_cbor_skip(buf, len, offset);
    case 7:
        (*offset)++;
        return true;
    default:
        return false;
    }
}

static bool debug_cbor_read_uint_key(const uint8_t *buf, size_t len, size_t *offset, uint64_t *key)
{
    return debug_cbor_read_ai(buf, len, offset, 0, key);
}

static bool debug_cbor_read_byte_string_header(const uint8_t *buf,
                                               size_t len,
                                               size_t *offset,
                                               const uint8_t **data,
                                               uint64_t *data_len)
{
    if (!debug_cbor_read_ai(buf, len, offset, 2, data_len) || *data_len > len - *offset) {
        return false;
    }

    *data = &buf[*offset];
    *offset += (size_t)*data_len;
    return true;
}

static bool debug_cbor_read_text_string_header(const uint8_t *buf,
                                               size_t len,
                                               size_t *offset,
                                               const uint8_t **data,
                                               uint64_t *data_len)
{
    if (!debug_cbor_read_ai(buf, len, offset, 3, data_len) || *data_len > len - *offset) {
        return false;
    }

    *data = &buf[*offset];
    *offset += (size_t)*data_len;
    return true;
}

static void debug_log_get_assertion_allow_list(const uint8_t *buf, size_t len, size_t offset)
{
    uint64_t item_count = 0;
    if (!debug_cbor_read_ai(buf, len, &offset, 4, &item_count)) {
        ESP_LOGW(TAG, "CTAP getAssertion allowList parse failed");
        return;
    }

    ESP_LOGW(TAG, "CTAP getAssertion allowList count=%llu", item_count);
    for (uint64_t i = 0; i < item_count; i++) {
        uint64_t map_items = 0;
        if (!debug_cbor_read_ai(buf, len, &offset, 5, &map_items)) {
            ESP_LOGW(TAG, "CTAP getAssertion allowList[%llu] descriptor parse failed", i);
            return;
        }

        ESP_LOGW(TAG, "CTAP getAssertion allowList[%llu] descriptor map_items=%llu", i, map_items);
        for (uint64_t j = 0; j < map_items; j++) {
            const uint8_t *name = NULL;
            uint64_t name_len = 0;
            if (!debug_cbor_read_text_string_header(buf, len, &offset, &name, &name_len)) {
                return;
            }

            if (name_len == 2 && memcmp(name, "id", 2) == 0) {
                const uint8_t *id = NULL;
                uint64_t id_len = 0;
                if (!debug_cbor_read_byte_string_header(buf, len, &offset, &id, &id_len)) {
                    return;
                }
                ESP_LOGW(TAG, "CTAP getAssertion allowList[%llu].id len=%llu", i, id_len);
                ESP_LOG_BUFFER_HEXDUMP(TAG, id, (uint16_t)id_len, ESP_LOG_WARN);
            } else if (name_len == 4 && memcmp(name, "type", 4) == 0) {
                const uint8_t *type = NULL;
                uint64_t type_len = 0;
                if (!debug_cbor_read_text_string_header(buf, len, &offset, &type, &type_len)) {
                    return;
                }
                ESP_LOGW(TAG, "CTAP getAssertion allowList[%llu].type=%.*s",
                         i,
                         (int)type_len,
                         (const char *)type);
            } else {
                if (!debug_cbor_skip(buf, len, &offset)) {
                    return;
                }
            }
        }
    }
}

static bool debug_cbor_read_bool(const uint8_t *buf, size_t len, size_t *offset, bool *value)
{
    if (*offset >= len) {
        return false;
    }

    uint8_t initial = buf[(*offset)++];
    if (initial == 0xF4) {
        *value = false;
        return true;
    }
    if (initial == 0xF5) {
        *value = true;
        return true;
    }
    return false;
}

static bool debug_cbor_read_int_with_location(uint8_t *buf,
                                              size_t len,
                                              size_t *offset,
                                              int64_t *value,
                                              size_t *value_offset)
{
    if (*offset >= len) {
        return false;
    }

    *value_offset = *offset;
    uint8_t initial = buf[*offset];
    uint8_t major = initial >> 5;
    uint64_t raw = 0;
    if (major != 0 && major != 1) {
        return false;
    }

    if (!debug_cbor_read_ai(buf, len, offset, major, &raw)) {
        return false;
    }

    *value = major == 0 ? (int64_t)raw : -1 - (int64_t)raw;
    return true;
}

static void patch_make_credential_pub_key_algs(uint8_t *buf, size_t len)
{
    if (len < 2 || buf[0] != 0x01) {
        return;
    }

    size_t offset = 1;
    uint64_t map_items = 0;
    if (!debug_cbor_read_ai(buf, len, &offset, 5, &map_items)) {
        return;
    }

    unsigned patched = 0;
    for (uint64_t i = 0; i < map_items; i++) {
        uint64_t key = 0;
        if (!debug_cbor_read_uint_key(buf, len, &offset, &key)) {
            return;
        }

        if (key != 4) {
            if (!debug_cbor_skip(buf, len, &offset)) {
                return;
            }
            continue;
        }

        uint64_t param_count = 0;
        if (!debug_cbor_read_ai(buf, len, &offset, 4, &param_count)) {
            return;
        }

        ESP_LOGW(TAG, "CTAP makeCredential pubKeyCredParams count=%llu", param_count);
        for (uint64_t p = 0; p < param_count; p++) {
            uint64_t param_map_items = 0;
            if (!debug_cbor_read_ai(buf, len, &offset, 5, &param_map_items)) {
                return;
            }

            for (uint64_t j = 0; j < param_map_items; j++) {
                const uint8_t *name = NULL;
                uint64_t name_len = 0;
                if (!debug_cbor_read_text_string_header(buf, len, &offset, &name, &name_len)) {
                    return;
                }

                if (name_len == 3 && memcmp(name, "alg", 3) == 0) {
                    int64_t alg = 0;
                    size_t alg_offset = 0;
                    if (!debug_cbor_read_int_with_location(buf, len, &offset, &alg, &alg_offset)) {
                        return;
                    }

                    ESP_LOGW(TAG, "CTAP makeCredential pubKeyCredParams[%llu].alg=%lld", p, alg);
                    if (alg == -8 && buf[alg_offset] == 0x27) {
                        buf[alg_offset] = 0x26;
                        patched++;
                        ESP_LOGW(TAG, "CTAP makeCredential force alg EdDSA(-8) -> ES256(-7) at offset=%u",
                                 (unsigned)alg_offset);
                    }
                } else {
                    if (!debug_cbor_skip(buf, len, &offset)) {
                        return;
                    }
                }
            }
        }
    }

    if (patched == 0) {
        ESP_LOGW(TAG, "CTAP makeCredential alg patch: no EdDSA(-8) entry changed");
    }
}

static void debug_log_ctap_options(const char *prefix, const uint8_t *buf, size_t len, size_t offset)
{
    uint64_t item_count = 0;
    if (!debug_cbor_read_ai(buf, len, &offset, 5, &item_count)) {
        ESP_LOGW(TAG, "%s options parse failed", prefix);
        return;
    }

    bool has_rk = false;
    bool has_uv = false;
    bool has_up = false;
    bool rk = false;
    bool uv = false;
    bool up = false;

    for (uint64_t i = 0; i < item_count; i++) {
        const uint8_t *name = NULL;
        uint64_t name_len = 0;
        if (!debug_cbor_read_text_string_header(buf, len, &offset, &name, &name_len)) {
            return;
        }

        bool value = false;
        if (name_len == 2 && memcmp(name, "rk", 2) == 0) {
            if (!debug_cbor_read_bool(buf, len, &offset, &value)) {
                return;
            }
            has_rk = true;
            rk = value;
        } else if (name_len == 2 && memcmp(name, "uv", 2) == 0) {
            if (!debug_cbor_read_bool(buf, len, &offset, &value)) {
                return;
            }
            has_uv = true;
            uv = value;
        } else if (name_len == 2 && memcmp(name, "up", 2) == 0) {
            if (!debug_cbor_read_bool(buf, len, &offset, &value)) {
                return;
            }
            has_up = true;
            up = value;
        } else {
            if (!debug_cbor_skip(buf, len, &offset)) {
                return;
            }
        }
    }

    ESP_LOGW(TAG,
             "%s options rk=%s%s uv=%s%s up=%s%s",
             prefix,
             has_rk ? "" : "absent/",
             has_rk ? (rk ? "true" : "false") : "default",
             has_uv ? "" : "absent/",
             has_uv ? (uv ? "true" : "false") : "default",
             has_up ? "" : "absent/",
             has_up ? (up ? "true" : "false") : "default");
}

static void debug_log_auth_data_flags(const uint8_t *auth_data, uint64_t auth_data_len, uint8_t status, uint64_t key)
{
    if (auth_data_len < 33) {
        ESP_LOGW(TAG, "CTAP response authData too short key=%llu status=0x%02x len=%llu",
                 key,
                 status,
                 auth_data_len);
        return;
    }

    uint8_t flags = auth_data[32];
    ESP_LOGW(TAG,
             "CTAP response authData key=%llu status=0x%02x flags=0x%02x UP=%u UV=%u AT=%u ED=%u authDataLen=%llu",
             key,
             status,
             flags,
             (flags & 0x01) ? 1 : 0,
             (flags & 0x04) ? 1 : 0,
             (flags & 0x40) ? 1 : 0,
             (flags & 0x80) ? 1 : 0,
             auth_data_len);

    if (status == 0x00 && s_debug_client_data_hash_valid && auth_data_len + sizeof(s_debug_client_data_hash) <= 512) {
        uint8_t signed_message[512];
        memcpy(signed_message, auth_data, (size_t)auth_data_len);
        memcpy(signed_message + auth_data_len, s_debug_client_data_hash, sizeof(s_debug_client_data_hash));
        ESP_LOGW(TAG,
                 "CTAP signed memcpy arg1=data_buf+len offset=%llu authDataLen=%llu",
                 auth_data_len,
                 auth_data_len);
        ESP_LOG_BUFFER_HEXDUMP(TAG, auth_data, (uint16_t)auth_data_len, ESP_LOG_WARN);
        ESP_LOGW(TAG,
                 "CTAP signed memcpy arg2=ga_state.client_data_hash len=%u",
                 (unsigned)sizeof(s_debug_client_data_hash));
        ESP_LOG_BUFFER_HEXDUMP(TAG, s_debug_client_data_hash, sizeof(s_debug_client_data_hash), ESP_LOG_WARN);
        ESP_LOGW(TAG,
                 "CTAP signed memcpy arg3=CLIENT_DATA_HASH_SIZE value=%u",
                 (unsigned)sizeof(s_debug_client_data_hash));
        ESP_LOGW(TAG,
                 "CTAP signed message authData||clientDataHash cmd=0x%02x authDataLen=%llu totalLen=%u",
                 s_debug_client_data_hash_cmd,
                 auth_data_len,
                 (unsigned)(auth_data_len + sizeof(s_debug_client_data_hash)));
        ESP_LOG_BUFFER_HEXDUMP(TAG,
                               signed_message,
                               (uint16_t)(auth_data_len + sizeof(s_debug_client_data_hash)),
                               ESP_LOG_WARN);
    }
}

static void debug_log_response_credential_alg(const uint8_t *credential, uint64_t credential_len, uint8_t status)
{
    size_t offset = 0;
    uint64_t map_items = 0;
    if (!debug_cbor_read_ai(credential, (size_t)credential_len, &offset, 5, &map_items)) {
        return;
    }

    for (uint64_t i = 0; i < map_items; i++) {
        const uint8_t *name = NULL;
        uint64_t name_len = 0;
        if (!debug_cbor_read_text_string_header(credential, (size_t)credential_len, &offset, &name, &name_len)) {
            return;
        }

        if (name_len == 2 && memcmp(name, "id", 2) == 0) {
            const uint8_t *id = NULL;
            uint64_t id_len = 0;
            if (!debug_cbor_read_byte_string_header(credential, (size_t)credential_len, &offset, &id, &id_len)) {
                return;
            }

            if (id_len >= 70) {
                int32_t alg_type = (int32_t)((uint32_t)id[66] |
                                             ((uint32_t)id[67] << 8) |
                                             ((uint32_t)id[68] << 16) |
                                             ((uint32_t)id[69] << 24));
                ESP_LOGW(TAG,
                         "CTAP response credential alg_type=%ld status=0x%02x idLen=%llu",
                         (long)alg_type,
                         status,
                         id_len);
            } else {
                ESP_LOGW(TAG,
                         "CTAP response credential id too short for alg_type status=0x%02x idLen=%llu",
                         status,
                         id_len);
            }
        } else {
            if (!debug_cbor_skip(credential, (size_t)credential_len, &offset)) {
                return;
            }
        }
    }
}

static void debug_parse_ctap_cbor_response(const uint8_t *buf, size_t len)
{
    if (len < 2) {
        return;
    }

    uint8_t status = buf[0];
    ESP_LOGW(TAG, "CTAP response full payload status=0x%02x len=%u", status, (unsigned)len);
    ESP_LOG_BUFFER_HEXDUMP(TAG, buf, (uint16_t)len, ESP_LOG_WARN);

    size_t offset = 1;
    uint64_t map_items = 0;
    if (!debug_cbor_read_ai(buf, len, &offset, 5, &map_items)) {
        ESP_LOGW(TAG, "CTAP response parse skipped: status=0x%02x first_cbor=0x%02x len=%u",
                 status,
                 buf[1],
                 (unsigned)len);
        return;
    }

    for (uint64_t i = 0; i < map_items; i++) {
        uint64_t key = 0;
        if (!debug_cbor_read_uint_key(buf, len, &offset, &key)) {
            return;
        }

        if (key == 1 || key == 2) {
            size_t value_offset = offset;
            const uint8_t *auth_data = NULL;
            uint64_t auth_data_len = 0;
            if (debug_cbor_read_byte_string_header(buf, len, &value_offset, &auth_data, &auth_data_len)) {
                debug_log_auth_data_flags(auth_data, auth_data_len, status, key);
            }
        }

        if (key == 1) {
            size_t value_offset = offset;
            uint64_t credential_map_items = 0;
            if (debug_cbor_read_ai(buf, len, &value_offset, 5, &credential_map_items)) {
                debug_log_response_credential_alg(&buf[offset], value_offset > offset ? len - offset : len - offset, status);
            }
        }

        if (key == 3) {
            size_t value_offset = offset;
            const uint8_t *signature = NULL;
            uint64_t signature_len = 0;
            if (debug_cbor_read_byte_string_header(buf, len, &value_offset, &signature, &signature_len)) {
                ESP_LOGW(TAG,
                         "CTAP response signature key=3 status=0x%02x sigLen=%llu",
                         status,
                         signature_len);
                ESP_LOG_BUFFER_HEXDUMP(TAG, signature, (uint16_t)signature_len, ESP_LOG_WARN);
            }
        }

        if (!debug_cbor_skip(buf, len, &offset)) {
            return;
        }
    }
}

static void debug_parse_ctap_cbor_request(const uint8_t *buf, size_t len)
{
    if (len < 2) {
        return;
    }

    uint8_t cbor_cmd = buf[0];
    uint64_t client_data_hash_key = 0;
    switch (cbor_cmd) {
    case 0x01:
        client_data_hash_key = 1;
        break;
    case 0x02:
        client_data_hash_key = 2;
        break;
    default:
        return;
    }

    size_t offset = 1;
    uint64_t map_items = 0;
    if (!debug_cbor_read_ai(buf, len, &offset, 5, &map_items)) {
        return;
    }

    for (uint64_t i = 0; i < map_items; i++) {
        uint64_t key = 0;
        if (!debug_cbor_read_uint_key(buf, len, &offset, &key)) {
            return;
        }

        if (cbor_cmd == 0x02 && key == 1) {
            size_t value_offset = offset;
            const uint8_t *rp_id = NULL;
            uint64_t rp_id_len = 0;
            if (debug_cbor_read_text_string_header(buf, len, &value_offset, &rp_id, &rp_id_len)) {
                ESP_LOGW(TAG, "CTAP getAssertion rpId=%.*s len=%llu",
                         (int)rp_id_len,
                         (const char *)rp_id,
                         rp_id_len);
            }
        }

        if (key == client_data_hash_key) {
            size_t value_offset = offset;
            const uint8_t *hash = NULL;
            uint64_t hash_len = 0;
            if (debug_cbor_read_byte_string_header(buf, len, &value_offset, &hash, &hash_len) && hash_len == 32) {
                memcpy(s_debug_client_data_hash, hash, sizeof(s_debug_client_data_hash));
                s_debug_client_data_hash_cmd = cbor_cmd;
                s_debug_client_data_hash_valid = true;
                ESP_LOGW(TAG,
                         "CTAP request clientDataHash captured cmd=0x%02x(%s)",
                         cbor_cmd,
                         ctap_cbor_cmd_name(cbor_cmd));
                ESP_LOG_BUFFER_HEXDUMP(TAG, s_debug_client_data_hash, sizeof(s_debug_client_data_hash), ESP_LOG_WARN);
            }
        }

        if (cbor_cmd == 0x02 && key == 3) {
            debug_log_get_assertion_allow_list(buf, len, offset);
        }

        if (cbor_cmd == 0x01 && key == 7) {
            debug_log_ctap_options("CTAP makeCredential", buf, len, offset);
        }

        if (cbor_cmd == 0x02 && key == 5) {
            debug_log_ctap_options("CTAP getAssertion", buf, len, offset);
        }

        if (!debug_cbor_skip(buf, len, &offset)) {
            return;
        }
    }
}

static void debug_capture_ctaphid_cbor_rx(const uint8_t *report)
{
    uint8_t raw_cmd = report[4];
    if ((raw_cmd & 0x80) != 0) {
        uint8_t cmd = ctaphid_cmd_code(raw_cmd);
        if (cmd != 0x10) {
            s_debug_cbor_rx.active = false;
            return;
        }

        uint16_t total_len = (uint16_t)((report[5] << 8) | report[6]);
        s_debug_cbor_rx.active = true;
        s_debug_cbor_rx.total_len = total_len;
        s_debug_cbor_rx.received_len = 0;
        s_debug_client_data_hash_valid = false;

        uint16_t copy_len = total_len < (CANOKEY_CTAPHID_REPORT_SIZE - 7) ?
                            total_len :
                            (CANOKEY_CTAPHID_REPORT_SIZE - 7);
        if (copy_len > CANOKEY_CTAPHID_DEBUG_CBOR_BUFFER_SIZE) {
            copy_len = CANOKEY_CTAPHID_DEBUG_CBOR_BUFFER_SIZE;
        }
        memcpy(s_debug_cbor_rx.data, report + 7, copy_len);
        s_debug_cbor_rx.received_len = copy_len;
    } else if (s_debug_cbor_rx.active) {
        uint16_t remaining = s_debug_cbor_rx.total_len - s_debug_cbor_rx.received_len;
        uint16_t copy_len = remaining < (CANOKEY_CTAPHID_REPORT_SIZE - 5) ?
                            remaining :
                            (CANOKEY_CTAPHID_REPORT_SIZE - 5);
        if (s_debug_cbor_rx.received_len + copy_len > CANOKEY_CTAPHID_DEBUG_CBOR_BUFFER_SIZE) {
            copy_len = CANOKEY_CTAPHID_DEBUG_CBOR_BUFFER_SIZE - s_debug_cbor_rx.received_len;
        }
        memcpy(s_debug_cbor_rx.data + s_debug_cbor_rx.received_len, report + 5, copy_len);
        s_debug_cbor_rx.received_len += copy_len;
    }

    if (s_debug_cbor_rx.active && s_debug_cbor_rx.received_len >= s_debug_cbor_rx.total_len) {
        debug_parse_ctap_cbor_request(s_debug_cbor_rx.data, s_debug_cbor_rx.total_len);
        s_debug_cbor_rx.active = false;
    }
}

static void debug_capture_ctaphid_cbor_tx(const uint8_t *report)
{
    uint8_t raw_cmd = report[4];
    if ((raw_cmd & 0x80) != 0) {
        uint8_t cmd = ctaphid_cmd_code(raw_cmd);
        if (cmd != 0x10) {
            s_debug_cbor_tx.active = false;
            return;
        }

        uint16_t total_len = (uint16_t)((report[5] << 8) | report[6]);
        s_debug_cbor_tx.active = true;
        s_debug_cbor_tx.total_len = total_len;
        s_debug_cbor_tx.received_len = 0;

        uint16_t copy_len = total_len < (CANOKEY_CTAPHID_REPORT_SIZE - 7) ?
                            total_len :
                            (CANOKEY_CTAPHID_REPORT_SIZE - 7);
        if (copy_len > CANOKEY_CTAPHID_DEBUG_CBOR_BUFFER_SIZE) {
            copy_len = CANOKEY_CTAPHID_DEBUG_CBOR_BUFFER_SIZE;
        }
        memcpy(s_debug_cbor_tx.data, report + 7, copy_len);
        s_debug_cbor_tx.received_len = copy_len;
    } else if (s_debug_cbor_tx.active) {
        uint16_t remaining = s_debug_cbor_tx.total_len - s_debug_cbor_tx.received_len;
        uint16_t copy_len = remaining < (CANOKEY_CTAPHID_REPORT_SIZE - 5) ?
                            remaining :
                            (CANOKEY_CTAPHID_REPORT_SIZE - 5);
        if (s_debug_cbor_tx.received_len + copy_len > CANOKEY_CTAPHID_DEBUG_CBOR_BUFFER_SIZE) {
            copy_len = CANOKEY_CTAPHID_DEBUG_CBOR_BUFFER_SIZE - s_debug_cbor_tx.received_len;
        }
        memcpy(s_debug_cbor_tx.data + s_debug_cbor_tx.received_len, report + 5, copy_len);
        s_debug_cbor_tx.received_len += copy_len;
    }

    if (s_debug_cbor_tx.active && s_debug_cbor_tx.received_len >= s_debug_cbor_tx.total_len) {
        debug_parse_ctap_cbor_response(s_debug_cbor_tx.data, s_debug_cbor_tx.total_len);
        s_debug_cbor_tx.active = false;
    }
}

void canokey_esp32p4_ctaphid_usb_init(void)
{
    ESP_LOGI(TAG, "CTAPHID init start: itf=%d report_size=%d queue_depth=%d",
             CANOKEY_CTAPHID_TINYUSB_ITF,
             CANOKEY_CTAPHID_REPORT_SIZE,
             CANOKEY_CTAPHID_EVENT_QUEUE_DEPTH);

    if (!s_ctaphid_event_queue) {
        s_ctaphid_event_queue = xQueueCreate(CANOKEY_CTAPHID_EVENT_QUEUE_DEPTH, sizeof(canokey_ctaphid_event_t));
        if (!s_ctaphid_event_queue) {
            ESP_LOGE(TAG, "Failed to create CTAPHID event queue");
            s_ctaphid_initialized = false;
            return;
        }
        ESP_LOGI(TAG, "CTAPHID event queue created");
    } else {
        xQueueReset(s_ctaphid_event_queue);
        ESP_LOGI(TAG, "CTAPHID event queue reset");
    }
    s_ctaphid_state = CTAPHID_IDLE;
    CTAPHID_TxReset();
    CTAPHID_Init(USBD_CTAPHID_SendReport);
    s_ctaphid_initialized = true;
    ESP_LOGI(TAG, "CTAPHID init done: state=%s mounted=%u ready=%u",
             ctaphid_state_name(),
             tud_mounted() ? 1 : 0,
             tud_hid_n_ready(CANOKEY_CTAPHID_TINYUSB_ITF) ? 1 : 0);
}

uint8_t USBD_CTAPHID_WaitIdle(void)
{
    if (s_ctaphid_state != CTAPHID_IDLE) {
        ESP_LOGD(TAG, "CTAPHID wait idle: current=%s", ctaphid_state_name());
    }

    for (int i = 0; i < CANOKEY_CTAPHID_IN_TIMEOUT_MS; i++) {
        if (s_ctaphid_state == CTAPHID_IDLE) {
            if (i > 0) {
                ESP_LOGD(TAG, "CTAPHID wait idle done after %d ms", i);
            }
            return USBD_OK;
        }
        device_delay(1);
    }

    ESP_LOGW(TAG, "CTAPHID wait idle timeout: state=%s", ctaphid_state_name());
    return s_ctaphid_state == CTAPHID_IDLE ? USBD_OK : USBD_BUSY;
}

static uint8_t USBD_CTAPHID_WaitReady(void)
{
    for (int i = 0; i < CANOKEY_CTAPHID_IN_TIMEOUT_MS; i++) {
        if (!tud_mounted()) {
            ESP_LOGW(TAG, "CTAPHID wait ready failed: USB not mounted");
            return USBD_FAIL;
        }

        if (s_ctaphid_state == CTAPHID_IDLE && tud_hid_n_ready(CANOKEY_CTAPHID_TINYUSB_ITF)) {
            if (i > 0) {
                ESP_LOGD(TAG, "CTAPHID wait ready done after %d ms", i);
            }
            return USBD_OK;
        }

        device_delay(1);
    }

    ESP_LOGW(TAG,
             "CTAPHID wait ready timeout: state=%s mounted=%u ready=%u",
             ctaphid_state_name(),
             tud_mounted() ? 1 : 0,
             tud_hid_n_ready(CANOKEY_CTAPHID_TINYUSB_ITF) ? 1 : 0);
    return USBD_BUSY;
}

uint8_t USBD_CTAPHID_IsIdle(void)
{
    return s_ctaphid_state == CTAPHID_IDLE ? USBD_OK : USBD_BUSY;
}

void USBD_CTAPHID_ServiceReceive(void)
{
    if (!s_ctaphid_initialized || !s_ctaphid_event_queue) {
        ESP_LOGD(TAG, "CTAPHID service skipped: initialized=%u queue=%u",
                 s_ctaphid_initialized ? 1 : 0,
                 s_ctaphid_event_queue ? 1 : 0);
        return;
    }

    canokey_ctaphid_event_t event;
    while (xQueueReceive(s_ctaphid_event_queue, &event, 0) == pdTRUE) {
        switch (event.type) {
        case CANOKEY_CTAPHID_EVENT_OUT:
            if (canokey_nfc_is_active()) {
                /* Report busy for new USB commands; ignore their continuation
                 * frames so they cannot overwrite the NFC request buffers. */
                if (event.report[4] & TYPE_INIT) {
                    uint32_t cid;
                    memcpy(&cid, event.report, sizeof(cid));
                    const uint8_t error = ERR_CHANNEL_BUSY;
                    CTAPHID_SendResponseAuto(cid, CTAPHID_ERROR, &error, sizeof(error));
                }
                break;
            }
            {
                uint8_t raw_cmd = event.report[4];
                if ((raw_cmd & 0x80) != 0) {
                    uint8_t cmd = ctaphid_cmd_code(raw_cmd);
                    uint16_t msg_len = (uint16_t)((event.report[5] << 8) | event.report[6]);
                    ESP_LOGI(TAG, "CTAPHID RX dequeue cmd=0x%02x(%s) raw=0x%02x len=%u state=%s",
                             cmd,
                             ctaphid_cmd_name(cmd),
                             raw_cmd,
                             msg_len,
                             ctaphid_state_name());
                } else {
                    ESP_LOGI(TAG, "CTAPHID RX dequeue cont seq=0x%02x state=%s",
                             raw_cmd,
                             ctaphid_state_name());
                }
            }
            CTAPHID_OutEvent(event.report);
            break;
        default:
            ESP_LOGW(TAG, "CTAPHID unknown event type=%d", event.type);
            break;
        }
    }
}

uint8_t USBD_CTAPHID_DataIn(void)
{
    return canokey_esp32p4_ctaphid_queue_data_in();
}

uint8_t canokey_esp32p4_ctaphid_queue_data_in(void)
{
    s_ctaphid_state = CTAPHID_IDLE;
    ESP_LOGI(TAG, "CTAPHID IN complete: state=%s", ctaphid_state_name());
    return USBD_OK;
}

static uint8_t canokey_esp32p4_ctaphid_enqueue_out_report(const uint8_t *report)
{
    canokey_ctaphid_event_t event = {
        .type = CANOKEY_CTAPHID_EVENT_OUT,
        .len = CANOKEY_CTAPHID_REPORT_SIZE,
    };
    memcpy(event.report, report, CANOKEY_CTAPHID_REPORT_SIZE);
    if (xQueueSend(s_ctaphid_event_queue, &event, 0) != pdTRUE) {
        ESP_LOGW(TAG, "CTAPHID event queue full on OUT");
        return USBD_BUSY;
    }
    return USBD_OK;
}

static uint8_t enqueue_patched_make_credential_reports(void)
{
    uint8_t report[CANOKEY_CTAPHID_REPORT_SIZE] = {0};
    uint16_t remaining = s_make_credential_rx.total_len;
    uint16_t offset = 0;
    uint16_t cont_len = s_make_credential_rx.total_len > (CANOKEY_CTAPHID_REPORT_SIZE - 7) ?
                        s_make_credential_rx.total_len - (CANOKEY_CTAPHID_REPORT_SIZE - 7) :
                        0;
    uint16_t report_count = 1 + (cont_len + (CANOKEY_CTAPHID_REPORT_SIZE - 6)) / (CANOKEY_CTAPHID_REPORT_SIZE - 5);

    ESP_LOGW(TAG,
             "CTAP makeCredential patched request enqueue reports=%u len=%u",
             (unsigned)report_count,
             (unsigned)s_make_credential_rx.total_len);

    memcpy(report, s_make_credential_cid, sizeof(s_make_credential_cid));
    report[4] = 0x90;
    report[5] = (uint8_t)(s_make_credential_rx.total_len >> 8);
    report[6] = (uint8_t)(s_make_credential_rx.total_len & 0xFF);
    uint16_t copy_len = remaining < (CANOKEY_CTAPHID_REPORT_SIZE - 7) ?
                        remaining :
                        (CANOKEY_CTAPHID_REPORT_SIZE - 7);
    memcpy(report + 7, s_make_credential_rx.data, copy_len);
    uint8_t status = canokey_esp32p4_ctaphid_enqueue_out_report(report);
    if (status != USBD_OK) {
        return status;
    }

    remaining -= copy_len;
    offset += copy_len;
    uint8_t seq = 0;
    while (remaining > 0) {
        memset(report, 0, sizeof(report));
        memcpy(report, s_make_credential_cid, sizeof(s_make_credential_cid));
        report[4] = seq++;
        copy_len = remaining < (CANOKEY_CTAPHID_REPORT_SIZE - 5) ?
                   remaining :
                   (CANOKEY_CTAPHID_REPORT_SIZE - 5);
        memcpy(report + 5, s_make_credential_rx.data + offset, copy_len);
        status = canokey_esp32p4_ctaphid_enqueue_out_report(report);
        if (status != USBD_OK) {
            return status;
        }
        remaining -= copy_len;
        offset += copy_len;
    }

    ESP_LOGW(TAG,
             "CTAP makeCredential patched request enqueued len=%u",
             (unsigned)s_make_credential_rx.total_len);
    return USBD_OK;
}

static bool capture_make_credential_for_patch(const uint8_t *report, uint8_t *status)
{
    uint8_t raw_cmd = report[4];
    if ((raw_cmd & 0x80) != 0) {
        uint8_t cmd = ctaphid_cmd_code(raw_cmd);
        uint16_t total_len = (uint16_t)((report[5] << 8) | report[6]);
        if (cmd != 0x10 || total_len == 0 || report[7] != 0x01) {
            s_make_credential_rx.active = false;
            return false;
        }

        if (total_len > CANOKEY_CTAPHID_DEBUG_CBOR_BUFFER_SIZE) {
            ESP_LOGW(TAG,
                     "CTAP makeCredential patch skipped: request too large len=%u",
                     (unsigned)total_len);
            s_make_credential_rx.active = false;
            return false;
        }

        memcpy(s_make_credential_cid, report, sizeof(s_make_credential_cid));
        s_make_credential_rx.active = true;
        s_make_credential_rx.total_len = total_len;
        s_make_credential_rx.received_len = 0;

        uint16_t copy_len = total_len < (CANOKEY_CTAPHID_REPORT_SIZE - 7) ?
                            total_len :
                            (CANOKEY_CTAPHID_REPORT_SIZE - 7);
        memcpy(s_make_credential_rx.data, report + 7, copy_len);
        s_make_credential_rx.received_len = copy_len;
    } else if (s_make_credential_rx.active) {
        uint16_t remaining = s_make_credential_rx.total_len - s_make_credential_rx.received_len;
        uint16_t copy_len = remaining < (CANOKEY_CTAPHID_REPORT_SIZE - 5) ?
                            remaining :
                            (CANOKEY_CTAPHID_REPORT_SIZE - 5);
        memcpy(s_make_credential_rx.data + s_make_credential_rx.received_len, report + 5, copy_len);
        s_make_credential_rx.received_len += copy_len;
    } else {
        return false;
    }

    if (s_make_credential_rx.received_len < s_make_credential_rx.total_len) {
        *status = USBD_OK;
        return true;
    }

    patch_make_credential_pub_key_algs(s_make_credential_rx.data, s_make_credential_rx.total_len);
    *status = enqueue_patched_make_credential_reports();
    s_make_credential_rx.active = false;
    return true;
}

uint8_t canokey_esp32p4_ctaphid_queue_out_report(const uint8_t *report, uint16_t len)
{
    if (!s_ctaphid_initialized || !s_ctaphid_event_queue || !report || len < CANOKEY_CTAPHID_REPORT_SIZE) {
        ESP_LOGW(TAG, "CTAPHID OUT reject initialized=%u queue=%u report=%u len=%u",
                 s_ctaphid_initialized ? 1 : 0,
                 s_ctaphid_event_queue ? 1 : 0,
                 report ? 1 : 0,
                 len);
        return USBD_FAIL;
    }

    uint8_t raw_cmd = report[4];
    if ((raw_cmd & 0x80) != 0) {
        uint8_t cmd = ctaphid_cmd_code(raw_cmd);
        uint16_t msg_len = (uint16_t)((report[5] << 8) | report[6]);
        ESP_LOGI(TAG, "CTAPHID RX cmd=0x%02x(%s) raw=0x%02x len=%u mounted=%u ready=%u",
                 cmd,
                 ctaphid_cmd_name(cmd),
                 raw_cmd,
                 msg_len,
                 tud_mounted() ? 1 : 0,
                 tud_hid_n_ready(CANOKEY_CTAPHID_TINYUSB_ITF) ? 1 : 0);
        if (cmd == 0x10 && msg_len > 0) {
            uint8_t cbor_cmd = report[7];
            s_current_ctap_cbor_cmd = cbor_cmd;
            ESP_LOGW(TAG, "CTAPHID RX CBOR cmd=0x%02x(%s) payload_len=%u cid=%02x%02x%02x%02x",
                     cbor_cmd,
                     ctap_cbor_cmd_name(cbor_cmd),
                     msg_len,
                     report[0],
                     report[1],
                     report[2],
                     report[3]);
        }
        if (cmd == 0x06 || cmd == 0x10) {
            ESP_LOG_BUFFER_HEXDUMP(TAG, report, CANOKEY_CTAPHID_REPORT_SIZE, ESP_LOG_INFO);
        }
    } else {
        ESP_LOGI(TAG, "CTAPHID RX cont seq=0x%02x", raw_cmd);
    }

    debug_capture_ctaphid_cbor_rx(report);

    uint8_t patch_status = USBD_OK;
    if (capture_make_credential_for_patch(report, &patch_status)) {
        return patch_status;
    }

    return canokey_esp32p4_ctaphid_enqueue_out_report(report);
}

uint8_t USBD_CTAPHID_SendReport(USBD_HandleTypeDef *pdev, uint8_t *report, uint16_t len)
{
    (void)pdev;

    ESP_LOGI(TAG, "CTAPHID IN send request len=%u mounted=%u ready=%u state=%s",
             len,
             tud_mounted() ? 1 : 0,
             tud_hid_n_ready(CANOKEY_CTAPHID_TINYUSB_ITF) ? 1 : 0,
             ctaphid_state_name());

    if (!s_ctaphid_initialized) {
        ESP_LOGW(TAG, "CTAPHID IN drop: not initialized");
        return USBD_FAIL;
    }

    if (len != CANOKEY_CTAPHID_REPORT_SIZE) {
        ESP_LOGW(TAG, "CTAPHID IN drop invalid len=%u", len);
        return USBD_FAIL;
    }

    if (USBD_CTAPHID_WaitReady() != USBD_OK) {
        ESP_LOGW(TAG, "CTAPHID IN busy waiting ready");
        return USBD_BUSY;
    }

    uint8_t raw_cmd = report[4];
    if ((raw_cmd & 0x80) != 0) {
        uint8_t cmd = ctaphid_cmd_code(raw_cmd);
        uint16_t msg_len = (uint16_t)((report[5] << 8) | report[6]);
        ESP_LOGI(TAG, "CTAPHID IN cmd=0x%02x(%s) raw=0x%02x len=%u",
                 cmd,
                 ctaphid_cmd_name(cmd),
                 raw_cmd,
                 msg_len);
        if (cmd == 0x3B && msg_len > 0) {
            uint8_t status = report[7];
            ESP_LOGW(TAG, "CTAPHID KEEPALIVE status=%u(%s) cid=%02x%02x%02x%02x",
                     status,
                     ctaphid_keepalive_status_name(status),
                     report[0],
                     report[1],
                     report[2],
                     report[3]);
        }
        if (cmd == 0x06) {
            uint16_t dump_len =
                msg_len < (CANOKEY_CTAPHID_REPORT_SIZE - 7) ? msg_len : (CANOKEY_CTAPHID_REPORT_SIZE - 7);
            ESP_LOG_BUFFER_HEXDUMP(TAG, report + 7, dump_len, ESP_LOG_INFO);
        }
        if (cmd == 0x10) {
            ESP_LOGI(TAG, "CTAPHID IN CTAPHID_CBOR full report");
            ESP_LOG_BUFFER_HEXDUMP(TAG, report, len, ESP_LOG_INFO);
        }
    } else {
        ESP_LOGI(TAG, "CTAPHID IN cont seq=0x%02x", raw_cmd);
    }

    debug_capture_ctaphid_cbor_tx(report);

    s_ctaphid_state = CTAPHID_BUSY;
    if (!tud_hid_n_report(CANOKEY_CTAPHID_TINYUSB_ITF, 0, report, len)) {
        s_ctaphid_state = CTAPHID_IDLE;
        ESP_LOGW(TAG, "CTAPHID IN TinyUSB submit failed raw=0x%02x", raw_cmd);
        return USBD_BUSY;
    }

    ESP_LOGI(TAG, "CTAPHID IN submitted raw=0x%02x", raw_cmd);
    return USBD_OK;
}

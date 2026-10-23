#include "canokey_nfc.h"

#include <stdatomic.h>
#include <string.h>

#include "apdu.h"
#include "canokey_esp32p4.h"
#include "canokey_port_internal.h"
#include "ctaphid.h"
#include "device.h"
#include "esp_log.h"
#include "pke.h"
#include "usbd_ctaphid.h"

#define NFC_EVENT_HEADER_SIZE 9
#define NFC_IDS_SIZE 8
#define NFC_MAX_CAPDU_SIZE (APDU_INCOMING_DATA_SIZE + 9)
#define NFC_MAX_RAPDU_SIZE (4096 - NFC_IDS_SIZE)
#define NFC_UART_TIMEOUT_MS 300
#define NFC_RETRY_MS 5000
#define NFC_MODE_RETRY_MS 500
#define NFC_DISABLE_RETRY_MAX 3
#define NFC_LINK_TIMEOUT_MS 2000
#define NFC_POLL_IDLE_MS 100
#define NFC_POLL_ACTIVE_MS 20
#define NFC_POLL_ERROR_MIN_MS 500
#define NFC_POLL_ERROR_MAX_MS 5000
#define NFC_PRESENCE_TOKEN_MASK 0x3FFFFFFFu
#define NFC_CTAP_GET_ASSERTION 0x02
#define NFC_CTAP_GET_INFO 0x04

#ifndef CANOKEY_NFC_POLL_SCHEDULING
#define CANOKEY_NFC_POLL_SCHEDULING 1
#endif

/* Core owners are integer tokens. Reserve unused values locally so CCID and
 * NFC never share a session, without changing the vendored core's enums. */
#define NFC_SESSION_OWNER ((device_applet_session_owner_t)(DEVICE_APPLET_SESSION_WEBUSB + 1))
#define NFC_BUFFER_OWNER (BUFFER_OWNER_CTAPHID + 1)
#define NFC_PKE_PROBE_OWNER (PKE_BUFFER_OWNER_CTAP + 1)

static const char *TAG = "canokey_nfc";
static const uint8_t s_fido_aid[] = {0xA0, 0x00, 0x00, 0x06, 0x47, 0x2F, 0x00, 0x01};
static const uint8_t s_ndef_aid[] = {0xD2, 0x76, 0x00, 0x00, 0x85, 0x01, 0x01};

typedef enum {
    NFC_APPLET_NONE,
    NFC_APPLET_NDEF,
    NFC_APPLET_FIDO,
} nfc_applet_t;
static canokey_nfc_transceive_t s_transceive;
static atomic_bool s_enabled;
static atomic_bool s_active;
static atomic_bool s_presence_pending;
static atomic_uchar s_cbor_cmd;
static atomic_uint_least32_t s_presence_token;
static atomic_uint_least32_t s_presence_reply;
static atomic_uchar s_disable_retry_count;
static bool s_configured;
static bool s_disabled;
static bool s_remote_disabled;
static bool s_config_attempted;
static nfc_applet_t s_applet;
static bool s_poll_pending;
static bool s_command_chaining;
static uint32_t s_session_id;
static uint32_t s_last_link_tick;
static uint32_t s_config_tick;
static uint32_t s_ack_session;
static uint32_t s_ack_event;
static uint32_t s_cached_event;
static uint32_t s_cached_tick;
static uint32_t s_next_poll_tick;
static uint32_t s_poll_error_delay;
static uint16_t s_cached_command_len;
static uint16_t s_cached_response_len;
static uint8_t s_cached_cbor_cmd;
static uint8_t s_cached_command[NFC_MAX_CAPDU_SIZE];
static uint8_t s_cached_response[NFC_IDS_SIZE + NFC_MAX_RAPDU_SIZE];

static uint32_t read_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static void write_be32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)(value >> 24);
    p[1] = (uint8_t)(value >> 16);
    p[2] = (uint8_t)(value >> 8);
    p[3] = (uint8_t)value;
}

static void format_log_bytes(const uint8_t *data, uint16_t length, char *hex,
                             uint16_t hex_capacity, bool *truncated)
{
    static const char digits[] = "0123456789abcdef";
    uint16_t shown = length;
    const uint16_t max_bytes = (hex_capacity - 1U) / 2U;
    if (shown > max_bytes) shown = max_bytes;
    for (uint16_t i = 0; i < shown; i++) {
        hex[i * 2U] = digits[data[i] >> 4];
        hex[i * 2U + 1U] = digits[data[i] & 0x0F];
    }
    hex[shown * 2U] = '\0';
    *truncated = shown != length;
}

static bool poll_is_due(uint32_t now)
{
#if CANOKEY_NFC_POLL_SCHEDULING
    return (int32_t)(now - s_next_poll_tick) >= 0;
#else
    (void)now;
    return true;
#endif
}

static void schedule_poll_success(uint32_t now)
{
    s_poll_error_delay = 0;
    s_next_poll_tick = now + (canokey_nfc_is_active() ? NFC_POLL_ACTIVE_MS : NFC_POLL_IDLE_MS);
}

static void schedule_poll_error(uint32_t now)
{
    if (s_poll_error_delay < NFC_POLL_ERROR_MIN_MS) {
        s_poll_error_delay = NFC_POLL_ERROR_MIN_MS;
    } else if (!canokey_nfc_is_active() && s_poll_error_delay < NFC_POLL_ERROR_MAX_MS) {
        uint32_t doubled = s_poll_error_delay * 2U;
        s_poll_error_delay = doubled > NFC_POLL_ERROR_MAX_MS ? NFC_POLL_ERROR_MAX_MS : doubled;
    }
    /* Keep checking an active field often enough to enforce the link timeout.
     * Once inactive, repeated failures back off exponentially to five seconds. */
    uint32_t delay = canokey_nfc_is_active() ? NFC_POLL_ERROR_MIN_MS : s_poll_error_delay;
    s_next_poll_tick = now + delay;
}

static esp_err_t exchange(uint16_t command, const uint8_t *request, uint16_t request_len,
                          uint8_t *response, uint16_t capacity, uint16_t *length)
{
    uint16_t status = 0;
    *length = 0;
    esp_err_t ret = s_transceive(command, request, request_len, response, capacity,
                                 length, &status, NFC_UART_TIMEOUT_MS);
    if (ret != ESP_OK) return ret;
    if (*length > capacity) return ESP_ERR_INVALID_SIZE;
    return status == CANOKEY_NFC_STATUS_SUCCESS ? ESP_OK : ESP_ERR_NOT_SUPPORTED;
}

esp_err_t canokey_nfc_bind_transport(canokey_nfc_transceive_t transceive)
{
    if (!transceive) return ESP_ERR_INVALID_ARG;
    if (canokey_esp32p4_is_running()) return ESP_ERR_INVALID_STATE;
    s_transceive = transceive;
    s_disabled = false;
    s_remote_disabled = false;
    s_config_attempted = false;
    atomic_store(&s_disable_retry_count, 0u);
    s_next_poll_tick = 0;
    s_poll_error_delay = 0;
    return ESP_OK;
}

void canokey_nfc_set_enabled(bool enabled)
{
    atomic_store(&s_enabled, enabled);
    atomic_store(&s_disable_retry_count, 0u);
}

bool canokey_nfc_is_enabled(void) { return atomic_load(&s_enabled); }
bool canokey_nfc_is_active(void) { return atomic_load(&s_active); }
bool canokey_nfc_user_presence_pending(void) { return atomic_load(&s_presence_pending); }
uint8_t canokey_nfc_current_cbor_cmd(void) { return atomic_load(&s_cbor_cmd); }
uint32_t canokey_nfc_presence_token(void) { return atomic_load(&s_presence_token) & NFC_PRESENCE_TOKEN_MASK; }
void canokey_nfc_presence_requested(void)
{
    atomic_store(&s_presence_reply, 0);
    atomic_fetch_add(&s_presence_token, 1);
    atomic_store(&s_presence_pending, true);
}

bool canokey_nfc_submit_user_presence(uint32_t token, uint8_t result)
{
    if ((result != TOUCH_SHORT && result != TOUCH_CANCEL) ||
        !canokey_nfc_user_presence_pending() || token != canokey_nfc_presence_token()) return false;
    /* One atomic word binds the result to its prompt. Only the CanoKey task
     * consumes it, so a UI callback racing field loss cannot grant a new one. */
    atomic_store(&s_presence_reply, (token << 2) | result);
    return true;
}

static void consume_user_presence(void)
{
    uint32_t reply = atomic_exchange(&s_presence_reply, 0);
    if ((reply & 3u) && canokey_nfc_user_presence_pending() &&
        (reply >> 2) == canokey_nfc_presence_token()) set_touch_result((uint8_t)(reply & 3u));
}

uint8_t __real_get_touch_result(void);

uint8_t __wrap_get_touch_result(void)
{
    consume_user_presence();
    return __real_get_touch_result();
}

static void field_off(void)
{
    const uint32_t session = s_session_id;
    const bool was_active = canokey_nfc_is_active();
    if (canokey_nfc_is_active()) {
        device_applet_session_release(NFC_SESSION_OWNER);
        apdu_fido_chain_reset();
        init_apdu_buffer();
        stop_blinking();
        set_touch_result(TOUCH_NO);
    }
    set_nfc_state(0);
    atomic_store(&s_active, false);
    atomic_store(&s_presence_pending, false);
    atomic_store(&s_presence_reply, 0);
    atomic_store(&s_cbor_cmd, 0);
    s_session_id = 0;
    s_applet = NFC_APPLET_NONE;
    s_poll_pending = false;
    s_command_chaining = false;
    s_cached_event = 0;
    s_cached_cbor_cmd = 0;
    if (was_active) ESP_LOGI(TAG, "NFC field off: session=%lu", (unsigned long)session);
}

static esp_err_t field_on(uint32_t session)
{
    if (canokey_nfc_is_active()) {
        if (s_session_id == session) return ESP_OK;
        ESP_LOGW(TAG, "NFC session changed without FIELD_OFF: old=%lu new=%lu",
                 (unsigned long)s_session_id, (unsigned long)session);
        field_off();
    }
    /* Do not preempt a powered CCID card or an active/streaming HID command.
     * The PKE probe also detects a partially received large HID request. */
    if (canokey_port_ccid_session_active() ||
        device_applet_session_owner() != DEVICE_APPLET_SESSION_NONE ||
        !apdu_session_can_preempt() || CTAPHID_TxBusy()) return ESP_ERR_INVALID_STATE;
    if (pke_buffer_acquire(NFC_PKE_PROBE_OWNER) != 0) return ESP_ERR_INVALID_STATE;
    pke_buffer_release(NFC_PKE_PROBE_OWNER);
    if (device_applet_session_reset(NFC_SESSION_OWNER) != 0 ||
        device_applet_session_acquire(NFC_SESSION_OWNER) != 0) return ESP_ERR_INVALID_STATE;
    init_apdu_buffer();
    set_touch_result(TOUCH_NO);
    s_session_id = session;
    set_nfc_state(1);
    atomic_store(&s_active, true);
    ESP_LOGI(TAG, "NFC field on: session=%lu", (unsigned long)session);
    return ESP_OK;
}

static void reset_bridge(void)
{
    field_off();
    uint16_t length;
    uint8_t response[1];
    exchange(CANOKEY_NFC_CMD_RESET, NULL, 0, response, sizeof(response), &length);
    s_ack_session = 0;
    s_ack_event = 0;
    s_configured = false;
    s_config_tick = device_get_tick();
    s_next_poll_tick = 0;
    s_poll_error_delay = 0;
}

static esp_err_t configure_bridge(bool enabled)
{
    const uint8_t config[] = {
        CANOKEY_NFC_PROTOCOL_VERSION, enabled ? 1 : 0,
        (uint8_t)(NFC_MAX_CAPDU_SIZE >> 8), (uint8_t)NFC_MAX_CAPDU_SIZE,
        (uint8_t)(NFC_MAX_RAPDU_SIZE >> 8), (uint8_t)NFC_MAX_RAPDU_SIZE,
    };
    uint8_t reply[sizeof(config)];
    uint16_t length;
    esp_err_t ret = exchange(CANOKEY_NFC_CMD_CONFIGURE, config, sizeof(config),
                             reply, sizeof(reply), &length);
    if (ret != ESP_OK) return ret;
    if (length != sizeof(config) || memcmp(reply, config, sizeof(config)) != 0) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}

static void clear_bridge_state(void)
{
    field_off();
    s_ack_session = 0;
    s_ack_event = 0;
    s_cached_command_len = 0;
    s_cached_response_len = 0;
    s_configured = false;
    s_next_poll_tick = 0;
    s_poll_error_delay = 0;
}

static void disable_bridge(uint32_t now)
{
    clear_bridge_state();
    esp_err_t ret = configure_bridge(false);
    s_config_attempted = true;
    s_config_tick = now;
    if (ret == ESP_OK) {
        s_remote_disabled = true;
        s_config_attempted = false;
        atomic_store(&s_disable_retry_count, 0u);
        ESP_LOGI(TAG, "Chameleon NFC FIDO bridge disabled");
    } else if (ret == ESP_ERR_NOT_SUPPORTED) {
        /* Stock Chameleon firmware has no bridge to disable. */
        s_remote_disabled = true;
        s_disabled = true;
    } else {
        uint8_t attempts = atomic_load(&s_disable_retry_count);
        if (attempts < NFC_DISABLE_RETRY_MAX) {
            atomic_store(&s_disable_retry_count, (uint8_t)(attempts + 1u));
        }
        ESP_LOGW(TAG, "Failed to disable Chameleon NFC FIDO bridge: %s",
                 esp_err_to_name(ret));
    }
}

static bool is_fido_select(const CAPDU *capdu)
{
    return capdu->cla == 0x00 && capdu->ins == 0xA4 && capdu->p1 == 0x04 &&
           capdu->p2 == 0x00 && capdu->lc == sizeof(s_fido_aid) &&
           memcmp(capdu->data, s_fido_aid, sizeof(s_fido_aid)) == 0;
}

static bool is_ndef_select(const CAPDU *capdu)
{
    return capdu->cla == 0x00 && capdu->ins == 0xA4 && capdu->p1 == 0x04 &&
           capdu->p2 == 0x00 && capdu->lc == sizeof(s_ndef_aid) &&
           memcmp(capdu->data, s_ndef_aid, sizeof(s_ndef_aid)) == 0;
}

static bool is_fido_command(const CAPDU *capdu)
{
    if ((capdu->cla & 0xEF) == 0x80 && capdu->ins == 0x10) return true;
    if (capdu->cla != 0x00) return false;
    if (capdu->ins == 0x01 || capdu->ins == 0x02 || capdu->ins == 0x03) return true;
    return capdu->ins == 0xA4 && !(capdu->p1 == 0x04 && capdu->p2 == 0x00);
}

static bool is_nfc_presence_poll(const CAPDU *capdu)
{
    return capdu->cla == 0x80 && capdu->ins == 0x11;
}

static bool is_fido_selected_command(const CAPDU *capdu)
{
    if (is_fido_command(capdu) || is_nfc_presence_poll(capdu)) return true;
    return (capdu->cla == 0x00 || capdu->cla == 0x80) && capdu->ins == 0xC0;
}

static bool is_ndef_command(const CAPDU *capdu)
{
    if ((capdu->cla & 0xEF) != 0x00) return false;
    return capdu->ins == 0xA4 || capdu->ins == 0xB0 ||
           capdu->ins == 0xD6 || capdu->ins == 0xC0;
}

static const char *applet_name(void)
{
    switch (s_applet) {
    case NFC_APPLET_NDEF: return "ndef";
    case NFC_APPLET_FIDO: return "fido";
    default: return "none";
    }
}

static void process_nfc_apdu(uint32_t event_id, const uint8_t *command, uint16_t length,
                             uint8_t *response, uint16_t *response_len)
{
    uint8_t data[APDU_INCOMING_DATA_SIZE];
    uint8_t output[APDU_COMMAND_BUFFER_SIZE];
    CAPDU capdu = {.data = data};
    RAPDU rapdu = {.data = output, .len = 0, .sw = SW_WRONG_LENGTH};
    uint16_t total_len = 0;
    bool aggregate_fido_response = false;

    s_cached_cbor_cmd = 0;

    if (build_capdu(&capdu, command, length) < 0) {
        char raw_hex[65];
        bool truncated;
        format_log_bytes(command, length, raw_hex, sizeof(raw_hex), &truncated);
        ESP_LOGW(TAG, "NFC CAPDU parse failed: session=%lu event=%lu len=%u raw=%s%s",
                 (unsigned long)s_session_id, (unsigned long)event_id, length,
                 raw_hex, truncated ? "..." : "");
        goto done;
    }
    ESP_LOGI(TAG,
             "NFC CAPDU: session=%lu event=%lu cla=%02x ins=%02x p1=%02x p2=%02x lc=%u le=%lu%s",
             (unsigned long)s_session_id, (unsigned long)event_id,
             capdu.cla, capdu.ins, capdu.p1, capdu.p2, capdu.lc,
             (unsigned long)capdu.le, capdu.extended ? " extended" : "");
    const bool select_apdu = capdu.cla == 0x00 && capdu.ins == 0xA4 &&
                             capdu.p1 == 0x04 && capdu.p2 == 0x00;
    const bool fido_select = is_fido_select(&capdu);
    const bool ndef_select = is_ndef_select(&capdu);
    if (select_apdu) {
        char aid_hex[65];
        bool truncated;
        format_log_bytes(capdu.data, capdu.lc, aid_hex, sizeof(aid_hex), &truncated);
        ESP_LOGI(TAG, "NFC SELECT AID: session=%lu event=%lu aid=%s%s",
                 (unsigned long)s_session_id, (unsigned long)event_id,
                 aid_hex, truncated ? "..." : "");
    }
    const bool implicit_fido = !select_apdu && s_applet == NFC_APPLET_NONE &&
                               is_fido_command(&capdu);
    if (s_poll_pending && !is_nfc_presence_poll(&capdu)) {
        /* Once CTAP has returned 9100, the only legal next APDU is the NFC
         * GET RESPONSE poll. Do not let a reader probe or re-select clear
         * the pending UI request. */
        rapdu.sw = SW_CONDITIONS_NOT_SATISFIED;
        goto done;
    } else if (select_apdu && !fido_select && !ndef_select) {
        /* Abort any prior applet state when the reader selects an unknown AID. */
        apdu_fido_chain_reset();
        init_apdu_buffer();
        s_applet = NFC_APPLET_NONE;
        s_command_chaining = false;
        atomic_store(&s_cbor_cmd, 0);
        rapdu.sw = SW_FILE_NOT_FOUND;
        goto done;
    } else if (!select_apdu) {
        if (s_applet == NFC_APPLET_FIDO && !is_fido_selected_command(&capdu)) {
            rapdu.sw = SW_INS_NOT_SUPPORTED;
            goto done;
        }
        if (s_applet == NFC_APPLET_NDEF && !is_ndef_command(&capdu)) {
            /* Selecting NDEF must never authorize a CTAP command. */
            rapdu.sw = SW_CONDITIONS_NOT_SATISFIED;
            goto done;
        }
        if (s_applet == NFC_APPLET_NONE && !implicit_fido) {
            rapdu.sw = SW_CONDITIONS_NOT_SATISFIED;
            goto done;
        }
    }
    if (select_apdu) {
        /* A new AID selection terminates local command-chain bookkeeping. */
        s_command_chaining = false;
        atomic_store(&s_cbor_cmd, 0);
    }
    if (implicit_fido) s_applet = NFC_APPLET_FIDO;
    if (acquire_apdu_buffer(NFC_BUFFER_OWNER) != 0) {
        rapdu.sw = SW_CONDITIONS_NOT_SATISFIED;
        goto done;
    }
    const bool fido_ctap_apdu = (capdu.cla & 0xEF) == 0x80 && capdu.ins == 0x10;
    const bool first_fido_fragment = fido_ctap_apdu && !s_command_chaining;
    if (fido_ctap_apdu && capdu.lc && !s_poll_pending) {
        /* A chained command's first fragment carries the CBOR command byte. */
        if (first_fido_fragment) {
            atomic_store(&s_cbor_cmd, capdu.data[0]);
            s_cached_cbor_cmd = capdu.data[0];
            ESP_LOGI(TAG, "NFC CTAP command: session=%lu event=%lu cmd=0x%02x",
                     (unsigned long)s_session_id, (unsigned long)event_id, capdu.data[0]);
        }
        s_command_chaining = (capdu.cla & 0x10) != 0;

        const uint8_t cbor_cmd = first_fido_fragment ? capdu.data[0] : s_cached_cbor_cmd;
        if (cbor_cmd == NFC_CTAP_GET_ASSERTION) {
            ESP_LOGW(TAG,
                     "getAssertionRequest: session=%lu event=%lu first=%u chained=%u len=%u",
                     (unsigned long)s_session_id, (unsigned long)event_id,
                     first_fido_fragment ? 1 : 0, s_command_chaining ? 1 : 0, capdu.lc);
            ESP_LOG_BUFFER_HEXDUMP(TAG, capdu.data, capdu.lc, ESP_LOG_WARN);
        }
    }
    if (s_poll_pending && get_touch_result() == TOUCH_CANCEL) {
        /* Core NFC polling treats any nonzero touch as approval. Translate
         * UI cancellation to the protocol's cancel request before dispatch. */
        capdu = (CAPDU){.data = data, .cla = 0x80, .ins = 0x11, .p1 = 0x11, .le = 256};
        set_touch_result(TOUCH_NO);
    } else if (!s_poll_pending && !canokey_nfc_user_presence_pending()) {
        set_touch_result(TOUCH_NO);
    }
    aggregate_fido_response = capdu.extended && is_fido_command(&capdu);
    device_applet_session_touch(NFC_SESSION_OWNER);
    process_apdu_from(&capdu, &rapdu, APDU_TRANSPORT_NFC);
    const bool selected = (fido_select || ndef_select) && rapdu.sw == SW_NO_ERROR;
    /* Extended FIDO APDUs expect one complete RAPDU. Drain the core's internal
     * 61xx chunks here; Chameleon applies ISO-DEP block chaining afterwards. */
    while (aggregate_fido_response && (rapdu.sw >> 8) == 0x61) {
        if (rapdu.len == 0 || rapdu.len > APDU_BUFFER_SIZE ||
            total_len + rapdu.len > NFC_MAX_RAPDU_SIZE - 2) {
            total_len = 0;
            rapdu.len = 0;
            rapdu.sw = SW_UNABLE_TO_PROCESS;
            apdu_response_source_clear();
            break;
        }
        memcpy(response + total_len, output, rapdu.len);
        total_len += rapdu.len;
        CAPDU next = {.data = data, .cla = 0x00, .ins = 0xC0, .le = APDU_BUFFER_SIZE};
        process_apdu_from(&next, &rapdu, APDU_TRANSPORT_NFC);
    }
    release_apdu_buffer(NFC_BUFFER_OWNER);
    device_applet_session_touch(NFC_SESSION_OWNER);
    if (fido_select || ndef_select) {
        s_applet = selected ? (fido_select ? NFC_APPLET_FIDO : NFC_APPLET_NDEF)
                            : NFC_APPLET_NONE;
    }
    s_poll_pending = rapdu.sw == 0x9100;
    if (s_poll_pending && !canokey_nfc_user_presence_pending()) {
        extern void device_user_presence_request(uint8_t entry);
        device_user_presence_request(CANOKEY_NFC_WAIT_ENTRY);
    } else if (!s_poll_pending) {
        atomic_store(&s_presence_pending, false);
        if (!s_command_chaining) atomic_store(&s_cbor_cmd, 0);
        if (rapdu.sw != SW_NO_ERROR && (rapdu.sw >> 8) != 0x61) s_command_chaining = false;
    }
done:
    if (rapdu.len > APDU_BUFFER_SIZE || total_len + rapdu.len > NFC_MAX_RAPDU_SIZE - 2) {
        total_len = 0;
        rapdu.len = 0;
        rapdu.sw = SW_UNABLE_TO_PROCESS;
    }
    memcpy(response + total_len, output, rapdu.len);
    total_len += rapdu.len;
    response[total_len] = (uint8_t)(rapdu.sw >> 8);
    response[total_len + 1] = (uint8_t)rapdu.sw;
    *response_len = total_len + 2;
    ESP_LOGI(TAG,
             "NFC RAPDU: session=%lu event=%lu data_len=%u sw=%04x applet=%s presence_pending=%u",
             (unsigned long)s_session_id, (unsigned long)event_id,
             total_len, rapdu.sw, applet_name(),
             canokey_nfc_user_presence_pending() ? 1 : 0);
}

static esp_err_t send_cached_response(void)
{
    uint8_t ack[NFC_IDS_SIZE];
    uint16_t length;
    esp_err_t ret = exchange(CANOKEY_NFC_CMD_RESPONSE, s_cached_response,
                             NFC_IDS_SIZE + s_cached_response_len, ack, sizeof(ack), &length);
    if (ret == ESP_OK && (length != sizeof(ack) || memcmp(ack, s_cached_response, sizeof(ack)) != 0)) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ret;
}

static void log_cached_get_info_response(uint32_t session, uint32_t event_id)
{
    if (s_cached_cbor_cmd != NFC_CTAP_GET_INFO) return;
    if (s_cached_response_len < 3) {
        ESP_LOGW(TAG, "NFC GetInfo response too short: session=%lu event=%lu len=%u",
                 (unsigned long)session, (unsigned long)event_id, s_cached_response_len);
        return;
    }

    const uint8_t *ctap_response = s_cached_response + NFC_IDS_SIZE;
    const uint16_t ctap_len = s_cached_response_len - 2;
    const uint16_t cbor_len = ctap_len - 1;
    const uint16_t sw = ((uint16_t)ctap_response[ctap_len] << 8) |
                        ctap_response[ctap_len + 1];
    ESP_LOGI(TAG,
             "NFC GetInfo CTAP status + CBOR: session=%lu event=%lu ctap_status=0x%02x cbor_len=%u sw=%04x",
             (unsigned long)session, (unsigned long)event_id,
             ctap_response[0], cbor_len, sw);
    ESP_LOG_BUFFER_HEXDUMP(TAG, ctap_response, ctap_len, ESP_LOG_INFO);
}

void canokey_nfc_loop(void)
{
    if (!s_transceive) return;
    uint32_t now = device_get_tick();
    if (!canokey_nfc_is_enabled()) {
        if (s_remote_disabled || s_disabled) return;
        if (atomic_load(&s_disable_retry_count) >= NFC_DISABLE_RETRY_MAX) return;
        if (s_config_attempted && now - s_config_tick < NFC_MODE_RETRY_MS) return;
        disable_bridge(now);
        return;
    }
    if (s_disabled) return;
    s_remote_disabled = false;
    if (!s_configured) {
        if (s_config_attempted && now - s_config_tick < NFC_RETRY_MS) return;
        s_config_attempted = true;
        s_config_tick = now;
        esp_err_t ret = configure_bridge(true);
        if (ret == ESP_ERR_NOT_SUPPORTED || ret == ESP_ERR_INVALID_RESPONSE) {
            reset_bridge();
            s_disabled = true;
            ESP_LOGW(TAG, "Chameleon firmware does not support NFC FIDO bridge v1; USB remains available");
            return;
        }
        if (ret != ESP_OK) return;
        s_configured = true;
        s_config_attempted = false;
        s_last_link_tick = device_get_tick();
        schedule_poll_success(s_last_link_tick);
        ESP_LOGI(TAG, "Chameleon NFC FIDO bridge v1 ready");
        return;
    }

    if (!poll_is_due(now)) return;
    if (canokey_nfc_is_active()) device_applet_session_touch(NFC_SESSION_OWNER);
    uint8_t ack[NFC_IDS_SIZE];
    write_be32(ack, s_ack_session);
    write_be32(ack + 4, s_ack_event);
    uint8_t event[NFC_EVENT_HEADER_SIZE + NFC_MAX_CAPDU_SIZE];
    uint16_t length;
    esp_err_t ret = exchange(CANOKEY_NFC_CMD_POLL, ack, sizeof(ack), event, sizeof(event), &length);
    if (ret != ESP_OK) {
        now = device_get_tick();
        if (canokey_nfc_is_active() && now - s_last_link_tick >= NFC_LINK_TIMEOUT_MS) {
            ESP_LOGW(TAG, "NFC link lost; dropping RF session");
            reset_bridge();
        } else {
            schedule_poll_error(now);
        }
        return;
    }
    s_last_link_tick = device_get_tick();
    uint32_t session = 0;
    uint32_t id = 0;
    if (length < NFC_EVENT_HEADER_SIZE) goto invalid;
    session = read_be32(event + 1);
    id = read_be32(event + 5);
    if (event[0] == CANOKEY_NFC_EVENT_NONE) {
        if (length != NFC_EVENT_HEADER_SIZE || session != 0 || id != 0) goto invalid;
        if (s_cached_event && s_ack_event != s_cached_event) goto invalid;
        schedule_poll_success(s_last_link_tick);
        return;
    }
    if (!session || !id) goto invalid;
    if (session == s_ack_session && id < s_ack_event) goto invalid;
    switch (event[0]) {
    case CANOKEY_NFC_EVENT_FIELD_ON:
        if (length != NFC_EVENT_HEADER_SIZE) goto invalid;
        if (field_on(session) != ESP_OK) {
            /* Retry the unacknowledged event after the USB session ends. */
            s_next_poll_tick = s_last_link_tick + NFC_POLL_ACTIVE_MS;
            return;
        }
        break;
    case CANOKEY_NFC_EVENT_FIELD_OFF:
        if (length != NFC_EVENT_HEADER_SIZE) goto invalid;
        if (s_session_id == session) {
            field_off();
        } else {
            ESP_LOGW(TAG, "Ignoring stale FIELD_OFF: active=%lu event_session=%lu event=%lu",
                     (unsigned long)s_session_id, (unsigned long)session, (unsigned long)id);
        }
        break;
    case CANOKEY_NFC_EVENT_APDU:
        if (!canokey_nfc_is_active() || session != s_session_id || length == NFC_EVENT_HEADER_SIZE) goto invalid;
        if (id == s_cached_event) {
            if (s_cached_command_len != length - NFC_EVENT_HEADER_SIZE ||
                memcmp(s_cached_command, event + NFC_EVENT_HEADER_SIZE, s_cached_command_len) != 0) goto invalid;
            ESP_LOGW(TAG, "Retrying cached NFC RAPDU: session=%lu event=%lu",
                     (unsigned long)session, (unsigned long)id);
        } else {
            /* A changed ID after the prior send timed out is a protocol error:
             * the frontend must keep that APDU pending until RAPDU acceptance. */
            if (s_cached_event && s_ack_event != s_cached_event) goto invalid;
            s_cached_event = id;
            s_cached_command_len = length - NFC_EVENT_HEADER_SIZE;
            memcpy(s_cached_command, event + NFC_EVENT_HEADER_SIZE, s_cached_command_len);
            write_be32(s_cached_response, session);
            write_be32(s_cached_response + 4, id);
            process_nfc_apdu(id, s_cached_command, s_cached_command_len,
                             s_cached_response + NFC_IDS_SIZE, &s_cached_response_len);
            s_cached_tick = device_get_tick();
        }
        ret = send_cached_response();
        if (ret != ESP_OK) {
            now = device_get_tick();
            if (now - s_cached_tick >= NFC_LINK_TIMEOUT_MS) {
                ESP_LOGW(TAG, "NFC RAPDU acknowledgement timeout: session=%lu event=%lu err=%s",
                         (unsigned long)session, (unsigned long)id, esp_err_to_name(ret));
                reset_bridge();
            } else {
                schedule_poll_error(now);
            }
            return;
        }
        ESP_LOGI(TAG, "NFC RAPDU accepted by Chameleon: session=%lu event=%lu len=%u",
                 (unsigned long)session, (unsigned long)id, s_cached_response_len);
        log_cached_get_info_response(session, id);
        break;
    default:
        goto invalid;
    }
    s_ack_session = session;
    s_ack_event = id;
    schedule_poll_success(device_get_tick());
    return;
invalid:
    ESP_LOGW(TAG, "Invalid NFC bridge event: type=%u session=%lu event=%lu len=%u; resetting session",
             length ? event[0] : 0, (unsigned long)session, (unsigned long)id, length);
    reset_bridge();
}

uint8_t __real_CTAPHID_Loop(uint8_t wait_for_user);

uint8_t __wrap_CTAPHID_Loop(uint8_t wait_for_user)
{
    if (canokey_nfc_is_active()) {
        consume_user_presence();
        /* Drain USB reports through the port's busy-response path, without
         * letting core HID dispatch touch NFC's APDU/PKE/CTAP state. */
        USBD_CTAPHID_ServiceReceive();
        return LOOP_SUCCESS;
    }
    return __real_CTAPHID_Loop(wait_for_user);
}

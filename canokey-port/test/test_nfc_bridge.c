/* Freestanding harness: run actual port_nfc.c against a fake UART and
 * applet engine. No target hardware, CRT, or CanoKey source edits required. */
#include "canokey_nfc.h"
#include "apdu.h"
#include "device.h"
#include "string.h"

static uint32_t tick;
static device_applet_session_owner_t owner;
static uint8_t touch;
static bool ccid_busy;
static bool pke_busy;
static bool hid_busy;
static uint8_t rf;
static unsigned dispatches, resets, prompts, real_hid_loops, drains;
static unsigned configurations, configuration_enables, configuration_disables;
static unsigned polls, response_sends, source_clears;
static uint8_t event_data[512], sent_response[4096];
static uint16_t event_len, sent_len;
static uint32_t session = 1, event_id;
static bool response_timeout, poll_timeout, unsupported;
static uint8_t event_type;
static unsigned bad_cancel_grants;
static enum { MOCK_APPLET_NONE, MOCK_APPLET_NDEF, MOCK_APPLET_FIDO } mock_applet;
static uint16_t mock_ndef_file;
static bool mock_get_info_more;

void *memcpy(void *destination, const void *source, size_t count)
{
    uint8_t *out = destination;
    const uint8_t *in = source;
    for (size_t i = 0; i < count; i++) out[i] = in[i];
    return destination;
}

void *memset(void *destination, int value, size_t count)
{
    uint8_t *out = destination;
    for (size_t i = 0; i < count; i++) out[i] = (uint8_t)value;
    return destination;
}

int memcmp(const void *left, const void *right, size_t count)
{
    const uint8_t *a = left, *b = right;
    for (size_t i = 0; i < count; i++) if (a[i] != b[i]) return a[i] - b[i];
    return 0;
}

bool canokey_esp32p4_is_running(void) { return false; }
bool canokey_port_ccid_session_active(void) { return ccid_busy; }
uint32_t device_get_tick(void) { return tick; }
void device_applet_session_touch(device_applet_session_owner_t value) { (void)value; }
void device_applet_session_release(device_applet_session_owner_t value) { if (owner == value) owner = 0; }
int device_applet_session_acquire(device_applet_session_owner_t value) { owner = value; return 0; }
int device_applet_session_reset(device_applet_session_owner_t value) { (void)value; owner = 0; return 0; }
device_applet_session_owner_t device_applet_session_owner(void) { return owner; }
void stop_blinking(void) {}
void set_touch_result(uint8_t value) { touch = value; }
uint8_t get_touch_result(void) { return touch; }
void set_nfc_state(uint8_t value) { rf = value; }
int acquire_apdu_buffer(uint8_t value) { (void)value; return 0; }
int release_apdu_buffer(uint8_t value) { (void)value; return 0; }
int apdu_session_can_preempt(void) { return 1; }
void init_apdu_buffer(void)
{
    mock_applet = MOCK_APPLET_NONE;
    mock_ndef_file = 0;
    mock_get_info_more = false;
}
void apdu_response_source_clear(void) { source_clears++; }
void apdu_fido_chain_reset(void) {}
int pke_buffer_acquire(uint8_t value) { (void)value; return pke_busy ? -1 : 0; }
int pke_buffer_release(uint8_t value) { (void)value; return 0; }
uint8_t CTAPHID_TxBusy(void) { return hid_busy; }
uint8_t __real_CTAPHID_Loop(uint8_t wait) { (void)wait; real_hid_loops++; return 0; }
void USBD_CTAPHID_ServiceReceive(void) { drains++; }
void device_user_presence_request(uint8_t entry)
{
    if (entry == CANOKEY_NFC_WAIT_ENTRY) {
        canokey_nfc_presence_requested();
        prompts++;
    }
}

int build_capdu(CAPDU *capdu, const uint8_t *command, uint16_t length)
{
    if (length < 4) return -1;
    capdu->cla = command[0]; capdu->ins = command[1];
    capdu->p1 = command[2]; capdu->p2 = command[3];
    capdu->lc = 0; capdu->le = 256; capdu->extended = 0;
    if (length == 4) return 0;
    if (length == 5) { capdu->le = command[4] ? command[4] : 256; return 0; }
    if (command[4] == 0) {
        if (length < 7) return -1;
        capdu->extended = 1;
        if (length == 7) {
            capdu->le = ((uint32_t)command[5] << 8) | command[6];
            if (capdu->le == 0) capdu->le = 65536;
            return 0;
        }
        capdu->lc = ((uint16_t)command[5] << 8) | command[6];
        if (length < 7 + capdu->lc) return -1;
        memcpy(capdu->data, command + 7, capdu->lc);
        if (length == 9 + capdu->lc) {
            capdu->le = ((uint32_t)command[7 + capdu->lc] << 8) | command[8 + capdu->lc];
            if (capdu->le == 0) capdu->le = 65536;
        }
        return 0;
    }
    if (length < 5 + command[4]) return -1;
    capdu->lc = command[4];
    memcpy(capdu->data, command + 5, capdu->lc);
    if (length == 6 + capdu->lc) capdu->le = command[5 + capdu->lc] ? command[5 + capdu->lc] : 256;
    return 0;
}

void process_apdu_from(CAPDU *capdu, RAPDU *rapdu, apdu_transport_t transport)
{
    static const uint8_t fido_aid[] = {0xA0, 0, 0, 6, 0x47, 0x2F, 0, 1};
    static const uint8_t ndef_aid[] = {0xD2, 0x76, 0, 0, 0x85, 1, 1};
    static const uint8_t cc[] = {
        0x00, 0x0F, 0x20, 0x04, 0x00, 0x04, 0x00, 0x04,
        0x06, 0x00, 0x01, 0x04, 0x00, 0x00, 0x00,
    };
    dispatches++;
    rapdu->len = 0; rapdu->sw = 0x9000;
    if (transport != APDU_TRANSPORT_NFC) { rapdu->sw = 0x6F00; return; }
    if (capdu->cla == 0 && capdu->ins == 0xA4 && capdu->p1 == 4 && capdu->p2 == 0) {
        if (capdu->lc == sizeof(fido_aid) && memcmp(capdu->data, fido_aid, sizeof(fido_aid)) == 0) {
            mock_applet = MOCK_APPLET_FIDO;
        } else if (capdu->lc == sizeof(ndef_aid) && memcmp(capdu->data, ndef_aid, sizeof(ndef_aid)) == 0) {
            mock_applet = MOCK_APPLET_NDEF;
            mock_ndef_file = 0;
            return;
        } else {
            mock_applet = MOCK_APPLET_NONE;
            rapdu->sw = 0x6A82;
            return;
        }
    }
    if (mock_applet == MOCK_APPLET_NDEF) {
        if (capdu->cla == 0 && capdu->ins == 0xA4 && capdu->p1 == 0 &&
            capdu->p2 == 0x0C && capdu->lc == 2) {
            mock_ndef_file = ((uint16_t)capdu->data[0] << 8) | capdu->data[1];
            if (mock_ndef_file != 0xE103 && mock_ndef_file != 0x0001) rapdu->sw = 0x6A82;
        } else if (capdu->cla == 0 && capdu->ins == 0xB0 && mock_ndef_file == 0xE103) {
            rapdu->len = capdu->le < sizeof(cc) ? (uint16_t)capdu->le : sizeof(cc);
            memcpy(rapdu->data, cc, rapdu->len);
        } else if (capdu->cla == 0 && capdu->ins == 0xB0 && mock_ndef_file == 0x0001) {
            rapdu->data[0] = 0;
            rapdu->data[1] = 0x11;
            rapdu->len = capdu->le < 2 ? (uint16_t)capdu->le : 2;
        } else {
            rapdu->sw = 0x6D00;
        }
        return;
    }
    if (mock_applet == MOCK_APPLET_NONE) { rapdu->sw = 0x6A82; return; }
    if (capdu->ins == 0xA4) {
        memcpy(rapdu->data, "U2F_V2", 6); rapdu->len = 6;
    } else if (capdu->ins == 0x10 && capdu->p1 == 0 && capdu->extended &&
               capdu->lc == 1 && capdu->data[0] == 4) {
        for (unsigned i = 0; i < 250; i++) rapdu->data[i] = (uint8_t)i;
        rapdu->len = 250;
        rapdu->sw = 0x61AF;
        mock_get_info_more = true;
    } else if (capdu->ins == 0x10 && capdu->p1 == 0x80) {
        rapdu->data[0] = 2; rapdu->len = 1; rapdu->sw = 0x9100;
    } else if (capdu->ins == 0x11) {
        if (capdu->p1 == 0x11) rapdu->data[0] = 0x2D;
        else {
            if (touch == TOUCH_CANCEL) bad_cancel_grants++;
            rapdu->data[0] = 0;
        }
        rapdu->len = 1;
    } else if (capdu->ins == 0x01 && capdu->extended) {
        for (unsigned i = 0; i < 256; i++) rapdu->data[i] = (uint8_t)i;
        rapdu->len = 256; rapdu->sw = 0x6101;
    } else if (capdu->ins == 0xC0 && mock_get_info_more) {
        for (unsigned i = 0; i < 175; i++) rapdu->data[i] = (uint8_t)(250 + i);
        rapdu->len = 175;
        mock_get_info_more = false;
    } else if (capdu->ins == 0xC0) {
        rapdu->data[0] = 0xAB; rapdu->len = 1;
    }
}

static void be32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)(value >> 24); p[1] = (uint8_t)(value >> 16);
    p[2] = (uint8_t)(value >> 8); p[3] = (uint8_t)value;
}

static void queue_event(uint8_t type, const uint8_t *command, uint16_t length)
{
    event_type = type;
    event_data[0] = type;
    be32(event_data + 1, session);
    be32(event_data + 5, ++event_id);
    if (length) memcpy(event_data + 9, command, length);
    event_len = 9 + length;
}

static esp_err_t uart_exchange(uint16_t command, const uint8_t *request, uint16_t request_len,
                               uint8_t *response, uint16_t capacity, uint16_t *length,
                               uint16_t *status, uint32_t timeout)
{
    (void)timeout;
    *status = CANOKEY_NFC_STATUS_SUCCESS;
    *length = 0;
    if (command == CANOKEY_NFC_CMD_CONFIGURE) {
        configurations++;
        if (unsupported) { *status = 0x0067; return ESP_OK; }
        if (request_len != 6 || request[0] != 1 || request[4] != 0x0F || request[5] != 0xF8) return ESP_ERR_INVALID_ARG;
        if (request[1] == 1) configuration_enables++;
        else if (request[1] == 0) { configuration_disables++; event_type = 0; }
        else return ESP_ERR_INVALID_ARG;
        memcpy(response, request, 6); *length = 6;
    } else if (command == CANOKEY_NFC_CMD_POLL) {
        polls++;
        if (poll_timeout) return ESP_ERR_TIMEOUT;
        if (event_type && memcmp(request, event_data + 1, 8) == 0) event_type = 0;
        if (event_type) {
            if (event_len > capacity) return ESP_ERR_INVALID_SIZE;
            memcpy(response, event_data, event_len); *length = event_len;
        } else {
            for (unsigned i = 0; i < 9; i++) response[i] = 0;
            *length = 9;
        }
    } else if (command == CANOKEY_NFC_CMD_RESPONSE) {
        response_sends++;
        memcpy(sent_response, request, request_len); sent_len = request_len;
        if (response_timeout) return ESP_ERR_TIMEOUT;
        memcpy(response, request, 8); *length = 8;
    } else if (command == CANOKEY_NFC_CMD_RESET) {
        resets++; event_type = 0;
    }
    return ESP_OK;
}

#define CHECK(condition) do { if (!(condition)) return __LINE__; } while (0)
extern uint8_t __wrap_CTAPHID_Loop(uint8_t wait);

int test_nfc_bridge(void)
{
    const uint8_t select[] = {0, 0xA4, 4, 0, 8, 0xA0, 0, 0, 6, 0x47, 0x2F, 0, 1, 0};
    const uint8_t msg[] = {0x80, 0x10, 0x80, 0, 1, 1, 0};
    const uint8_t poll[] = {0x80, 0x11, 0, 0, 0};
    const uint8_t u2f_extended[] = {0, 1, 0, 0, 0, 0, 0};
    const uint8_t get_info_extended[] = {0x80, 0x10, 0, 0, 0, 0, 1, 4, 0, 0};
    CHECK(canokey_nfc_bind_transport(uart_exchange) == ESP_OK);
    canokey_nfc_set_enabled(true);

    queue_event(CANOKEY_NFC_EVENT_FIELD_ON, 0, 0);
    ccid_busy = true;
    canokey_nfc_loop(); CHECK(!canokey_nfc_is_active());
    ccid_busy = false; pke_busy = true;
    canokey_nfc_loop(); CHECK(!canokey_nfc_is_active());
    pke_busy = false; hid_busy = true;
    canokey_nfc_loop(); CHECK(!canokey_nfc_is_active());
    hid_busy = false;
    canokey_nfc_loop(); CHECK(canokey_nfc_is_active() && rf == 1);
    __wrap_CTAPHID_Loop(1); CHECK(drains == 1 && real_hid_loops == 0);

    queue_event(CANOKEY_NFC_EVENT_APDU, select, sizeof(select));
    response_timeout = true;
    canokey_nfc_loop();
    CHECK(dispatches == 1 && sent_len == 16 && memcmp(sent_response + 8, "U2F_V2\x90\x00", 8) == 0);
    canokey_nfc_loop(); CHECK(dispatches == 1 && response_sends == 2);
    response_timeout = false;
    canokey_nfc_loop(); CHECK(dispatches == 1 && response_sends == 3);

    /* Reusing an ID with changed bytes must reset, never dispatch again. */
    event_data[13] ^= 1;
    canokey_nfc_loop();
    /* The preceding successful RESPONSE is acknowledged by this POLL. */
    CHECK(dispatches == 1);
    /* Replay the changed ID explicitly despite its previous acknowledgement. */
    event_type = CANOKEY_NFC_EVENT_APDU;
    be32(event_data + 5, ++event_id);
    queue_event(CANOKEY_NFC_EVENT_APDU, select, sizeof(select));
    response_timeout = true;
    canokey_nfc_loop();
    event_data[13] ^= 1;
    canokey_nfc_loop(); CHECK(resets == 1 && !canokey_nfc_is_active());
    response_timeout = false;

    tick += 5000; session++; event_id = 0;
    canokey_nfc_loop();
    queue_event(CANOKEY_NFC_EVENT_FIELD_ON, 0, 0); canokey_nfc_loop();
    queue_event(CANOKEY_NFC_EVENT_APDU, select, sizeof(select)); canokey_nfc_loop();
    queue_event(CANOKEY_NFC_EVENT_APDU, msg, sizeof(msg)); canokey_nfc_loop();
    CHECK(canokey_nfc_user_presence_pending() && prompts == 1 && canokey_nfc_current_cbor_cmd() == 1);
    uint32_t token = canokey_nfc_presence_token();
    CHECK(canokey_nfc_submit_user_presence(token, TOUCH_CANCEL));
    queue_event(CANOKEY_NFC_EVENT_APDU, poll, sizeof(poll)); canokey_nfc_loop();
    CHECK(sent_len == 11 && memcmp(sent_response + 8, "\x2D\x90\x00", 3) == 0);
    CHECK(!canokey_nfc_user_presence_pending() && bad_cancel_grants == 0);

    queue_event(CANOKEY_NFC_EVENT_APDU, msg, sizeof(msg)); canokey_nfc_loop();
    CHECK(canokey_nfc_presence_token() != token && prompts == 2);
    CHECK(!canokey_nfc_submit_user_presence(token, TOUCH_SHORT));
    CHECK(canokey_nfc_submit_user_presence(canokey_nfc_presence_token(), TOUCH_SHORT));
    queue_event(CANOKEY_NFC_EVENT_APDU, poll, sizeof(poll)); canokey_nfc_loop();
    CHECK(sent_response[8] == 0 && !canokey_nfc_user_presence_pending());

    queue_event(CANOKEY_NFC_EVENT_APDU, u2f_extended, sizeof(u2f_extended)); canokey_nfc_loop();
    CHECK(sent_len == 267 && sent_response[264] == 0xAB && sent_response[265] == 0x90 && sent_response[266] == 0);

    unsigned get_info_before = dispatches;
    queue_event(CANOKEY_NFC_EVENT_APDU, get_info_extended, sizeof(get_info_extended)); canokey_nfc_loop();
    CHECK(dispatches == get_info_before + 2 && sent_len == 435 &&
          sent_response[8] == 0 && sent_response[257] == 249 &&
          sent_response[258] == 250 && sent_response[432] == (uint8_t)424 &&
          sent_response[433] == 0x90 && sent_response[434] == 0);

    queue_event(CANOKEY_NFC_EVENT_APDU, msg, sizeof(msg)); canokey_nfc_loop();
    queue_event(CANOKEY_NFC_EVENT_FIELD_OFF, 0, 0); canokey_nfc_loop();
    CHECK(!canokey_nfc_is_active() && !canokey_nfc_user_presence_pending() && owner == 0 && touch == 0 && rf == 0);

    /* Android may complete Type-4 NDEF discovery before selecting FIDO.
     * NDEF commands are routed to that applet but never authorize CTAP. */
    session++; event_id = 0;
    const uint8_t ndef_select[] = {0, 0xA4, 4, 0, 7, 0xD2, 0x76, 0, 0, 0x85, 1, 1, 0};
    const uint8_t select_cc[] = {0, 0xA4, 0, 0x0C, 2, 0xE1, 0x03};
    const uint8_t read_cc[] = {0, 0xB0, 0, 0, 15};
    const uint8_t select_ndef_file[] = {0, 0xA4, 0, 0x0C, 2, 0, 1};
    const uint8_t read_ndef_len[] = {0, 0xB0, 0, 0, 2};
    const uint8_t expected_cc[] = {
        0x00, 0x0F, 0x20, 0x04, 0x00, 0x04, 0x00, 0x04,
        0x06, 0x00, 0x01, 0x04, 0x00, 0x00, 0x00,
    };
    queue_event(CANOKEY_NFC_EVENT_FIELD_ON, 0, 0); canokey_nfc_loop();
    unsigned dispatch_before = dispatches;
    queue_event(CANOKEY_NFC_EVENT_APDU, ndef_select, sizeof(ndef_select)); canokey_nfc_loop();
    CHECK(dispatches == dispatch_before + 1 && sent_len == 10 &&
          sent_response[8] == 0x90 && sent_response[9] == 0x00);
    queue_event(CANOKEY_NFC_EVENT_APDU, select_cc, sizeof(select_cc)); canokey_nfc_loop();
    CHECK(sent_len == 10 && sent_response[8] == 0x90 && sent_response[9] == 0x00);
    queue_event(CANOKEY_NFC_EVENT_APDU, read_cc, sizeof(read_cc)); canokey_nfc_loop();
    CHECK(sent_len == 25 && memcmp(sent_response + 8, expected_cc, sizeof(expected_cc)) == 0 &&
          sent_response[23] == 0x90 && sent_response[24] == 0x00);
    queue_event(CANOKEY_NFC_EVENT_APDU, select_ndef_file, sizeof(select_ndef_file)); canokey_nfc_loop();
    CHECK(sent_response[8] == 0x90 && sent_response[9] == 0x00);
    queue_event(CANOKEY_NFC_EVENT_APDU, read_ndef_len, sizeof(read_ndef_len)); canokey_nfc_loop();
    CHECK(sent_len == 12 && sent_response[8] == 0 && sent_response[9] == 0x11 &&
          sent_response[10] == 0x90 && sent_response[11] == 0x00);
    dispatch_before = dispatches;
    queue_event(CANOKEY_NFC_EVENT_APDU, msg, sizeof(msg)); canokey_nfc_loop();
    CHECK(dispatches == dispatch_before && !canokey_nfc_user_presence_pending() &&
          sent_response[8] == 0x69 && sent_response[9] == 0x85);
    queue_event(CANOKEY_NFC_EVENT_APDU, select, sizeof(select)); canokey_nfc_loop();
    CHECK(sent_len == 16 && memcmp(sent_response + 8, "U2F_V2\x90\x00", 8) == 0);
    queue_event(CANOKEY_NFC_EVENT_APDU, msg, sizeof(msg)); canokey_nfc_loop();
    CHECK(canokey_nfc_user_presence_pending() && prompts == 4);
    const uint8_t admin_probe[] = {0, 0x20, 0, 0};
    queue_event(CANOKEY_NFC_EVENT_APDU, admin_probe, sizeof(admin_probe)); canokey_nfc_loop();
    CHECK(canokey_nfc_user_presence_pending() && sent_response[8] == 0x69 && sent_response[9] == 0x85);
    token = canokey_nfc_presence_token();
    CHECK(canokey_nfc_submit_user_presence(token, TOUCH_CANCEL));
    queue_event(CANOKEY_NFC_EVENT_APDU, poll, sizeof(poll)); canokey_nfc_loop();
    CHECK(!canokey_nfc_user_presence_pending() && sent_len == 11 &&
          memcmp(sent_response + 8, "\x2D\x90\x00", 3) == 0);
    queue_event(CANOKEY_NFC_EVENT_FIELD_OFF, 0, 0); canokey_nfc_loop();
    CHECK(!canokey_nfc_is_active() && !canokey_nfc_user_presence_pending());

    __wrap_CTAPHID_Loop(0); CHECK(real_hid_loops == 1);

    /* Leaving NFC+BT cancels local state, sends one idempotent disable and
     * stops the high-frequency POLL loop until the mode is enabled again. */
    session++; event_id = 0;
    queue_event(CANOKEY_NFC_EVENT_FIELD_ON, 0, 0); canokey_nfc_loop();
    queue_event(CANOKEY_NFC_EVENT_APDU, select, sizeof(select)); canokey_nfc_loop();
    queue_event(CANOKEY_NFC_EVENT_APDU, msg, sizeof(msg)); canokey_nfc_loop();
    CHECK(canokey_nfc_is_active() && canokey_nfc_user_presence_pending());
    unsigned polls_before_disable = polls;
    canokey_nfc_set_enabled(false);
    canokey_nfc_loop();
    CHECK(!canokey_nfc_is_enabled() && !canokey_nfc_is_active() &&
          !canokey_nfc_user_presence_pending() && configuration_disables == 1);
    canokey_nfc_loop();
    CHECK(polls == polls_before_disable);

    canokey_nfc_set_enabled(true);
    canokey_nfc_loop();
    CHECK(canokey_nfc_is_enabled() && configuration_enables >= 2);

    session++; event_id = 0;
    queue_event(CANOKEY_NFC_EVENT_FIELD_ON, 0, 0); canokey_nfc_loop();
    unsigned before = dispatches;
    queue_event(CANOKEY_NFC_EVENT_APDU, poll, sizeof(poll)); canokey_nfc_loop();
    CHECK(dispatches == before && sent_len == 10 && sent_response[8] == 0x69 && sent_response[9] == 0x85);
    queue_event(CANOKEY_NFC_EVENT_APDU, select, sizeof(select)); canokey_nfc_loop();
    const uint8_t admin[] = {0, 0x20, 0, 0};
    before = dispatches;
    queue_event(CANOKEY_NFC_EVENT_APDU, admin, sizeof(admin)); canokey_nfc_loop();
    CHECK(dispatches == before && sent_response[8] == 0x6D);

    poll_timeout = true; tick += 2001;
    canokey_nfc_loop(); CHECK(!canokey_nfc_is_active() && resets == 2);
    poll_timeout = false; unsupported = true; tick += 5000;
    canokey_nfc_loop(); before = configurations;
    tick += 5000; canokey_nfc_loop(); CHECK(configurations == before);
    return 0;
}

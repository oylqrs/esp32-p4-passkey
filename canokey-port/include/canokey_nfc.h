#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/* Private Chameleon extension, not commands supported by stock firmware. */
#define CANOKEY_NFC_PROTOCOL_VERSION 1
#define CANOKEY_NFC_CMD_CONFIGURE 0x7F00
#define CANOKEY_NFC_CMD_POLL 0x7F01
#define CANOKEY_NFC_CMD_RESPONSE 0x7F02
#define CANOKEY_NFC_CMD_RESET 0x7F03
#define CANOKEY_NFC_STATUS_SUCCESS 0x0068
#define CANOKEY_NFC_WAIT_ENTRY 2

typedef enum {
    CANOKEY_NFC_EVENT_NONE = 0,
    CANOKEY_NFC_EVENT_FIELD_ON = 1,
    CANOKEY_NFC_EVENT_FIELD_OFF = 2,
    CANOKEY_NFC_EVENT_APDU = 3,
} canokey_nfc_event_t;

typedef esp_err_t (*canokey_nfc_transceive_t)(
    uint16_t command, const uint8_t *request, uint16_t request_len,
    uint8_t *response, uint16_t response_capacity, uint16_t *response_len,
    uint16_t *status, uint32_t timeout_ms);

/* Bind before canokey_esp32p4_start(); processing starts only after device unlock. */
esp_err_t canokey_nfc_bind_transport(canokey_nfc_transceive_t transceive);
void canokey_nfc_set_enabled(bool enabled);
bool canokey_nfc_is_enabled(void);
bool canokey_nfc_is_active(void);
bool canokey_nfc_user_presence_pending(void);
uint8_t canokey_nfc_current_cbor_cmd(void);
uint32_t canokey_nfc_presence_token(void);
bool canokey_nfc_submit_user_presence(uint32_t token, uint8_t result);

/* Called only by the CanoKey task, with the port's core mutex held. */
void canokey_nfc_loop(void);
void canokey_nfc_presence_requested(void);

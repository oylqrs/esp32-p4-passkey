#pragma once
#include <stdint.h>
#include <stdbool.h>
#define APDU_BUFFER_SIZE 256
#define APDU_COMMAND_BUFFER_SIZE 288
#define APDU_INCOMING_DATA_SIZE 288
#define SW_NO_ERROR 0x9000
#define SW_WRONG_LENGTH 0x6700
#define SW_UNABLE_TO_PROCESS 0x6900
#define SW_CONDITIONS_NOT_SATISFIED 0x6985
#define SW_FILE_NOT_FOUND 0x6A82
#define SW_INS_NOT_SUPPORTED 0x6D00
enum {BUFFER_OWNER_NONE, BUFFER_OWNER_CCID, BUFFER_OWNER_WEBUSB, BUFFER_OWNER_USBD, BUFFER_OWNER_CTAPHID};
typedef struct {
    uint8_t *data;
    uint8_t cla, ins, p1, p2;
    uint32_t le;
    uint16_t lc;
    uint8_t extended;
} CAPDU;
typedef struct {uint8_t *data; uint16_t len, sw;} RAPDU;
typedef enum {APDU_TRANSPORT_CCID, APDU_TRANSPORT_WEBUSB, APDU_TRANSPORT_NFC, APDU_TRANSPORT_HID} apdu_transport_t;
int build_capdu(CAPDU *capdu, const uint8_t *command, uint16_t length);
int acquire_apdu_buffer(uint8_t owner);
int release_apdu_buffer(uint8_t owner);
int apdu_session_can_preempt(void);
void init_apdu_buffer(void);
void apdu_response_source_clear(void);
void apdu_fido_chain_reset(void);
void process_apdu_from(CAPDU *capdu, RAPDU *rapdu, apdu_transport_t transport);

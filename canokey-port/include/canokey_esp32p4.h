#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

esp_err_t canokey_esp32p4_start(void);
bool canokey_esp32p4_is_running(void);
bool canokey_esp32p4_is_ready(void);
void canokey_esp32p4_set_user_presence_request_callback(void (*callback)(uint8_t entry));
uint8_t canokey_esp32p4_get_current_ctap_cbor_cmd(void);
esp_err_t canokey_esp32p4_crypto_init(void);
esp_err_t canokey_esp32p4_crypto_selftest(void);
esp_err_t canokey_esp32p4_flash_init(void);
void canokey_esp32p4_ctaphid_usb_init(void);
uint8_t canokey_esp32p4_ctaphid_queue_out_report(const uint8_t *report, uint16_t len);
uint8_t canokey_esp32p4_ctaphid_queue_data_in(void);
esp_err_t canokey_esp32p4_ccid_power_on(void);
void canokey_esp32p4_ccid_power_off(void);
int canokey_esp32p4_process_ccid_apdu(const uint8_t *command, uint16_t command_len,
                                      uint8_t *response, uint16_t response_size,
                                      uint16_t *response_len, uint16_t *status_word);
esp_err_t canokey_esp32p4_openpgp_probe(void);

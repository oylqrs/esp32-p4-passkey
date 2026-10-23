#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CHAMELEON_BLE_HID_REPORT_SIZE 8

typedef struct {
    bool enabled;
    bool pairing_allowed;
    bool connected;
    bool encrypted;
    bool bonded;
    bool notifications_enabled;
    bool suspended;
    uint8_t protocol_mode;
    uint8_t led_state;
    uint8_t queued_reports;
} chameleon_ble_hid_status_t;

esp_err_t chameleon_ble_hid_init(void);
void chameleon_ble_hid_set_mode(bool enabled, bool pairing_allowed);
bool chameleon_ble_hid_is_ready(void);
void chameleon_ble_hid_get_cached_status(chameleon_ble_hid_status_t *status);

esp_err_t chameleon_ble_hid_type_text(const char *text);
esp_err_t chameleon_ble_hid_report_press(uint8_t modifier, uint8_t key);
esp_err_t chameleon_ble_hid_key_release(void);
esp_err_t chameleon_ble_hid_delay(uint32_t delay_ms);
esp_err_t chameleon_ble_hid_wait_idle(void);

#ifdef __cplusplus
}
#endif

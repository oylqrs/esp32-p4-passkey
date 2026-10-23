#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "hid_keyboard.h"

typedef enum {
    SETUP_LOCK_CLOCK_HM_YMD = 0,
    SETUP_LOCK_CLOCK_HM_MD,
    SETUP_LOCK_CLOCK_HM,
    SETUP_LOCK_CLOCK_OFF,
    SETUP_LOCK_CLOCK_MODE_COUNT,
} setup_lock_clock_mode_t;

typedef enum {
    SETUP_SECURITY_MODE_NORMAL = 0,
    SETUP_SECURITY_MODE_EMERGENCY,
    SETUP_SECURITY_MODE_DURESS,
    SETUP_SECURITY_MODE_COUNT,
} setup_security_mode_t;

typedef enum {
    SETUP_NRF_MODE_OFF = 0,
    SETUP_NRF_MODE_NFC_BT,
    SETUP_NRF_MODE_CHAMELEON,
    SETUP_NRF_MODE_BT_PAIRING,
    SETUP_NRF_MODE_UPGRADE,
    SETUP_NRF_MODE_COUNT,
} setup_nrf_mode_t;

bool setup_ui_device_password_matches(const char *pin);
setup_lock_clock_mode_t setup_ui_get_lock_clock_mode(void);
setup_security_mode_t setup_ui_get_security_mode(void);
uint8_t setup_ui_get_pin_error_limit(void);
bool setup_ui_get_pin_masking(void);
bool setup_ui_get_pin_scramble(void);
bool setup_ui_get_hide_password(void);
bool setup_ui_get_custom_background(void);
setup_nrf_mode_t setup_ui_get_nrf_mode(void);
hid_keyboard_transport_t setup_ui_get_hid_transport(void);
void setup_ui_show(void);

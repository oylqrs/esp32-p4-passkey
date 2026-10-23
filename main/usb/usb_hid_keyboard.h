#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "keyboard_layout.h"
#include "usb_msc.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t usb_hid_keyboard_init(void);
esp_err_t usb_hid_enable_smartcard_reader(void);
esp_err_t usb_hid_disable_smartcard_reader(void);
esp_err_t usb_hid_keyboard_type_text(const char *text);
esp_err_t usb_hid_keyboard_key_press(uint8_t key);
esp_err_t usb_hid_keyboard_report_press(uint8_t modifier, uint8_t key);
esp_err_t usb_hid_keyboard_key_release(void);
esp_err_t usb_hid_keyboard_delay(uint32_t delay_ms);
esp_err_t usb_hid_keyboard_wait_idle(void);
bool usb_hid_keyboard_is_ready(void);
esp_err_t usb_hid_keyboard_set_layout(hid_keyboard_layout_t layout);
hid_keyboard_layout_t usb_hid_keyboard_get_layout(void);

#ifdef __cplusplus
}
#endif

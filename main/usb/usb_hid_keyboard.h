#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "usb_msc.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t usb_hid_keyboard_init(void);
esp_err_t usb_hid_enable_smartcard_reader(void);
esp_err_t usb_hid_disable_smartcard_reader(void);
esp_err_t usb_hid_keyboard_type_text(const char *text);
bool usb_hid_keyboard_is_ready(void);

#ifdef __cplusplus
}
#endif

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    HID_KEYBOARD_TRANSPORT_USB = 0,
    HID_KEYBOARD_TRANSPORT_BLUETOOTH,
    HID_KEYBOARD_TRANSPORT_AUTO,
    HID_KEYBOARD_TRANSPORT_COUNT,
} hid_keyboard_transport_t;

void hid_key_press(uint8_t key);
void hid_key_release(void);
void hid_type_string(char *str);

esp_err_t hid_type_string_async(const char *str);
esp_err_t hid_key_report_async(uint8_t modifier, uint8_t key);
esp_err_t hid_key_release_async(void);
esp_err_t hid_delay_async(uint32_t delay_ms);
esp_err_t hid_keyboard_wait_idle(void);
bool hid_keyboard_is_ready(void);
esp_err_t hid_keyboard_set_transport(hid_keyboard_transport_t transport);
hid_keyboard_transport_t hid_keyboard_get_transport(void);

#ifdef __cplusplus
}
#endif

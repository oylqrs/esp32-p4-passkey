#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    HID_LAYOUT_US = 0,
    HID_LAYOUT_UK,
    HID_LAYOUT_GERMAN,
    HID_LAYOUT_FRENCH,
    HID_LAYOUT_SPANISH,
    HID_LAYOUT_ITALIAN,
    HID_LAYOUT_PORTUGUESE,
    HID_LAYOUT_COUNT,
} hid_keyboard_layout_t;

bool keyboard_layout_char_for(hid_keyboard_layout_t layout,
                              char character,
                              uint8_t *modifier,
                              uint8_t *key);
bool keyboard_layout_char(char character, uint8_t *modifier, uint8_t *key);
bool keyboard_layout_named_key(const char *name, uint8_t *key);
const char *keyboard_layout_name(hid_keyboard_layout_t layout);
void keyboard_layout_set(hid_keyboard_layout_t layout);
hid_keyboard_layout_t keyboard_layout_get(void);

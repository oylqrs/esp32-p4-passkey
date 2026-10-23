#include "keyboard_layout.h"

#include <ctype.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* USB HID usage codes for the 104/105-key keyboard positions. */
#define HID_KEY_A                 0x04
#define HID_KEY_Z                 0x1D
#define HID_KEY_1                 0x1E
#define HID_KEY_2                 0x1F
#define HID_KEY_3                 0x20
#define HID_KEY_4                 0x21
#define HID_KEY_5                 0x22
#define HID_KEY_6                 0x23
#define HID_KEY_7                 0x24
#define HID_KEY_8                 0x25
#define HID_KEY_9                 0x26
#define HID_KEY_0                 0x27
#define HID_KEY_ENTER             0x28
#define HID_KEY_TAB               0x2B
#define HID_KEY_SPACE             0x2C
#define HID_KEY_MINUS             0x2D
#define HID_KEY_EQUAL             0x2E
#define HID_KEY_BRACKET_LEFT      0x2F
#define HID_KEY_BRACKET_RIGHT     0x30
#define HID_KEY_BACKSLASH         0x31
#define HID_KEY_EUROPE_1          0x32
#define HID_KEY_SEMICOLON         0x33
#define HID_KEY_APOSTROPHE        0x34
#define HID_KEY_GRAVE             0x35
#define HID_KEY_COMMA             0x36
#define HID_KEY_PERIOD            0x37
#define HID_KEY_SLASH             0x38
#define HID_KEY_KEYPAD_SUBTRACT   0x56

#define HID_MOD_SHIFT             0x02
#define HID_MOD_RIGHT_ALT         0x40

typedef struct {
    const char *name;
    uint8_t key;
} named_key_t;

static _Atomic uint8_t s_current_layout = HID_LAYOUT_US;

static bool set_mapping(uint8_t modifier,
                        uint8_t key,
                        uint8_t *out_modifier,
                        uint8_t *out_key)
{
    *out_modifier = modifier;
    *out_key = key;
    return true;
}

static bool map_us(char character, uint8_t *modifier, uint8_t *key)
{
    *modifier = 0;
    *key = 0;

    if (character >= 'a' && character <= 'z') {
        *key = HID_KEY_A + (uint8_t)(character - 'a');
        return true;
    }
    if (character >= 'A' && character <= 'Z') {
        *modifier = HID_MOD_SHIFT;
        *key = HID_KEY_A + (uint8_t)(character - 'A');
        return true;
    }
    if (character >= '1' && character <= '9') {
        *key = HID_KEY_1 + (uint8_t)(character - '1');
        return true;
    }
    if (character == '0') {
        *key = HID_KEY_0;
        return true;
    }

    switch (character) {
    case '\n':
    case '\r': return set_mapping(0, HID_KEY_ENTER, modifier, key);
    case '\t': return set_mapping(0, HID_KEY_TAB, modifier, key);
    case ' ': return set_mapping(0, HID_KEY_SPACE, modifier, key);
    case '-': return set_mapping(0, HID_KEY_MINUS, modifier, key);
    case '_': return set_mapping(HID_MOD_SHIFT, HID_KEY_MINUS, modifier, key);
    case '=': return set_mapping(0, HID_KEY_EQUAL, modifier, key);
    case '+': return set_mapping(HID_MOD_SHIFT, HID_KEY_EQUAL, modifier, key);
    case '[': return set_mapping(0, HID_KEY_BRACKET_LEFT, modifier, key);
    case '{': return set_mapping(HID_MOD_SHIFT, HID_KEY_BRACKET_LEFT, modifier, key);
    case ']': return set_mapping(0, HID_KEY_BRACKET_RIGHT, modifier, key);
    case '}': return set_mapping(HID_MOD_SHIFT, HID_KEY_BRACKET_RIGHT, modifier, key);
    case '\\': return set_mapping(0, HID_KEY_BACKSLASH, modifier, key);
    case '|': return set_mapping(HID_MOD_SHIFT, HID_KEY_BACKSLASH, modifier, key);
    case ';': return set_mapping(0, HID_KEY_SEMICOLON, modifier, key);
    case ':': return set_mapping(HID_MOD_SHIFT, HID_KEY_SEMICOLON, modifier, key);
    case '\'': return set_mapping(0, HID_KEY_APOSTROPHE, modifier, key);
    case '"': return set_mapping(HID_MOD_SHIFT, HID_KEY_APOSTROPHE, modifier, key);
    case '`': return set_mapping(0, HID_KEY_GRAVE, modifier, key);
    case '~': return set_mapping(HID_MOD_SHIFT, HID_KEY_GRAVE, modifier, key);
    case ',': return set_mapping(0, HID_KEY_COMMA, modifier, key);
    case '<': return set_mapping(HID_MOD_SHIFT, HID_KEY_COMMA, modifier, key);
    case '.': return set_mapping(0, HID_KEY_PERIOD, modifier, key);
    case '>': return set_mapping(HID_MOD_SHIFT, HID_KEY_PERIOD, modifier, key);
    case '/': return set_mapping(0, HID_KEY_SLASH, modifier, key);
    case '?': return set_mapping(HID_MOD_SHIFT, HID_KEY_SLASH, modifier, key);
    case '!': return set_mapping(HID_MOD_SHIFT, HID_KEY_1, modifier, key);
    case '@': return set_mapping(HID_MOD_SHIFT, HID_KEY_2, modifier, key);
    case '#': return set_mapping(HID_MOD_SHIFT, HID_KEY_3, modifier, key);
    case '$': return set_mapping(HID_MOD_SHIFT, HID_KEY_4, modifier, key);
    case '%': return set_mapping(HID_MOD_SHIFT, HID_KEY_5, modifier, key);
    case '^': return set_mapping(HID_MOD_SHIFT, HID_KEY_6, modifier, key);
    case '&': return set_mapping(HID_MOD_SHIFT, HID_KEY_7, modifier, key);
    case '*': return set_mapping(HID_MOD_SHIFT, HID_KEY_8, modifier, key);
    case '(': return set_mapping(HID_MOD_SHIFT, HID_KEY_9, modifier, key);
    case ')': return set_mapping(HID_MOD_SHIFT, HID_KEY_0, modifier, key);
    default: return false;
    }
}

static void remap_letters(hid_keyboard_layout_t layout,
                          char character,
                          uint8_t *key)
{
    const bool upper = character >= 'A' && character <= 'Z';
    const char lower = upper ? (char)(character - 'A' + 'a') : character;

    if (layout == HID_LAYOUT_GERMAN && (lower == 'y' || lower == 'z')) {
        *key = lower == 'y' ? HID_KEY_Z : HID_KEY_A + ('y' - 'a');
    } else if (layout == HID_LAYOUT_FRENCH) {
        switch (lower) {
        case 'a': *key = HID_KEY_A + ('q' - 'a'); break;
        case 'q': *key = HID_KEY_A; break;
        case 'w': *key = HID_KEY_Z; break;
        case 'z': *key = HID_KEY_A + ('w' - 'a'); break;
        default: break;
        }
    }
}

static bool map_layout_symbols(hid_keyboard_layout_t layout,
                               char character,
                               uint8_t *modifier,
                               uint8_t *key)
{
    switch (layout) {
    case HID_LAYOUT_UK:
        switch (character) {
        case '@': return set_mapping(0, HID_KEY_APOSTROPHE, modifier, key);
        case '"': return set_mapping(HID_MOD_SHIFT, HID_KEY_2, modifier, key);
        case '#': return set_mapping(0, HID_KEY_BACKSLASH, modifier, key);
        case '~': return set_mapping(HID_MOD_SHIFT, HID_KEY_BACKSLASH, modifier, key);
        case '\\': return set_mapping(0, HID_KEY_EUROPE_1, modifier, key);
        case '|': return set_mapping(HID_MOD_SHIFT, HID_KEY_EUROPE_1, modifier, key);
        default: return false;
        }

    case HID_LAYOUT_GERMAN:
        switch (character) {
        case '@': return set_mapping(HID_MOD_RIGHT_ALT, HID_KEY_A + ('q' - 'a'), modifier, key);
        case '#': return set_mapping(0, HID_KEY_BACKSLASH, modifier, key);
        case '\'': return set_mapping(HID_MOD_SHIFT, HID_KEY_BACKSLASH, modifier, key);
        case '+': return set_mapping(0, HID_KEY_BRACKET_RIGHT, modifier, key);
        case '*': return set_mapping(HID_MOD_SHIFT, HID_KEY_BRACKET_RIGHT, modifier, key);
        case '=': return set_mapping(HID_MOD_SHIFT, HID_KEY_0, modifier, key);
        case '-': return set_mapping(0, HID_KEY_KEYPAD_SUBTRACT, modifier, key);
        case '_': return set_mapping(HID_MOD_SHIFT, HID_KEY_KEYPAD_SUBTRACT, modifier, key);
        case '[': return set_mapping(HID_MOD_RIGHT_ALT, HID_KEY_8, modifier, key);
        case ']': return set_mapping(HID_MOD_RIGHT_ALT, HID_KEY_9, modifier, key);
        case '{': return set_mapping(HID_MOD_RIGHT_ALT, HID_KEY_7, modifier, key);
        case '}': return set_mapping(HID_MOD_RIGHT_ALT, HID_KEY_0, modifier, key);
        case '\\': return set_mapping(HID_MOD_RIGHT_ALT, HID_KEY_MINUS, modifier, key);
        case '|': return set_mapping(HID_MOD_RIGHT_ALT | HID_MOD_SHIFT, HID_KEY_MINUS, modifier, key);
        case '^': return set_mapping(0, HID_KEY_GRAVE, modifier, key);
        default: return false;
        }

    case HID_LAYOUT_FRENCH:
        if (character >= '1' && character <= '9') {
            return set_mapping(HID_MOD_SHIFT,
                               HID_KEY_1 + (uint8_t)(character - '1'),
                               modifier, key);
        }
        if (character == '0') {
            return set_mapping(HID_MOD_SHIFT, HID_KEY_0, modifier, key);
        }
        switch (character) {
        case '&': return set_mapping(0, HID_KEY_1, modifier, key);
        case '"': return set_mapping(0, HID_KEY_3, modifier, key);
        case '\'': return set_mapping(0, HID_KEY_4, modifier, key);
        case '(': return set_mapping(0, HID_KEY_5, modifier, key);
        case ')': return set_mapping(0, HID_KEY_0, modifier, key);
        case '-': return set_mapping(0, HID_KEY_6, modifier, key);
        case '_': return set_mapping(0, HID_KEY_8, modifier, key);
        case '+': return set_mapping(HID_MOD_SHIFT, HID_KEY_EQUAL, modifier, key);
        case '@': return set_mapping(HID_MOD_RIGHT_ALT, HID_KEY_0, modifier, key);
        case '#': return set_mapping(HID_MOD_RIGHT_ALT, HID_KEY_3, modifier, key);
        case '[': return set_mapping(HID_MOD_RIGHT_ALT, HID_KEY_5, modifier, key);
        case ']': return set_mapping(HID_MOD_RIGHT_ALT, HID_KEY_MINUS, modifier, key);
        case '{': return set_mapping(HID_MOD_RIGHT_ALT, HID_KEY_4, modifier, key);
        case '}': return set_mapping(HID_MOD_RIGHT_ALT, HID_KEY_EQUAL, modifier, key);
        case '\\': return set_mapping(HID_MOD_RIGHT_ALT, HID_KEY_8, modifier, key);
        case '|': return set_mapping(HID_MOD_RIGHT_ALT, HID_KEY_6, modifier, key);
        default: return false;
        }

    case HID_LAYOUT_SPANISH:
        switch (character) {
        case '@': return set_mapping(HID_MOD_RIGHT_ALT, HID_KEY_2, modifier, key);
        case '#': return set_mapping(HID_MOD_RIGHT_ALT, HID_KEY_3, modifier, key);
        case '=': return set_mapping(0, HID_KEY_0, modifier, key);
        case '+': return set_mapping(0, HID_KEY_BRACKET_RIGHT, modifier, key);
        case '*': return set_mapping(HID_MOD_SHIFT, HID_KEY_BRACKET_RIGHT, modifier, key);
        case '\'': return set_mapping(0, HID_KEY_MINUS, modifier, key);
        case '?': return set_mapping(HID_MOD_SHIFT, HID_KEY_MINUS, modifier, key);
        case '[': return set_mapping(HID_MOD_RIGHT_ALT, HID_KEY_BRACKET_LEFT, modifier, key);
        case ']': return set_mapping(HID_MOD_RIGHT_ALT, HID_KEY_BRACKET_RIGHT, modifier, key);
        case '{': return set_mapping(HID_MOD_RIGHT_ALT | HID_MOD_SHIFT, HID_KEY_BRACKET_LEFT, modifier, key);
        case '}': return set_mapping(HID_MOD_RIGHT_ALT | HID_MOD_SHIFT, HID_KEY_BRACKET_RIGHT, modifier, key);
        default: return false;
        }

    case HID_LAYOUT_ITALIAN:
        switch (character) {
        case '@': return set_mapping(HID_MOD_RIGHT_ALT, HID_KEY_SEMICOLON, modifier, key);
        case '#': return set_mapping(HID_MOD_RIGHT_ALT, HID_KEY_APOSTROPHE, modifier, key);
        case '[': return set_mapping(HID_MOD_RIGHT_ALT, HID_KEY_MINUS, modifier, key);
        case ']': return set_mapping(HID_MOD_RIGHT_ALT, HID_KEY_EQUAL, modifier, key);
        case '{': return set_mapping(HID_MOD_RIGHT_ALT | HID_MOD_SHIFT, HID_KEY_MINUS, modifier, key);
        case '}': return set_mapping(HID_MOD_RIGHT_ALT | HID_MOD_SHIFT, HID_KEY_EQUAL, modifier, key);
        case '\\': return set_mapping(HID_MOD_RIGHT_ALT, HID_KEY_GRAVE, modifier, key);
        case '|': return set_mapping(HID_MOD_RIGHT_ALT | HID_MOD_SHIFT, HID_KEY_GRAVE, modifier, key);
        case '=': return set_mapping(0, HID_KEY_EQUAL, modifier, key);
        case '+': return set_mapping(HID_MOD_SHIFT, HID_KEY_EQUAL, modifier, key);
        default: return false;
        }

    case HID_LAYOUT_PORTUGUESE:
        switch (character) {
        case '@': return set_mapping(HID_MOD_RIGHT_ALT, HID_KEY_2, modifier, key);
        case '=': return set_mapping(0, HID_KEY_0, modifier, key);
        case '+': return set_mapping(0, HID_KEY_BRACKET_RIGHT, modifier, key);
        case '*': return set_mapping(HID_MOD_SHIFT, HID_KEY_BRACKET_RIGHT, modifier, key);
        case '[': return set_mapping(HID_MOD_RIGHT_ALT, HID_KEY_8, modifier, key);
        case ']': return set_mapping(HID_MOD_RIGHT_ALT, HID_KEY_9, modifier, key);
        case '{': return set_mapping(HID_MOD_RIGHT_ALT | HID_MOD_SHIFT, HID_KEY_8, modifier, key);
        case '}': return set_mapping(HID_MOD_RIGHT_ALT | HID_MOD_SHIFT, HID_KEY_9, modifier, key);
        case '\\': return set_mapping(HID_MOD_RIGHT_ALT, HID_KEY_EUROPE_1, modifier, key);
        case '|': return set_mapping(HID_MOD_RIGHT_ALT | HID_MOD_SHIFT, HID_KEY_EUROPE_1, modifier, key);
        default: return false;
        }

    case HID_LAYOUT_US:
    default:
        return false;
    }
}

bool keyboard_layout_char_for(hid_keyboard_layout_t layout,
                              char character,
                              uint8_t *modifier,
                              uint8_t *key)
{
    if (!modifier || !key || (unsigned int)layout >= HID_LAYOUT_COUNT) {
        return false;
    }

    if (!map_us(character, modifier, key)) {
        return false;
    }

    if (isalpha((unsigned char)character)) {
        remap_letters(layout, character, key);
    }

    if (map_layout_symbols(layout, character, modifier, key)) {
        return true;
    }
    return true;
}

bool keyboard_layout_char(char character, uint8_t *modifier, uint8_t *key)
{
    return keyboard_layout_char_for(keyboard_layout_get(), character, modifier, key);
}

bool keyboard_layout_named_key(const char *name, uint8_t *key)
{
    static const named_key_t keys[] = {
        {"ENTER", 0x28}, {"RETURN", 0x28}, {"ESC", 0x29}, {"ESCAPE", 0x29},
        {"BACKSPACE", 0x2A}, {"TAB", 0x2B}, {"SPACE", 0x2C}, {"CAPSLOCK", 0x39},
        {"PRINTSCREEN", 0x46}, {"SCROLLLOCK", 0x47}, {"PAUSE", 0x48},
        {"INSERT", 0x49}, {"HOME", 0x4A}, {"PAGEUP", 0x4B}, {"DELETE", 0x4C},
        {"END", 0x4D}, {"PAGEDOWN", 0x4E}, {"RIGHT", 0x4F}, {"RIGHTARROW", 0x4F},
        {"LEFT", 0x50}, {"LEFTARROW", 0x50}, {"DOWN", 0x51}, {"DOWNARROW", 0x51},
        {"UP", 0x52}, {"UPARROW", 0x52}, {"NUMLOCK", 0x53}, {"MENU", 0x65},
    };

    if (!name || !key) {
        return false;
    }
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        if (strcasecmp(name, keys[i].name) == 0) {
            *key = keys[i].key;
            return true;
        }
    }
    if ((name[0] == 'F' || name[0] == 'f') && isdigit((unsigned char)name[1])) {
        char *end;
        long number = strtol(name + 1, &end, 10);
        if (*end == '\0' && number >= 1 && number <= 12) {
            *key = 0x3A + (uint8_t)(number - 1);
            return true;
        }
    }

    uint8_t modifier;
    return name[0] != '\0' && name[1] == '\0' &&
           keyboard_layout_char(name[0], &modifier, key);
}

const char *keyboard_layout_name(hid_keyboard_layout_t layout)
{
    static const char *const names[HID_LAYOUT_COUNT] = {
        "US", "UK", "German DE", "French FR", "Spanish ES", "Italian IT",
        "Portuguese PT-PT",
    };
    return (unsigned int)layout < HID_LAYOUT_COUNT ? names[layout] : names[HID_LAYOUT_US];
}

void keyboard_layout_set(hid_keyboard_layout_t layout)
{
    if ((unsigned int)layout >= HID_LAYOUT_COUNT) {
        layout = HID_LAYOUT_US;
    }
    atomic_store_explicit(&s_current_layout, (uint8_t)layout, memory_order_release);
}

hid_keyboard_layout_t keyboard_layout_get(void)
{
    uint8_t layout = atomic_load_explicit(&s_current_layout, memory_order_acquire);
    return (unsigned int)layout < HID_LAYOUT_COUNT ? (hid_keyboard_layout_t)layout : HID_LAYOUT_US;
}

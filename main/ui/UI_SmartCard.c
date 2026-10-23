#include "UI_SmartCard.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "admin.h"
#include "canokey_esp32p4.h"
#include "canokey_gpg_slots.h"
#include "canokey_piv_slots.h"
#include "ctap.h"
#include "device-config.h"
#include "esp_err.h"
#include "esp_log.h"
#include "fs.h"
#include "lvgl.h"
#include "nvs.h"
#include "openpgp.h"
#include "piv.h"
#include "ui/i18n/ui_i18n.h"
#include "ui_nav.h"
#include "ui_status_bar.h"
#include "usb_ccid.h"
#include "usb_hid_keyboard.h"

#define SMARTCARD_WIDTH              480
#define SMARTCARD_HEIGHT             640
#define SMARTCARD_CONTENT_WIDTH      448
#define SMARTCARD_SECTION_HEIGHT     54
#define SMARTCARD_SELECT_HEIGHT      120
#define SMARTCARD_INFO_ROW_HEIGHT    120
#define SMARTCARD_SETTING_ROW_HEIGHT 120
#define SMARTCARD_INFO_ROW_COUNT     6
#define SMARTCARD_SETTING_ROW_COUNT  5

#define SMARTCARD_NVS_NAMESPACE      "sc_ui"
#define SMARTCARD_SLOT_NVS_KEY       "selected_card"
#define SMARTCARD_OPENPGP_NVS_KEY    "openpgp_on"
#define SMARTCARD_PIV_NVS_KEY        "piv_on"

#define OPENPGP_NAME_ATTR            0x5B
#define OPENPGP_KEY_ATTR             0x00
#define OPENPGP_FINGERPRINT_LEN      20
#define OPENPGP_FINGERPRINT_TEXT_LEN (OPENPGP_FINGERPRINT_LEN * 2 + 1)
#define OPENPGP_SERIAL_LEN           4
#define OPENPGP_SERIAL_TEXT_LEN      (OPENPGP_SERIAL_LEN * 3)
#define OPENPGP_VALUE_LEN            256

typedef struct {
    char serial[OPENPGP_SERIAL_TEXT_LEN];
    char name[OPENPGP_VALUE_LEN];
    char url[OPENPGP_VALUE_LEN];
    char sig[OPENPGP_FINGERPRINT_TEXT_LEN];
    char dec[OPENPGP_FINGERPRINT_TEXT_LEN];
    char aut[OPENPGP_FINGERPRINT_TEXT_LEN];
} smartcard_openpgp_data_t;

typedef enum {
    SMARTCARD_RESET_OPENPGP = 0,
    SMARTCARD_RESET_PIV,
    SMARTCARD_RESET_FIDO,
} smartcard_reset_target_t;

static const char *TAG = "UI_SmartCard";
static uint8_t s_selected_card;
static lv_obj_t *s_reset_overlay;
static lv_obj_t *s_reset_message;
static smartcard_reset_target_t s_reset_target;

static void reload_screen_async(void *user_data);

static void set_text(char *dst, size_t dst_size, const char *text)
{
    if (!dst || dst_size == 0) {
        return;
    }
    strncpy(dst, text, dst_size - 1);
    dst[dst_size - 1] = '\0';
}

static void set_none(char *dst, size_t dst_size)
{
    set_text(dst, dst_size, ui_i18n_get(UI_STR_NONE));
}

static uint8_t read_setting(const char *key, uint8_t default_value)
{
    nvs_handle_t nvs;
    if (nvs_open(SMARTCARD_NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        return default_value;
    }

    uint8_t value = default_value;
    (void)nvs_get_u8(nvs, key, &value);
    nvs_close(nvs);
    return value;
}

static esp_err_t write_setting(const char *key, uint8_t value)
{
    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(SMARTCARD_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = nvs_set_u8(nvs, key, value);
    if (ret == ESP_OK) {
        ret = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return ret;
}

static void format_hex(const uint8_t *src, size_t src_len, char *dst, size_t dst_size)
{
    static const char hex[] = "0123456789ABCDEF";
    if (!src || !dst || dst_size < src_len * 2 + 1) {
        return;
    }

    for (size_t i = 0; i < src_len; i++) {
        dst[i * 2] = hex[src[i] >> 4];
        dst[i * 2 + 1] = hex[src[i] & 0x0F];
    }
    dst[src_len * 2] = '\0';
}

static void format_serial(const uint8_t *src, char *dst, size_t dst_size)
{
    static const char hex[] = "0123456789ABCDEF";
    if (!src || !dst || dst_size < OPENPGP_SERIAL_TEXT_LEN) {
        return;
    }

    for (size_t i = 0; i < OPENPGP_SERIAL_LEN; i++) {
        size_t out = i * 3;
        dst[out] = hex[src[i] >> 4];
        dst[out + 1] = hex[src[i] & 0x0F];
        dst[out + 2] = i + 1 < OPENPGP_SERIAL_LEN ? ' ' : '\0';
    }
}

static void read_text_attr(const char *path, uint8_t attr, char *dst, size_t dst_size)
{
    int len = read_attr(path, attr, dst, (lfs_size_t)(dst_size - 1));
    if (len <= 0) {
        set_none(dst, dst_size);
        return;
    }
    if ((size_t)len >= dst_size) {
        len = (int)dst_size - 1;
    }
    dst[len] = '\0';
}

static void read_text_file(const char *path, char *dst, size_t dst_size)
{
    int len = read_file(path, dst, 0, (lfs_size_t)(dst_size - 1));
    if (len <= 0) {
        set_none(dst, dst_size);
        return;
    }
    if ((size_t)len >= dst_size) {
        len = (int)dst_size - 1;
    }
    dst[len] = '\0';
}

static void read_fingerprint(const char *path, char *dst, size_t dst_size)
{
    uint8_t value[OPENPGP_FINGERPRINT_LEN] = {0};
    int len = read_attr(path, OPENPGP_KEY_ATTR, value, sizeof(value));
    if (len != OPENPGP_FINGERPRINT_LEN) {
        set_none(dst, dst_size);
        return;
    }

    bool all_zero = true;
    for (size_t i = 0; i < sizeof(value); i++) {
        if (value[i] != 0) {
            all_zero = false;
            break;
        }
    }
    if (all_zero) {
        set_none(dst, dst_size);
        return;
    }
    format_hex(value, sizeof(value), dst, dst_size);
}

static void load_openpgp_data(smartcard_openpgp_data_t *data)
{
    memset(data, 0, sizeof(*data));
    if (!canokey_esp32p4_is_ready()) {
        set_none(data->serial, sizeof(data->serial));
        set_none(data->name, sizeof(data->name));
        set_none(data->url, sizeof(data->url));
        set_none(data->sig, sizeof(data->sig));
        set_none(data->dec, sizeof(data->dec));
        set_none(data->aut, sizeof(data->aut));
        return;
    }

    uint8_t serial[OPENPGP_SERIAL_LEN] = {0};
    if (canokey_gpg_get_slot() > 0) {
        serial[3] = canokey_gpg_get_slot();
        format_serial(serial, data->serial, sizeof(data->serial));
    } else if (admin_platform_serial_read(serial) >= 0) {
        format_serial(serial, data->serial, sizeof(data->serial));
    } else {
        set_text(data->serial, sizeof(data->serial), "00 00 00 00");
    }

    read_text_attr("pgp-data", OPENPGP_NAME_ATTR, data->name, sizeof(data->name));
    read_text_file("pgp-data", data->url, sizeof(data->url));
    read_fingerprint("pgp-sigk", data->sig, sizeof(data->sig));
    read_fingerprint("pgp-deck", data->dec, sizeof(data->dec));
    read_fingerprint("pgp-autk", data->aut, sizeof(data->aut));
}

static esp_err_t select_card(uint8_t slot)
{
    if (!canokey_esp32p4_is_ready()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (slot >= CANOKEY_GPG_SLOT_COUNT || slot >= CANOKEY_PIV_SLOT_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }

    usb_ccid_deactivate_card();
    if (canokey_gpg_select_slot(slot) < 0 || canokey_piv_select_slot(slot) < 0) {
        return ESP_FAIL;
    }
    if (openpgp_install(0) < 0 || piv_install(0) < 0) {
        return ESP_FAIL;
    }

    if (read_setting(SMARTCARD_OPENPGP_NVS_KEY, 1) != 0 ||
        read_setting(SMARTCARD_PIV_NVS_KEY, 1) != 0) {
        if (!usb_ccid_activate_card()) {
            return ESP_FAIL;
        }
    }
    return ESP_OK;
}

static lv_obj_t *create_group(lv_obj_t *parent, int32_t height)
{
    lv_obj_t *group = lv_obj_create(parent);
    lv_obj_remove_style_all(group);
    lv_obj_set_size(group, SMARTCARD_CONTENT_WIDTH, height);
    lv_obj_set_style_bg_color(group, lv_color_hex(0x20232B), 0);
    lv_obj_set_style_bg_opa(group, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(group, 6, 0);
    lv_obj_set_style_pad_all(group, 0, 0);
    lv_obj_clear_flag(group, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(group, LV_OBJ_FLAG_GESTURE_BUBBLE);
    return group;
}

static void add_section_title(lv_obj_t *parent, const char *text)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_size(label, SMARTCARD_CONTENT_WIDTH, SMARTCARD_SECTION_HEIGHT);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_set_style_text_font(label, ui_i18n_font_35(), 0);
    lv_obj_set_style_pad_left(label, 4, 0);
    lv_obj_set_style_pad_top(label, 8, 0);
    lv_obj_add_flag(label, LV_OBJ_FLAG_GESTURE_BUBBLE);
}

static void slot_changed_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_VALUE_CHANGED) {
        return;
    }

    uint16_t selected = lv_dropdown_get_selected(lv_event_get_target(event));
    if (selected >= CANOKEY_GPG_SLOT_COUNT || selected == s_selected_card) {
        return;
    }

    uint8_t previous = s_selected_card;
    esp_err_t ret = write_setting(SMARTCARD_SLOT_NVS_KEY, (uint8_t)selected);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to save selected card: %s", esp_err_to_name(ret));
        lv_dropdown_set_selected(lv_event_get_target(event), previous);
        return;
    }
    s_selected_card = (uint8_t)selected;
    (void)lv_async_call(reload_screen_async, NULL);
}

static void add_card_selector(lv_obj_t *parent)
{
    lv_obj_t *group = create_group(parent, SMARTCARD_SELECT_HEIGHT);

    lv_obj_t *label = lv_label_create(group);
    lv_label_set_text(label, ui_i18n_get(UI_STR_SELECT_CARD));
    lv_obj_set_style_text_color(label, lv_color_hex(0xE5E7EC), 0);
    lv_obj_set_style_text_font(label, ui_i18n_font_32(), 0);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, 16, 0);

    lv_obj_t *dropdown = lv_dropdown_create(group);
    lv_dropdown_set_options(dropdown, "1\n2\n3\n4\n5");
    lv_dropdown_set_selected(dropdown, s_selected_card);
    lv_obj_set_size(dropdown, 132, 58);
    lv_obj_align(dropdown, LV_ALIGN_RIGHT_MID, -14, 0);
    lv_obj_set_style_bg_color(dropdown, lv_color_hex(0x2A2D36), 0);
    lv_obj_set_style_bg_opa(dropdown, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(dropdown, 0, 0);
    lv_obj_set_style_radius(dropdown, 4, 0);
    lv_obj_set_style_text_color(dropdown, lv_color_white(), 0);
    lv_obj_set_style_text_font(dropdown, ui_i18n_font_32(), 0);
    lv_obj_set_style_text_font(lv_dropdown_get_list(dropdown),
                               ui_i18n_font_32(), 0);
    lv_obj_add_event_cb(dropdown, slot_changed_event_cb,
                        LV_EVENT_VALUE_CHANGED, NULL);
}

static void add_info_row(lv_obj_t *parent,
                         const char *label_text,
                         const char *value_text)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, SMARTCARD_CONTENT_WIDTH, SMARTCARD_INFO_ROW_HEIGHT);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_pad_left(row, 16, 0);
    lv_obj_set_style_pad_right(row, 16, 0);
    lv_obj_set_style_pad_column(row, 20, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_GESTURE_BUBBLE);

    lv_obj_t *label = lv_label_create(row);
    lv_label_set_text(label, label_text);
    lv_obj_set_style_text_color(label, lv_color_hex(0xD6D8DE), 0);
    lv_obj_set_style_text_font(label, ui_i18n_font_32(), 0);
    lv_obj_add_flag(label, LV_OBJ_FLAG_GESTURE_BUBBLE);

    lv_obj_t *value = lv_label_create(row);
    lv_label_set_text(value, value_text && value_text[0]
                                 ? value_text
                                 : ui_i18n_get(UI_STR_NONE));
    lv_obj_set_width(value, 0);
    lv_obj_set_flex_grow(value, 1);
    lv_label_set_long_mode(value, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(value, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_text_color(value, lv_color_hex(0xAEB3BE), 0);
    lv_obj_set_style_text_font(value, ui_i18n_font_32(), 0);
    lv_obj_add_flag(value, LV_OBJ_FLAG_GESTURE_BUBBLE);
}

static void add_openpgp_information(lv_obj_t *parent,
                                    const smartcard_openpgp_data_t *data)
{
    lv_obj_t *group = create_group(parent,
                                   SMARTCARD_INFO_ROW_HEIGHT * SMARTCARD_INFO_ROW_COUNT);
    lv_obj_set_flex_flow(group, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(group, 0, 0);

    add_info_row(group, ui_i18n_get(UI_STR_SERIAL_NUMBER), data->serial);
    add_info_row(group, ui_i18n_get(UI_STR_NAME), data->name);
    add_info_row(group, "URL", data->url);
    add_info_row(group, "SIG", data->sig);
    add_info_row(group, "DEC", data->dec);
    add_info_row(group, "AUT", data->aut);
}

static void setting_switch_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_VALUE_CHANGED) {
        return;
    }

    const char *key = lv_event_get_user_data(event);
    lv_obj_t *sw = lv_event_get_target(event);
    uint8_t enabled = lv_obj_has_state(sw, LV_STATE_CHECKED) ? 1 : 0;
    uint8_t previous = read_setting(key, enabled ? 0 : 1);
    esp_err_t ret = write_setting(key, enabled);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to save %s: %s", key, esp_err_to_name(ret));
        return;
    }

    uint8_t openpgp_enabled = read_setting(SMARTCARD_OPENPGP_NVS_KEY, 1);
    uint8_t piv_enabled = read_setting(SMARTCARD_PIV_NVS_KEY, 1);
    if (admin_device_config_set_ccid_enabled(openpgp_enabled, piv_enabled) < 0) {
        ESP_LOGE(TAG, "Failed to update CanoKey CCID feature configuration");
        (void)write_setting(key, previous);
        if (previous) {
            lv_obj_add_state(sw, LV_STATE_CHECKED);
        } else {
            lv_obj_remove_state(sw, LV_STATE_CHECKED);
        }
        return;
    }

    usb_ccid_deactivate_card();
    bool card_enabled = openpgp_enabled != 0 || piv_enabled != 0;
    if (card_enabled) {
        (void)usb_ccid_activate_card();
    }
}

static lv_obj_t *create_setting_row(lv_obj_t *parent, const char *text)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, SMARTCARD_CONTENT_WIDTH, SMARTCARD_SETTING_ROW_HEIGHT);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_GESTURE_BUBBLE);

    lv_obj_t *label = lv_label_create(row);
    lv_label_set_text(label, text);
    lv_obj_set_width(label, 292);
    lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_color(label, lv_color_hex(0xE5E7EC), 0);
    lv_obj_set_style_text_font(label, ui_i18n_font_32(), 0);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, 16, 0);
    return row;
}

static void add_setting_switch(lv_obj_t *parent,
                               const char *text,
                               const char *key,
                               bool default_enabled)
{
    lv_obj_t *row = create_setting_row(parent, text);
    lv_obj_t *sw = lv_switch_create(row);
    lv_obj_set_size(sw, 80, 44);
    lv_obj_align(sw, LV_ALIGN_RIGHT_MID, -16, 0);
    lv_obj_set_style_bg_color(sw, lv_color_hex(0x3A3C46), LV_PART_MAIN);
    lv_obj_set_style_bg_color(sw, lv_color_hex(0x2997E8),
                              LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(sw, lv_color_white(), LV_PART_KNOB);
    if (read_setting(key, default_enabled ? 1 : 0) != 0) {
        lv_obj_add_state(sw, LV_STATE_CHECKED);
    }
    lv_obj_add_event_cb(sw, setting_switch_event_cb,
                        LV_EVENT_VALUE_CHANGED, (void *)key);
}

static const char *reset_target_name(smartcard_reset_target_t target)
{
    switch (target) {
    case SMARTCARD_RESET_OPENPGP:
        return "OpenPGP";
    case SMARTCARD_RESET_PIV:
        return "PIV";
    case SMARTCARD_RESET_FIDO:
        return "FIDO";
    default:
        return "Card";
    }
}

static void reset_dialog_deleted_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_DELETE &&
        lv_event_get_target(event) == s_reset_overlay) {
        s_reset_overlay = NULL;
        s_reset_message = NULL;
    }
}

static void close_reset_dialog(void)
{
    if (s_reset_overlay && lv_obj_is_valid(s_reset_overlay)) {
        lv_obj_delete(s_reset_overlay);
    }
}

static void reset_cancel_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_CLICKED) {
        close_reset_dialog();
    }
}

static void reload_screen_async(void *user_data)
{
    (void)user_data;
    ui_smartcard_show();
}

static void reset_confirm_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }
    if (!canokey_esp32p4_is_ready()) {
        if (s_reset_message && lv_obj_is_valid(s_reset_message)) {
            lv_label_set_text(s_reset_message,
                              ui_i18n_get(UI_STR_SMART_CARD_NOT_READY));
            lv_obj_set_style_text_color(s_reset_message,
                                        lv_color_hex(0xFF6B6B), 0);
        }
        return;
    }

    usb_ccid_deactivate_card();
    int ret = -1;
    switch (s_reset_target) {
    case SMARTCARD_RESET_OPENPGP:
        if (canokey_gpg_select_slot(s_selected_card) == 0) {
            ret = openpgp_install(1);
        }
        break;
    case SMARTCARD_RESET_PIV:
        if (canokey_piv_select_slot(s_selected_card) == 0) {
            ret = piv_install(1);
        }
        break;
    case SMARTCARD_RESET_FIDO:
        ret = ctap_install(1) == 0 ? 0 : -1;
        break;
    }

    if (ret < 0) {
        ESP_LOGE(TAG, "%s reset failed", reset_target_name(s_reset_target));
        if (s_reset_message && lv_obj_is_valid(s_reset_message)) {
            lv_label_set_text(s_reset_message,
                              ui_i18n_get(UI_STR_RESET_FAILED));
            lv_obj_set_style_text_color(s_reset_message,
                                        lv_color_hex(0xFF6B6B), 0);
        }
        return;
    }

    ESP_LOGW(TAG, "%s data reset completed", reset_target_name(s_reset_target));
    close_reset_dialog();
    (void)lv_async_call(reload_screen_async, NULL);
}

static lv_obj_t *add_dialog_button(lv_obj_t *parent,
                                   const char *text,
                                   lv_align_t align,
                                   int32_t x,
                                   lv_color_t color)
{
    lv_obj_t *button = lv_button_create(parent);
    lv_obj_set_size(button, 160, 56);
    lv_obj_align(button, align, x, -18);
    lv_obj_set_style_bg_color(button, color, 0);
    lv_obj_set_style_radius(button, 6, 0);
    lv_obj_set_style_shadow_width(button, 0, 0);

    lv_obj_t *label = lv_label_create(button);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_set_style_text_font(label, ui_i18n_font_32(), 0);
    lv_obj_center(label);
    return button;
}

static void show_reset_dialog(smartcard_reset_target_t target)
{
    if (s_reset_overlay) {
        return;
    }
    s_reset_target = target;

    lv_obj_t *screen = lv_screen_active();
    s_reset_overlay = lv_obj_create(screen);
    lv_obj_remove_style_all(s_reset_overlay);
    lv_obj_set_size(s_reset_overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_center(s_reset_overlay);
    lv_obj_set_style_bg_color(s_reset_overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_reset_overlay, LV_OPA_70, 0);
    lv_obj_add_flag(s_reset_overlay, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_FLOATING);
    lv_obj_clear_flag(s_reset_overlay,
                      LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(s_reset_overlay, reset_dialog_deleted_cb,
                        LV_EVENT_DELETE, NULL);

    lv_obj_t *dialog = lv_obj_create(s_reset_overlay);
    lv_obj_remove_style_all(dialog);
    lv_obj_set_size(dialog, 420, 250);
    lv_obj_center(dialog);
    lv_obj_set_style_bg_color(dialog, lv_color_hex(0x20232B), 0);
    lv_obj_set_style_bg_opa(dialog, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(dialog, lv_color_hex(0x3A3C46), 0);
    lv_obj_set_style_border_width(dialog, 1, 0);
    lv_obj_set_style_radius(dialog, 8, 0);
    lv_obj_clear_flag(dialog, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(dialog);
    lv_label_set_text_fmt(title, ui_i18n_get(UI_STR_RESET_TARGET_TITLE),
                          reset_target_name(target));
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_set_style_text_font(title, ui_i18n_font_35(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 18, 18);

    s_reset_message = lv_label_create(dialog);
    lv_label_set_text(s_reset_message,
                      ui_i18n_get(UI_STR_CARD_DATA_CLEAR_PROMPT));
    lv_obj_set_width(s_reset_message, 384);
    lv_label_set_long_mode(s_reset_message, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(s_reset_message, lv_color_hex(0xB8BDC7), 0);
    lv_obj_set_style_text_font(s_reset_message, ui_i18n_font_32(), 0);
    lv_obj_align(s_reset_message, LV_ALIGN_TOP_LEFT, 18, 72);

    lv_obj_t *cancel = add_dialog_button(dialog, ui_i18n_get(UI_STR_CANCEL),
                                         LV_ALIGN_BOTTOM_LEFT, 18,
                                         lv_color_hex(0x4B5263));
    lv_obj_add_event_cb(cancel, reset_cancel_event_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *confirm = add_dialog_button(dialog, ui_i18n_get(UI_STR_RESET),
                                          LV_ALIGN_BOTTOM_RIGHT, -18,
                                          lv_color_hex(0x2997E8));
    lv_obj_add_event_cb(confirm, reset_confirm_event_cb, LV_EVENT_CLICKED, NULL);
}

static void reset_action_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }
    show_reset_dialog((smartcard_reset_target_t)(uintptr_t)
                          lv_event_get_user_data(event));
}

static void add_reset_action(lv_obj_t *parent,
                             const char *text,
                             smartcard_reset_target_t target)
{
    lv_obj_t *row = create_setting_row(parent, text);
    lv_obj_t *button = lv_button_create(row);
    lv_obj_set_size(button, 106, 54);
    lv_obj_align(button, LV_ALIGN_RIGHT_MID, -14, 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x2997E8), 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x197FC7), LV_STATE_PRESSED);
    lv_obj_set_style_radius(button, 5, 0);
    lv_obj_set_style_shadow_width(button, 0, 0);
    lv_obj_add_event_cb(button, reset_action_event_cb,
                        LV_EVENT_CLICKED, (void *)(uintptr_t)target);

    lv_obj_t *label = lv_label_create(button);
    lv_label_set_text(label, ui_i18n_get(UI_STR_RESET));
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_set_style_text_font(label, ui_i18n_font_32(), 0);
    lv_obj_center(label);
}

static void add_smartcard_settings(lv_obj_t *parent)
{
    lv_obj_t *group = create_group(parent,
                                   SMARTCARD_SETTING_ROW_HEIGHT *
                                       SMARTCARD_SETTING_ROW_COUNT);
    lv_obj_set_flex_flow(group, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(group, 0, 0);

    add_setting_switch(group, ui_i18n_get(UI_STR_ENABLE_OPENPGP),
                       SMARTCARD_OPENPGP_NVS_KEY, true);
    add_setting_switch(group, ui_i18n_get(UI_STR_ENABLE_PIV),
                       SMARTCARD_PIV_NVS_KEY, true);
    add_reset_action(group, ui_i18n_get(UI_STR_PGP_CLEAR_DATA),
                     SMARTCARD_RESET_OPENPGP);
    add_reset_action(group, ui_i18n_get(UI_STR_PIV_CLEAR_DATA),
                     SMARTCARD_RESET_PIV);
    add_reset_action(group, ui_i18n_get(UI_STR_FIDO_CLEAR_DATA),
                     SMARTCARD_RESET_FIDO);
}

void ui_smartcard_show(void)
{
    s_selected_card = read_setting(SMARTCARD_SLOT_NVS_KEY, 0);
    if (s_selected_card >= CANOKEY_GPG_SLOT_COUNT ||
        s_selected_card >= CANOKEY_PIV_SLOT_COUNT) {
        s_selected_card = 0;
    }

    esp_err_t ret = canokey_esp32p4_start();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "CanoKey start failed: %s", esp_err_to_name(ret));
    }
    ret = usb_hid_enable_smartcard_reader();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "SmartCard reader enable failed: %s", esp_err_to_name(ret));
    }
    if (canokey_esp32p4_is_ready()) {
        uint8_t openpgp_enabled = read_setting(SMARTCARD_OPENPGP_NVS_KEY, 1);
        uint8_t piv_enabled = read_setting(SMARTCARD_PIV_NVS_KEY, 1);
        if (device_config_is_openpgp_ccid_enabled() != openpgp_enabled ||
            device_config_is_piv_ccid_enabled() != piv_enabled) {
            if (admin_device_config_set_ccid_enabled(openpgp_enabled,
                                                     piv_enabled) < 0) {
                ESP_LOGW(TAG, "Failed to synchronize CanoKey CCID feature configuration");
            } else {
                usb_ccid_deactivate_card();
                if (openpgp_enabled || piv_enabled) {
                    (void)usb_ccid_activate_card();
                }
            }
        }

        bool slot_changed = canokey_gpg_get_slot() != s_selected_card ||
                            canokey_piv_get_slot() != s_selected_card;
        if (slot_changed) {
            ret = select_card(s_selected_card);
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "Card %u initialization failed: %s",
                         (unsigned)(s_selected_card + 1), esp_err_to_name(ret));
            }
        } else if (!usb_ccid_card_is_inserted() &&
                   (read_setting(SMARTCARD_OPENPGP_NVS_KEY, 1) != 0 ||
                    read_setting(SMARTCARD_PIV_NVS_KEY, 1) != 0)) {
            (void)usb_ccid_activate_card();
        }
    }

    smartcard_openpgp_data_t data;
    load_openpgp_data(&data);

    lv_obj_t *screen = lv_screen_active();
    lv_obj_clean(screen);
    s_reset_overlay = NULL;
    s_reset_message = NULL;
    ui_nav_enable_right_swipe(screen);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x11141A), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(screen, 0, 0);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    ui_status_bar_add(screen, SMARTCARD_WIDTH);

    lv_obj_t *list = lv_obj_create(screen);
    lv_obj_remove_style_all(list);
    lv_obj_set_size(list, SMARTCARD_WIDTH,
                    SMARTCARD_HEIGHT - UI_STATUS_BAR_HEIGHT);
    lv_obj_align(list, LV_ALIGN_TOP_MID, 0, UI_STATUS_BAR_HEIGHT);
    lv_obj_set_style_pad_left(list, 16, 0);
    lv_obj_set_style_pad_right(list, 16, 0);
    lv_obj_set_style_pad_top(list, 4, 0);
    lv_obj_set_style_pad_bottom(list, 24, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list, 8, 0);
    lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_clear_flag(list, LV_OBJ_FLAG_SCROLL_CHAIN_HOR);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, LV_PART_SCROLLBAR);

    add_section_title(list, ui_i18n_get(UI_STR_SMART_CARD_ID));
    add_card_selector(list);
    add_section_title(list, "OpenPGP");
    add_openpgp_information(list, &data);
    add_section_title(list, ui_i18n_get(UI_STR_SMART_CARD_SETTINGS));
    add_smartcard_settings(list);
}

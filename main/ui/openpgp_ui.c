#include "openpgp_ui.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "admin.h"
#include "canokey_esp32p4.h"
#include "canokey_gpg_slots.h"
#include "canokey_piv_slots.h"
#include "esp_log.h"
#include "fs.h"
#include "lvgl.h"
#include "nvs.h"
#include "openpgp.h"
#include "piv.h"
#include "ui_nav.h"
#include "ui_status_bar.h"
#include "usb_ccid.h"
#include "usb_hid_keyboard.h"

#define OPENPGP_UI_WIDTH  480
#define OPENPGP_UI_HEIGHT 640
#define OPENPGP_CONTENT_W 454

#define TXT_CARD_TYPE     "Card type"
#define TXT_SERIAL        "Serial Number"
#define TXT_NAME          "Name"
#define TXT_URL           "Url"
#define OPENPGP_NAME_ATTR 0x5B
#define OPENPGP_KEY_ATTR  0x00
#define OPENPGP_FP_LEN    20
#define OPENPGP_FP_TEXT_LEN (OPENPGP_FP_LEN * 2 + 1)
#define OPENPGP_SN_LEN    4
#define OPENPGP_SN_TEXT_LEN (OPENPGP_SN_LEN * 3)
#define OPENPGP_VALUE_LEN 256
#define SMARTCARD_NVS_NAMESPACE "sc_ui"
#define SMARTCARD_SLOT_NVS_KEY  "selected_card"

static const char *TAG = "openpgp_ui";
static uint8_t s_openpgp_ui_slot;

static uint8_t read_selected_card(void)
{
    nvs_handle_t nvs;
    if (nvs_open(SMARTCARD_NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        return 0;
    }
    uint8_t slot = 0;
    (void)nvs_get_u8(nvs, SMARTCARD_SLOT_NVS_KEY, &slot);
    nvs_close(nvs);
    return slot < CANOKEY_GPG_SLOT_COUNT && slot < CANOKEY_PIV_SLOT_COUNT
               ? slot
               : 0;
}

static esp_err_t write_selected_card(uint8_t slot)
{
    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(SMARTCARD_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = nvs_set_u8(nvs, SMARTCARD_SLOT_NVS_KEY, slot);
    if (ret == ESP_OK) {
        ret = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return ret;
}

typedef struct {
    const char *label;
    const char *value;
    const char *symbol;
    lv_color_t accent;
} openpgp_info_item_t;

typedef struct {
    char status[24];
    char card_type[32];
    char serial[OPENPGP_SN_TEXT_LEN];
    char name[OPENPGP_VALUE_LEN];
    char url[OPENPGP_VALUE_LEN];
    char sig[OPENPGP_FP_TEXT_LEN];
    char dec[OPENPGP_FP_TEXT_LEN];
    char aut[OPENPGP_FP_TEXT_LEN];
} openpgp_card_data_t;

static void set_none(char *dst, size_t dst_size)
{
    if (dst_size > 0) {
        strncpy(dst, "[none]", dst_size - 1);
        dst[dst_size - 1] = '\0';
    }
}

static void set_text(char *dst, size_t dst_size, const char *text)
{
    if (!dst || dst_size == 0) {
        return;
    }
    strncpy(dst, text, dst_size - 1);
    dst[dst_size - 1] = '\0';
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

static void format_hex_spaced(const uint8_t *src, size_t src_len, char *dst, size_t dst_size)
{
    static const char hex[] = "0123456789ABCDEF";

    if (!src || !dst || dst_size < src_len * 3) {
        return;
    }

    for (size_t i = 0; i < src_len; i++) {
        size_t out = i * 3;
        dst[out] = hex[src[i] >> 4];
        dst[out + 1] = hex[src[i] & 0x0F];
        dst[out + 2] = i + 1 < src_len ? ' ' : '\0';
    }
}

static void read_text_attr(const char *path, uint8_t attr, char *dst, size_t dst_size)
{
    if (!dst || dst_size == 0) {
        return;
    }

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
    if (!dst || dst_size == 0) {
        return;
    }

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
    uint8_t fingerprint[OPENPGP_FP_LEN] = {0};
    int len = read_attr(path, OPENPGP_KEY_ATTR, fingerprint, sizeof(fingerprint));
    if (len != OPENPGP_FP_LEN) {
        set_none(dst, dst_size);
        return;
    }

    bool all_zero = true;
    for (size_t i = 0; i < sizeof(fingerprint); i++) {
        if (fingerprint[i] != 0) {
            all_zero = false;
            break;
        }
    }
    if (all_zero) {
        set_none(dst, dst_size);
        return;
    }

    format_hex(fingerprint, sizeof(fingerprint), dst, dst_size);
}

static void load_openpgp_card_data(openpgp_card_data_t *data)
{
    uint8_t serial_bytes[OPENPGP_SN_LEN] = {0};

    memset(data, 0, sizeof(*data));
    strncpy(data->card_type, "OpenPGP", sizeof(data->card_type) - 1);

    if (!canokey_esp32p4_is_ready()) {
        set_text(data->status, sizeof(data->status), "Offline");
        set_none(data->serial, sizeof(data->serial));
        set_none(data->name, sizeof(data->name));
        set_none(data->url, sizeof(data->url));
        set_none(data->sig, sizeof(data->sig));
        set_none(data->dec, sizeof(data->dec));
        set_none(data->aut, sizeof(data->aut));
        return;
    }

    set_text(data->status, sizeof(data->status), "Ready");

    if (canokey_gpg_get_slot() > 0) {
        serial_bytes[3] = canokey_gpg_get_slot();
        format_hex_spaced(serial_bytes, sizeof(serial_bytes), data->serial, sizeof(data->serial));
    } else if (admin_platform_serial_read(serial_bytes) >= 0) {
        format_hex_spaced(serial_bytes, sizeof(serial_bytes), data->serial, sizeof(data->serial));
    } else {
        set_text(data->serial, sizeof(data->serial), "00 00 00 00");
    }

    read_text_attr("pgp-data", OPENPGP_NAME_ATTR, data->name, sizeof(data->name));
    read_text_file("pgp-data", data->url, sizeof(data->url));
    read_fingerprint("pgp-sigk", data->sig, sizeof(data->sig));
    read_fingerprint("pgp-deck", data->dec, sizeof(data->dec));
    read_fingerprint("pgp-autk", data->aut, sizeof(data->aut));
}

static lv_obj_t *add_panel(lv_obj_t *parent, int32_t height)
{
    lv_obj_t *panel = lv_obj_create(parent);
    lv_obj_remove_style_all(panel);
    lv_obj_set_size(panel, OPENPGP_CONTENT_W, height);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x292A32), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(panel, 5, 0);
    lv_obj_set_style_pad_all(panel, 0, 0);
    return panel;
}

static void openpgp_slot_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_VALUE_CHANGED) {
        return;
    }

    lv_obj_t *dropdown = lv_event_get_target(event);
    uint16_t selected = lv_dropdown_get_selected(dropdown);
    if (selected >= CANOKEY_GPG_SLOT_COUNT || selected == s_openpgp_ui_slot) {
        return;
    }

    esp_err_t ret = write_selected_card((uint8_t)selected);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to save selected smart card: %s",
                 esp_err_to_name(ret));
        lv_dropdown_set_selected(dropdown, s_openpgp_ui_slot);
        return;
    }
    s_openpgp_ui_slot = (uint8_t)selected;
    openpgp_ui_show();
}

static void add_summary(lv_obj_t *parent, const openpgp_card_data_t *data)
{
    lv_obj_t *panel = add_panel(parent, 204);
    char serial_text[OPENPGP_SN_TEXT_LEN + 4] = {0};
    snprintf(serial_text, sizeof(serial_text), "SN %s", data->serial);

    lv_obj_t *icon_box = lv_obj_create(panel);
    lv_obj_remove_style_all(icon_box);
    lv_obj_set_size(icon_box, 72, 72);
    lv_obj_align(icon_box, LV_ALIGN_TOP_LEFT, 14, 18);
    lv_obj_set_style_bg_color(icon_box, lv_color_hex(0xA2ED8B), 0);
    lv_obj_set_style_bg_opa(icon_box, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(icon_box, 5, 0);

    lv_obj_t *icon = lv_label_create(icon_box);
    lv_label_set_text(icon, LV_SYMBOL_SD_CARD);
    lv_obj_set_style_text_color(icon, lv_color_hex(0x11141A), 0);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_32, 0);
    lv_obj_center(icon);

    lv_obj_t *title = lv_label_create(panel);
    lv_label_set_text(title, "OpenPGP");
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_32, 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 96, 16);

    lv_obj_t *serial = lv_label_create(panel);
    lv_obj_set_width(serial, 246);
    lv_label_set_text(serial, serial_text);
    lv_label_set_long_mode(serial, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(serial, lv_color_hex(0xAAB0BA), 0);
    lv_obj_set_style_text_font(serial, &lv_font_montserrat_24, 0);
    lv_obj_align(serial, LV_ALIGN_TOP_LEFT, 96, 54);

    lv_obj_t *slot = lv_dropdown_create(panel);
    lv_dropdown_set_options(slot, "1\n2\n3\n4\n5");
    lv_dropdown_set_selected(slot, s_openpgp_ui_slot);
    lv_obj_set_size(slot, 78, 42);
    lv_obj_align(slot, LV_ALIGN_TOP_RIGHT, -14, 18);
    lv_obj_set_style_bg_color(slot, lv_color_hex(0x24262E), 0);
    lv_obj_set_style_bg_opa(slot, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(slot, lv_color_hex(0x3A3C46), 0);
    lv_obj_set_style_border_width(slot, 1, 0);
    lv_obj_set_style_radius(slot, 4, 0);
    lv_obj_set_style_text_color(slot, lv_color_white(), 0);
    lv_obj_set_style_text_font(slot, &lv_font_montserrat_24, 0);
    lv_obj_add_event_cb(slot, openpgp_slot_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *status = lv_label_create(panel);
    lv_label_set_text(status, data->status);
    lv_obj_set_style_text_color(status, strcmp(data->status, "Ready") == 0 ? lv_color_hex(0xA2ED8B) : lv_color_hex(0xFFC958), 0);
    lv_obj_set_style_text_font(status, &lv_font_montserrat_24, 0);
    lv_obj_align(status, LV_ALIGN_TOP_RIGHT, -14, 102);

    lv_obj_t *line = lv_obj_create(panel);
    lv_obj_remove_style_all(line);
    lv_obj_set_size(line, OPENPGP_CONTENT_W - 28, 1);
    lv_obj_align(line, LV_ALIGN_TOP_MID, 0, 132);
    lv_obj_set_style_bg_color(line, lv_color_hex(0x3A3C46), 0);
    lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);

    lv_obj_t *name = lv_label_create(panel);
    lv_label_set_text_fmt(name, "Name  %s", data->name);
    lv_obj_set_width(name, 200);
    lv_label_set_long_mode(name, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_color(name, lv_color_hex(0xD8DCE5), 0);
    lv_obj_set_style_text_font(name, &lv_font_montserrat_24, 0);
    lv_obj_align(name, LV_ALIGN_BOTTOM_LEFT, 16, -22);

    lv_obj_t *url = lv_label_create(panel);
    lv_label_set_text_fmt(url, "URL  %s", data->url);
    lv_obj_set_width(url, 174);
    lv_label_set_long_mode(url, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_color(url, lv_color_hex(0xD8DCE5), 0);
    lv_obj_set_style_text_font(url, &lv_font_montserrat_24, 0);
    lv_obj_align(url, LV_ALIGN_BOTTOM_RIGHT, -16, -22);
}

static void add_action_row(lv_obj_t *parent)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, OPENPGP_CONTENT_W, 69);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 10, 0);

    static const struct {
        const char *symbol;
        const char *label;
    } actions[] = {
        {LV_SYMBOL_REFRESH, "Refresh"},
        {LV_SYMBOL_SETTINGS, "Manage"},
    };

    for (size_t i = 0; i < sizeof(actions) / sizeof(actions[0]); i++) {
        lv_obj_t *button = lv_button_create(row);
        lv_obj_set_size(button, (OPENPGP_CONTENT_W - 10) / 2, 69);
        lv_obj_set_style_bg_color(button, lv_color_hex(0x24262E), 0);
        lv_obj_set_style_bg_color(button, lv_color_hex(0x3A3C46), LV_STATE_PRESSED);
        lv_obj_set_style_radius(button, 5, 0);
        lv_obj_set_style_border_width(button, 1, 0);
        lv_obj_set_style_border_color(button, lv_color_hex(0x3A3C46), 0);

        lv_obj_t *label = lv_label_create(button);
        lv_label_set_text_fmt(label, "%s %s", actions[i].symbol, actions[i].label);
        lv_obj_set_style_text_color(label, lv_color_white(), 0);
        lv_obj_set_style_text_font(label, &lv_font_montserrat_24, 0);
        lv_obj_center(label);
    }
}

static void add_info_item(lv_obj_t *parent, const openpgp_info_item_t *item)
{
    lv_obj_t *row = add_panel(parent, 108);

    lv_obj_t *icon_box = lv_obj_create(row);
    lv_obj_remove_style_all(icon_box);
    lv_obj_set_size(icon_box, 50, 50);
    lv_obj_align(icon_box, LV_ALIGN_LEFT_MID, 12, 0);
    lv_obj_set_style_bg_color(icon_box, item->accent, 0);
    lv_obj_set_style_bg_opa(icon_box, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(icon_box, 4, 0);

    lv_obj_t *icon = lv_label_create(icon_box);
    lv_label_set_text(icon, item->symbol);
    lv_obj_set_style_text_color(icon, lv_color_hex(0x11141A), 0);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_24, 0);
    lv_obj_center(icon);

    lv_obj_t *label = lv_label_create(row);
    lv_label_set_text(label, item->label);
    lv_obj_set_width(label, 214);
    lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_color(label, lv_color_hex(0xAAB0BA), 0);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_24, 0);
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, 70, 14);

    lv_obj_t *value = lv_label_create(row);
    lv_label_set_text(value, item->value);
    lv_obj_set_width(value, 214);
    lv_label_set_long_mode(value, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_color(value, lv_color_white(), 0);
    lv_obj_set_style_text_font(value, &lv_font_montserrat_24, 0);
    lv_obj_align(value, LV_ALIGN_TOP_LEFT, 70, 58);
}

void openpgp_ui_show(void)
{
    esp_err_t ret = canokey_esp32p4_start();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "CanoKey start failed: %s", esp_err_to_name(ret));
    }
    ret = usb_hid_enable_smartcard_reader();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "SmartCard reader enable failed: %s", esp_err_to_name(ret));
    }
    s_openpgp_ui_slot = read_selected_card();
    bool slot_changed = canokey_gpg_get_slot() != s_openpgp_ui_slot ||
                        canokey_piv_get_slot() != s_openpgp_ui_slot;
    if (slot_changed) {
        usb_ccid_deactivate_card();
        if (canokey_gpg_select_slot(s_openpgp_ui_slot) < 0 ||
            canokey_piv_select_slot(s_openpgp_ui_slot) < 0 ||
            openpgp_install(0) < 0 ||
            piv_install(0) < 0) {
            ESP_LOGW(TAG, "Smart card %u select failed",
                     (unsigned)(s_openpgp_ui_slot + 1));
        }
    }
    if (!usb_ccid_card_is_inserted() && !usb_ccid_activate_openpgp_card()) {
        ESP_LOGW(TAG, "OpenPGP virtual card activation delayed or failed");
    }

    lv_obj_t *screen = lv_screen_active();
    lv_obj_clean(screen);
    ui_nav_enable_right_swipe(screen);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x11141A), 0);
    lv_obj_set_style_pad_all(screen, 0, 0);

    ui_status_bar_add(screen, OPENPGP_UI_WIDTH);

    lv_obj_t *list = lv_obj_create(screen);
    lv_obj_remove_style_all(list);
    lv_obj_set_size(list, OPENPGP_UI_WIDTH, OPENPGP_UI_HEIGHT - 38);
    lv_obj_align(list, LV_ALIGN_TOP_MID, 0, 38);
    lv_obj_set_style_pad_left(list, 12, 0);
    lv_obj_set_style_pad_right(list, 12, 0);
    lv_obj_set_style_pad_top(list, 12, 0);
    lv_obj_set_style_pad_bottom(list, 22, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list, 10, 0);
    lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, LV_PART_SCROLLBAR);

    openpgp_card_data_t data;
    load_openpgp_card_data(&data);

    add_summary(list, &data);
    add_action_row(list);

    const openpgp_info_item_t items[] = {
        {TXT_CARD_TYPE, data.card_type, LV_SYMBOL_SD_CARD, lv_color_hex(0xA2ED8B)},
        {TXT_SERIAL, data.serial, LV_SYMBOL_LIST, lv_color_hex(0xBAEEFF)},
        {TXT_NAME, data.name, LV_SYMBOL_EDIT, lv_color_hex(0xB8A8FF)},
        {TXT_URL, data.url, LV_SYMBOL_DIRECTORY, lv_color_hex(0x6DD6F2)},
        {"SIG Fingerprint", data.sig, LV_SYMBOL_SAVE, lv_color_hex(0xFFC958)},
        {"DEC Fingerprint", data.dec, LV_SYMBOL_KEYBOARD, lv_color_hex(0xF27A7A)},
        {"AUT Fingerprint", data.aut, LV_SYMBOL_OK, lv_color_hex(0x9EE6EA)},
    };

    for (size_t i = 0; i < sizeof(items) / sizeof(items[0]); i++) {
        add_info_item(list, &items[i]);
    }
}

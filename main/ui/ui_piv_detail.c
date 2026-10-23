#include "ui_piv_detail.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "admin.h"
#include "canokey_esp32p4.h"
#include "canokey_gpg_slots.h"
#include "canokey_piv_slots.h"
#include "esp_err.h"
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

#define PIV_UI_WIDTH  480
#define PIV_UI_HEIGHT 640
#define PIV_CONTENT_W 454//296

#define PIV_ATTR_DEFAULT 0x81
#define SMARTCARD_NVS_NAMESPACE "sc_ui"
#define SMARTCARD_SLOT_NVS_KEY  "selected_card"

static const char *TAG = "piv_ui";
static uint8_t s_piv_ui_slot;

static uint8_t read_selected_card(void)
{
    nvs_handle_t nvs;
    if (nvs_open(SMARTCARD_NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        return 0;
    }

    uint8_t slot = 0;
    (void)nvs_get_u8(nvs, SMARTCARD_SLOT_NVS_KEY, &slot);
    nvs_close(nvs);
    return slot < CANOKEY_PIV_SLOT_COUNT && slot < CANOKEY_GPG_SLOT_COUNT
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

static esp_err_t activate_selected_card(uint8_t slot)
{
    if (!canokey_esp32p4_is_ready()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (slot >= CANOKEY_PIV_SLOT_COUNT || slot >= CANOKEY_GPG_SLOT_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }

    usb_ccid_deactivate_card();
    if (canokey_gpg_select_slot(slot) < 0 ||
        canokey_piv_select_slot(slot) < 0 ||
        openpgp_install(0) < 0 ||
        piv_install(0) < 0) {
        return ESP_FAIL;
    }
    return usb_ccid_activate_card() ? ESP_OK : ESP_FAIL;
}

typedef struct {
    const char *label;
    const char *value;
    const char *symbol;
    lv_color_t accent;
} piv_info_item_t;

typedef struct {
    const char *name;
    const char *path;
} piv_object_spec_t;

typedef struct {
    char status[24];
    char serial[16];
    char pin[32];
    char puk[32];
    char admin_key[32];
    char chuid[32];
    char ccc[32];
    char auth_cert[32];
    char sig_cert[32];
    char key_mgmt_cert[32];
    char card_auth_cert[32];
    char retired[32];
} piv_card_data_t;

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

static int read_default_attr(const char *path)
{
    uint8_t value = 0;
    int len = read_attr(path, PIV_ATTR_DEFAULT, &value, sizeof(value));
    return len == sizeof(value) ? value : -1;
}

static void format_file_size(const char *path, char *dst, size_t dst_size)
{
    int size = get_file_size(path);
    if (size < 0) {
        set_text(dst, dst_size, "Not present");
        return;
    }

    snprintf(dst, dst_size, "%d B", size);
}

static void format_pin_state(const char *path, char *dst, size_t dst_size)
{
    int size = get_file_size(path);
    int retries = read_default_attr(path);

    if (size < 0) {
        set_text(dst, dst_size, "Not provisioned");
    } else if (retries == 1) {
        set_text(dst, dst_size, "Default value");
    } else if (retries == 0) {
        set_text(dst, dst_size, "Changed");
    } else {
        set_text(dst, dst_size, "Provisioned");
    }
}

static void load_piv_card_data(piv_card_data_t *data)
{
    static const piv_object_spec_t retired_certs[] = {
        {"82", "piv-82c"}, {"83", "piv-83c"}, {"84", "piv-r0f"}, {"85", "piv-r10"},
        {"86", "piv-r11"}, {"87", "piv-r12"}, {"88", "piv-r13"}, {"89", "piv-r14"},
        {"8A", "piv-r15"}, {"8B", "piv-r16"}, {"8C", "piv-r17"}, {"8D", "piv-r18"},
        {"8E", "piv-r19"}, {"8F", "piv-r1a"}, {"90", "piv-r1b"}, {"91", "piv-r1c"},
        {"92", "piv-r1d"}, {"93", "piv-r1e"}, {"94", "piv-r1f"}, {"95", "piv-r20"},
    };

    memset(data, 0, sizeof(*data));

    if (!canokey_esp32p4_is_ready()) {
        set_text(data->status, sizeof(data->status), "Offline");
        set_text(data->serial, sizeof(data->serial), "----");
        set_text(data->pin, sizeof(data->pin), "Unavailable");
        set_text(data->puk, sizeof(data->puk), "Unavailable");
        set_text(data->admin_key, sizeof(data->admin_key), "Unavailable");
        set_text(data->chuid, sizeof(data->chuid), "Unavailable");
        set_text(data->ccc, sizeof(data->ccc), "Unavailable");
        set_text(data->auth_cert, sizeof(data->auth_cert), "Unavailable");
        set_text(data->sig_cert, sizeof(data->sig_cert), "Unavailable");
        set_text(data->key_mgmt_cert, sizeof(data->key_mgmt_cert), "Unavailable");
        set_text(data->card_auth_cert, sizeof(data->card_auth_cert), "Unavailable");
        set_text(data->retired, sizeof(data->retired), "Unavailable");
        return;
    }

    uint8_t serial[4] = {0};
    if (canokey_piv_get_slot() > 0) {
        serial[3] = canokey_piv_get_slot();
        format_hex(serial, sizeof(serial), data->serial, sizeof(data->serial));
    } else if (admin_platform_serial_read(serial) >= 0) {
        format_hex(serial, sizeof(serial), data->serial, sizeof(data->serial));
    } else {
        set_text(data->serial, sizeof(data->serial), "00000000");
    }

    int ready_objects = 0;
    if (get_file_size("piv-chu") >= 0) {
        ready_objects++;
    }
    if (get_file_size("piv-ccc") >= 0) {
        ready_objects++;
    }
    if (get_file_size("piv-admk") >= 0) {
        ready_objects++;
    }
    set_text(data->status, sizeof(data->status), ready_objects == 3 ? "Ready" : "Partial");

    format_pin_state("piv-pin", data->pin, sizeof(data->pin));
    format_pin_state("piv-puk", data->puk, sizeof(data->puk));

    int admin_default = read_default_attr("piv-admk");
    if (admin_default == 1) {
        set_text(data->admin_key, sizeof(data->admin_key), "Default key");
    } else if (admin_default == 0) {
        set_text(data->admin_key, sizeof(data->admin_key), "Changed");
    } else {
        set_text(data->admin_key, sizeof(data->admin_key), "Not present");
    }

    format_file_size("piv-chu", data->chuid, sizeof(data->chuid));
    format_file_size("piv-ccc", data->ccc, sizeof(data->ccc));
    format_file_size("piv-pauc", data->auth_cert, sizeof(data->auth_cert));
    format_file_size("piv-sigc", data->sig_cert, sizeof(data->sig_cert));
    format_file_size("piv-mntc", data->key_mgmt_cert, sizeof(data->key_mgmt_cert));
    format_file_size("piv-cauc", data->card_auth_cert, sizeof(data->card_auth_cert));

    int retired_count = 0;
    for (size_t i = 0; i < sizeof(retired_certs) / sizeof(retired_certs[0]); i++) {
        (void)retired_certs[i].name;
        if (get_file_size(retired_certs[i].path) >= 0) {
            retired_count++;
        }
    }
    snprintf(data->retired, sizeof(data->retired), "%d / 20 present", retired_count);
}

static lv_obj_t *add_panel(lv_obj_t *parent, int32_t height)
{
    lv_obj_t *panel = lv_obj_create(parent);
    lv_obj_remove_style_all(panel);
    lv_obj_set_size(panel, PIV_CONTENT_W, height);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x292A32), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(panel, 5, 0);
    lv_obj_set_style_pad_all(panel, 0, 0);
    return panel;
}

static void piv_slot_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_VALUE_CHANGED) {
        return;
    }

    lv_obj_t *dropdown = lv_event_get_target(event);
    uint16_t selected = lv_dropdown_get_selected(dropdown);
    if (selected >= CANOKEY_PIV_SLOT_COUNT || selected == s_piv_ui_slot) {
        return;
    }

    esp_err_t ret = write_selected_card((uint8_t)selected);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to save selected smart card: %s",
                 esp_err_to_name(ret));
        lv_dropdown_set_selected(dropdown, s_piv_ui_slot);
        return;
    }

    s_piv_ui_slot = (uint8_t)selected;
    ui_piv_detail_show();
}

static void add_summary(lv_obj_t *parent, const piv_card_data_t *data)
{
    lv_obj_t *panel = add_panel(parent, 204);

    lv_obj_t *icon_box = lv_obj_create(panel);
    lv_obj_remove_style_all(icon_box);
    lv_obj_set_size(icon_box, 72, 72);
    lv_obj_align(icon_box, LV_ALIGN_TOP_LEFT, 14, 18);
    lv_obj_set_style_bg_color(icon_box, lv_color_hex(0xFFC958), 0);
    lv_obj_set_style_bg_opa(icon_box, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(icon_box, 5, 0);

    lv_obj_t *icon = lv_label_create(icon_box);
    lv_label_set_text(icon, LV_SYMBOL_SD_CARD);
    lv_obj_set_style_text_color(icon, lv_color_hex(0x11141A), 0);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_32, 0);
    lv_obj_center(icon);

    lv_obj_t *title = lv_label_create(panel);
    lv_label_set_text(title, "PIV Card");
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_32, 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 96, 16);

    lv_obj_t *serial = lv_label_create(panel);
    lv_obj_set_width(serial, 172);
    lv_label_set_text_fmt(serial, "SN %s", data->serial);
    lv_label_set_long_mode(serial, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_color(serial, lv_color_hex(0xAAB0BA), 0);
    lv_obj_set_style_text_font(serial, &lv_font_montserrat_24, 0);
    lv_obj_align(serial, LV_ALIGN_TOP_LEFT, 96, 58);

    lv_obj_t *slot = lv_dropdown_create(panel);
    lv_dropdown_set_options(slot, "1\n2\n3\n4\n5");
    lv_dropdown_set_selected(slot, s_piv_ui_slot);
    lv_obj_set_size(slot, 78, 42);
    lv_obj_align(slot, LV_ALIGN_TOP_RIGHT, -14, 18);
    lv_obj_set_style_bg_color(slot, lv_color_hex(0x24262E), 0);
    lv_obj_set_style_bg_opa(slot, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(slot, lv_color_hex(0x3A3C46), 0);
    lv_obj_set_style_border_width(slot, 1, 0);
    lv_obj_set_style_radius(slot, 4, 0);
    lv_obj_set_style_text_color(slot, lv_color_white(), 0);
    lv_obj_set_style_text_font(slot, &lv_font_montserrat_24, 0);
    lv_obj_add_event_cb(slot, piv_slot_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *status = lv_label_create(panel);
    lv_label_set_text(status, data->status);
    lv_obj_set_style_text_color(status, strcmp(data->status, "Ready") == 0 ? lv_color_hex(0xA2ED8B) : lv_color_hex(0xFFC958), 0);
    lv_obj_set_style_text_font(status, &lv_font_montserrat_24, 0);
    lv_obj_align(status, LV_ALIGN_TOP_RIGHT, -14, 102);

    lv_obj_t *line = lv_obj_create(panel);
    lv_obj_remove_style_all(line);
    lv_obj_set_size(line, PIV_CONTENT_W - 28, 1);
    lv_obj_align(line, LV_ALIGN_TOP_MID, 0, 132);
    lv_obj_set_style_bg_color(line, lv_color_hex(0x3A3C46), 0);
    lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);

    lv_obj_t *pin = lv_label_create(panel);
    lv_label_set_text_fmt(pin, "PIN  %s", data->pin);
    lv_obj_set_width(pin, 124);
    lv_label_set_long_mode(pin, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_color(pin, lv_color_hex(0xD8DCE5), 0);
    lv_obj_set_style_text_font(pin, &lv_font_montserrat_24, 0);
    lv_obj_align(pin, LV_ALIGN_BOTTOM_LEFT, 16, -22);

    lv_obj_t *puk = lv_label_create(panel);
    lv_label_set_text_fmt(puk, "PUK  %s", data->puk);
    lv_obj_set_width(puk, 124);
    lv_label_set_long_mode(puk, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_color(puk, lv_color_hex(0xD8DCE5), 0);
    lv_obj_set_style_text_font(puk, &lv_font_montserrat_24, 0);
    lv_obj_align(puk, LV_ALIGN_BOTTOM_RIGHT, -16, -22);
}

static void add_action_row(lv_obj_t *parent)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, PIV_CONTENT_W, 69);
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
        lv_obj_set_size(button, (PIV_CONTENT_W - 10) / 2, 69);
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

static void add_info_item(lv_obj_t *parent, const piv_info_item_t *item)
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

void ui_piv_detail_show(void)
{
    esp_err_t ret = canokey_esp32p4_start();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "CanoKey start failed: %s", esp_err_to_name(ret));
    }

    ret = usb_hid_enable_smartcard_reader();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "SmartCard reader enable failed: %s", esp_err_to_name(ret));
    }

    s_piv_ui_slot = read_selected_card();
    bool slot_changed = canokey_gpg_get_slot() != s_piv_ui_slot ||
                        canokey_piv_get_slot() != s_piv_ui_slot;
    if (slot_changed) {
        ret = activate_selected_card(s_piv_ui_slot);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Smart card %u activation failed: %s",
                     (unsigned)(s_piv_ui_slot + 1), esp_err_to_name(ret));
        }
    } else if (!usb_ccid_card_is_inserted() && !usb_ccid_activate_card()) {
        ESP_LOGW(TAG, "PIV virtual card activation delayed or failed");
    }

    lv_obj_t *screen = lv_screen_active();
    lv_obj_clean(screen);
    ui_nav_enable_right_swipe(screen);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x11141A), 0);
    lv_obj_set_style_pad_all(screen, 0, 0);

    ui_status_bar_add(screen, PIV_UI_WIDTH);

    lv_obj_t *list = lv_obj_create(screen);
    lv_obj_remove_style_all(list);
    lv_obj_set_size(list, PIV_UI_WIDTH, PIV_UI_HEIGHT - 38);
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

    piv_card_data_t data;
    load_piv_card_data(&data);

    add_summary(list, &data);
    add_action_row(list);

    const piv_info_item_t items[] = {
        {"Management Key", data.admin_key, LV_SYMBOL_KEYBOARD, lv_color_hex(0xFFC958)},
        {"CHUID", data.chuid, LV_SYMBOL_LIST, lv_color_hex(0xA2ED8B)},
        {"CCC", data.ccc, LV_SYMBOL_LIST, lv_color_hex(0xBAEEFF)},
        {"Authentication Cert 9A", data.auth_cert, LV_SYMBOL_SAVE, lv_color_hex(0xB8A8FF)},
        {"Signature Cert 9C", data.sig_cert, LV_SYMBOL_EDIT, lv_color_hex(0xF27A7A)},
        {"Key Management Cert 9D", data.key_mgmt_cert, LV_SYMBOL_KEYBOARD, lv_color_hex(0x6DD6F2)},
        {"Card Auth Cert 9E", data.card_auth_cert, LV_SYMBOL_OK, lv_color_hex(0x9EE6EA)},
        {"Retired Certs 82-95", data.retired, LV_SYMBOL_LIST, lv_color_hex(0xFF7C88)},
    };

    for (size_t i = 0; i < sizeof(items) / sizeof(items[0]); i++) {
        add_info_item(list, &items[i]);
    }
}

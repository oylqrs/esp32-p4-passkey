#include "chameleon_manager_ui.h"

#include <stdio.h>

#include "chameleon_uart/chameleon_commands.h"
#include "esp_log.h"
#include "lvgl.h"
#include "ui_nav.h"
#include "ui/i18n/ui_i18n.h"
#include "ui_status_bar.h"
#include "usb_nrf/USB_NRF_SWITCH.h"

#define CHAMELEON_UI_WIDTH 480
#define CHAMELEON_CONTENT_HEIGHT 584
#define CHAMELEON_CARD_WIDTH (CHAMELEON_UI_WIDTH - 36)
#define CHAMELEON_SLOT_ROW_HEIGHT 76
#define CHAMELEON_SETTING_ROW_HEIGHT 96
#define EM410X_UID_DISPLAY_BYTES 5
#define LF_DETAILS_Y (97 + CHAMELEON_SLOT_ROW_HEIGHT * 3)
#define EM410X_UID_Y (LF_DETAILS_Y + 40)

static const char *TAG = "chameleon_ui";

typedef struct {
    lv_obj_t *hf_name;
    lv_obj_t *hf_type;
    lv_obj_t *hf_uid;
    lv_obj_t *lf_name;
    lv_obj_t *lf_type;
    lv_obj_t *lf_uid;
} slot_view_t;

static slot_view_t s_slot_view;

static lv_obj_t *add_label(lv_obj_t *parent, const char *text, int32_t x, int32_t y,
                           const lv_font_t *font, lv_color_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_width(label, CHAMELEON_UI_WIDTH - 52 - x);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, color, 0);
    lv_obj_set_style_text_letter_space(label, 0, 0);
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, x, y);
    return label;
}

static lv_obj_t *add_card(lv_obj_t *parent, int32_t y, int32_t height)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, CHAMELEON_CARD_WIDTH, height);
    lv_obj_align(card, LV_ALIGN_TOP_MID, 0, y);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x292A32), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 8, 0);
    lv_obj_set_style_pad_all(card, 0, 0);
    return card;
}

static void style_dropdown_list(lv_event_t *event)
{
    lv_obj_t *dropdown = lv_event_get_target_obj(event);
    lv_obj_t *list = lv_dropdown_get_list(dropdown);
    if (list == NULL) {
        return;
    }

    lv_obj_set_style_text_font(list, ui_i18n_font_32(), LV_PART_MAIN);
    lv_obj_set_style_text_font(list, ui_i18n_font_32(), LV_PART_SELECTED);
    lv_obj_set_style_text_letter_space(list, 0, LV_PART_MAIN);
    lv_obj_set_style_text_letter_space(list, 0, LV_PART_SELECTED);
    lv_obj_set_style_pad_row(list, 10, LV_PART_MAIN);
}

static lv_obj_t *add_dropdown(lv_obj_t *parent, const char *options, int32_t y, int32_t width)
{
    lv_obj_t *dropdown = lv_dropdown_create(parent);
    lv_dropdown_set_options(dropdown, options);
    lv_obj_set_size(dropdown, width, 56);
    lv_obj_align(dropdown, LV_ALIGN_TOP_RIGHT, -14, y);
    lv_obj_set_style_bg_color(dropdown, lv_color_hex(0x24262E), 0);
    lv_obj_set_style_bg_opa(dropdown, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(dropdown, 0, 0);
    lv_obj_set_style_radius(dropdown, 6, 0);
    lv_obj_set_style_text_color(dropdown, lv_color_hex(0xD8D9DE), 0);
    lv_obj_set_style_text_font(dropdown, ui_i18n_font_32(), 0);
    lv_obj_set_style_text_letter_space(dropdown, 0, 0);
    lv_obj_set_style_pad_left(dropdown, 10, 0);
    lv_obj_set_style_pad_right(dropdown, 30, 0);
    lv_obj_add_event_cb(dropdown, style_dropdown_list, LV_EVENT_READY, NULL);
    return dropdown;
}

static void add_separator(lv_obj_t *parent, int32_t y)
{
    lv_obj_t *line = lv_obj_create(parent);
    lv_obj_remove_style_all(line);
    lv_obj_set_size(line, CHAMELEON_CARD_WIDTH - 32, 1);
    lv_obj_align(line, LV_ALIGN_TOP_MID, 0, y);
    lv_obj_set_style_bg_color(line, lv_color_hex(0x3A3C46), 0);
    lv_obj_set_style_bg_opa(line, LV_OPA_70, 0);
}

static const char *hf_tag_type_name(uint16_t type)
{
    switch (type) {
    case 0: return "Empty";
    case 1000: return "MF Mini";
    case 1001: return "MF 1K";
    case 1002: return "MF 2K";
    case 1003: return "MF 4K";
    case 1100: return "NTAG 213";
    case 1101: return "NTAG 215";
    case 1102: return "NTAG 216";
    case 1103: return "MF0ICU1";
    case 1104: return "MF0ICU2";
    case 1105: return "MF0UL11";
    case 1106: return "MF0UL21";
    case 1107: return "NTAG 210";
    case 1108: return "NTAG 212";
    case 3000: return "HF14A-4";
    default: return NULL;
    }
}

static const char *lf_tag_type_name(uint16_t type)
{
    switch (type) {
    case 0: return "Empty";
    case 100: return "EM410X";
    case 101: return "EM410X/16";
    case 102: return "EM410X/32";
    case 103: return "EM410X/64";
    case 104: return "EM410X Electra";
    case 150: return "PAC";
    case 170: return "Viking";
    case 200: return "HID Prox";
    case 201: return "ioProx";
    case 310: return "IDTECK";
    default: return NULL;
    }
}

static void format_type_name(char *text, size_t size, uint16_t type, bool hf)
{
    const char *name = hf ? hf_tag_type_name(type) : lf_tag_type_name(type);
    if (name != NULL) {
        snprintf(text, size, "%s", name);
    } else {
        snprintf(text, size, "0x%04X", type);
    }
}

static void format_uid(char *text, size_t size, const uint8_t *uid, uint8_t uid_length)
{
    if (uid_length == 0) {
        snprintf(text, size, "--");
        return;
    }

    size_t written = 0;
    for (uint8_t index = 0; index < uid_length && written + 2 < size; index++) {
        written += (size_t)snprintf(text + written, size - written, "%02X", uid[index]);
    }
}

static bool is_em410x_type(uint16_t type)
{
    return type >= 100 && type <= 104;
}

static void format_lf_uid(char *text, size_t size, uint16_t type,
                          const uint8_t *uid, uint8_t uid_length)
{
    if (is_em410x_type(type) && uid_length > EM410X_UID_DISPLAY_BYTES) {
        uid += uid_length - EM410X_UID_DISPLAY_BYTES;
        uid_length = EM410X_UID_DISPLAY_BYTES;
    }
    format_uid(text, size, uid, uid_length);
}

static void refresh_slot_view(uint8_t slot)
{
    chameleon_slot_details_t details;
    uint16_t device_status = 0;
    esp_err_t ret = chameleon_get_slot_details(slot, &details, &device_status);
    if (ret != ESP_OK) {
        lv_label_set_text(s_slot_view.hf_name, ui_i18n_get(UI_STR_UNAVAILABLE));
        lv_label_set_text(s_slot_view.hf_type, "--");
        lv_label_set_text(s_slot_view.hf_uid, "--");
        lv_label_set_text(s_slot_view.lf_name, ui_i18n_get(UI_STR_UNAVAILABLE));
        lv_label_set_text(s_slot_view.lf_type, "--");
        lv_label_set_text(s_slot_view.lf_uid, "--");
        ESP_LOGW(TAG, "Failed to read Chameleon slot %u: %s, status=0x%04x",
                 slot + 1, esp_err_to_name(ret), device_status);
        return;
    }

    uint16_t hf_type = details.actual_hf_tag_type != 0
                           ? details.actual_hf_tag_type
                           : details.configured.hf_tag_type;
    uint16_t lf_type = details.actual_lf_tag_type != 0
                           ? details.actual_lf_tag_type
                           : details.configured.lf_tag_type;
    char hf_type_text[20];
    char lf_type_text[20];
    char hf_uid[32];
    char lf_uid[40];
    format_type_name(hf_type_text, sizeof(hf_type_text), hf_type, true);
    format_type_name(lf_type_text, sizeof(lf_type_text), lf_type, false);
    format_uid(hf_uid, sizeof(hf_uid), details.hf_uid, details.hf_uid_length);
    format_lf_uid(lf_uid, sizeof(lf_uid), lf_type,
                  details.lf_uid, details.lf_uid_length);

    lv_label_set_text(s_slot_view.hf_name,
                      details.hf_nickname[0] != '\0' ? details.hf_nickname : "--");
    lv_label_set_text(s_slot_view.hf_type, hf_type_text);
    lv_label_set_text(s_slot_view.hf_uid, hf_uid);
    lv_label_set_text(s_slot_view.lf_name,
                      details.lf_nickname[0] != '\0' ? details.lf_nickname : "--");
    lv_label_set_text(s_slot_view.lf_type, lf_type_text);
    lv_label_set_text(s_slot_view.lf_uid, lf_uid);
    lv_obj_align(s_slot_view.lf_uid, LV_ALIGN_TOP_LEFT,
                 is_em410x_type(lf_type) ? 16 : 132,
                 is_em410x_type(lf_type) ? EM410X_UID_Y : LF_DETAILS_Y);
}

static void slot_dropdown_changed(lv_event_t *event)
{
    lv_obj_t *dropdown = lv_event_get_target_obj(event);
    refresh_slot_view((uint8_t)lv_dropdown_get_selected(dropdown));
}

static lv_obj_t *add_switch_row(lv_obj_t *parent, const char *text, int32_t y, bool checked)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, CHAMELEON_CARD_WIDTH, CHAMELEON_SETTING_ROW_HEIGHT);
    lv_obj_set_pos(row, 0, y);
    lv_obj_set_style_pad_left(row, 16, 0);
    lv_obj_set_style_pad_right(row, 16, 0);
    lv_obj_set_style_pad_column(row, 14, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                         LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *label = lv_label_create(row);
    lv_label_set_text(label, text);
    lv_obj_set_width(label, 0);
    lv_obj_set_flex_grow(label, 1);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(label, ui_i18n_font_32(), 0);
    lv_obj_set_style_text_color(label, lv_color_hex(0xD8D9DE), 0);
    lv_obj_set_style_text_letter_space(label, 0, 0);

    lv_obj_t *sw = lv_switch_create(row);
    lv_obj_set_size(sw, 74, 38);
    lv_obj_set_style_bg_color(sw, lv_color_hex(0x3A3C46), LV_PART_MAIN);
    lv_obj_set_style_bg_color(sw, lv_color_hex(0x279FE8), LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(sw, lv_color_white(), LV_PART_KNOB);
    if (checked) {
        lv_obj_add_state(sw, LV_STATE_CHECKED);
    }
    return sw;
}

static void usb_nrf_switch_changed(lv_event_t *event)
{
    lv_obj_t *sw = lv_event_get_target_obj(event);
    bool enabled = lv_obj_has_state(sw, LV_STATE_CHECKED);
    esp_err_t ret = usb_nrf_switch_set(enabled);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set USB to NRF switch to %d: %s",
                 enabled, esp_err_to_name(ret));
        if (enabled) {
            lv_obj_remove_state(sw, LV_STATE_CHECKED);
        } else {
            lv_obj_add_state(sw, LV_STATE_CHECKED);
        }
    }
}

static void chameleon_emulation_changed(lv_event_t *event)
{
    lv_obj_t *sw = lv_event_get_target_obj(event);
    bool enabled = lv_obj_has_state(sw, LV_STATE_CHECKED);
    uint16_t device_status = 0;
    chameleon_mode_t mode = enabled ? CHAMELEON_MODE_TAG : CHAMELEON_MODE_READER;
    esp_err_t ret = chameleon_change_mode(mode, &device_status);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set Chameleon emulation to %d: %s, status=0x%04x",
                 enabled, esp_err_to_name(ret), device_status);
        if (enabled) {
            lv_obj_remove_state(sw, LV_STATE_CHECKED);
        } else {
            lv_obj_add_state(sw, LV_STATE_CHECKED);
        }
    }
}

void chameleon_manager_ui_show(void)
{
    lv_obj_t *screen = lv_screen_active();
    lv_obj_clean(screen);
    ui_nav_enable_right_swipe(screen);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x11141A), 0);
    lv_obj_set_style_pad_all(screen, 0, 0);

    ui_status_bar_add(screen, CHAMELEON_UI_WIDTH);

    lv_obj_t *content = lv_obj_create(screen);
    lv_obj_remove_style_all(content);
    lv_obj_set_size(content, CHAMELEON_UI_WIDTH, CHAMELEON_CONTENT_HEIGHT);
    lv_obj_align(content, LV_ALIGN_TOP_MID, 0, UI_STATUS_BAR_HEIGHT);
    lv_obj_add_flag(content, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(content, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(content, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_width(content, 4, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_color(content, lv_color_hex(0x6B6E78), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(content, LV_OPA_50, LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(content, 2, LV_PART_SCROLLBAR);

    char title[48];
    snprintf(title, sizeof(title), "%s", ui_i18n_get(UI_STR_CHAMELEON));
    chameleon_firmware_version_t version;
    uint16_t device_status = 0;
    esp_err_t ret = chameleon_get_firmware_version(&version, &device_status);
    if (ret == ESP_OK) {
        snprintf(title, sizeof(title), "%s v%u.%u",
                 ui_i18n_get(UI_STR_CHAMELEON), version.major, version.minor);
    } else {
        ESP_LOGW(TAG, "Failed to read Chameleon version: %s, status=0x%04x",
                 esp_err_to_name(ret), device_status);
    }
    add_label(content, title, 18, 10, ui_i18n_font_35(), lv_color_white());

    lv_obj_t *card = add_card(content, 58, 420);
    add_label(card, ui_i18n_get(UI_STR_CARD_SLOT), 16, 19,
              ui_i18n_font_32(), lv_color_hex(0xD8D9DE));
    lv_obj_t *slot_dropdown = add_dropdown(card,
        ui_i18n_get_language() == UI_LANGUAGE_CHINESE
            ? "卡槽 1\n卡槽 2\n卡槽 3\n卡槽 4\n卡槽 5\n卡槽 6\n卡槽 7\n卡槽 8"
            : "Slot 1\nSlot 2\nSlot 3\nSlot 4\nSlot 5\nSlot 6\nSlot 7\nSlot 8", 12, 190);
    lv_obj_add_event_cb(slot_dropdown, slot_dropdown_changed, LV_EVENT_VALUE_CHANGED, NULL);

    add_separator(card, 79);

    add_label(card, "HF", 16, 97,
              ui_i18n_font_32(), lv_color_hex(0xD8D9DE));
    s_slot_view.hf_name = add_label(card, "--", 132, 97,
                                    ui_i18n_font_32(), lv_color_white());

    s_slot_view.hf_type = add_label(card, "--", 16,
                                    97 + CHAMELEON_SLOT_ROW_HEIGHT,
                                    ui_i18n_font_32(), lv_color_hex(0xD8D9DE));
    s_slot_view.hf_uid = add_label(card, "--", 132,
                                   97 + CHAMELEON_SLOT_ROW_HEIGHT,
                                   ui_i18n_font_32(), lv_color_white());

    add_label(card, "LF", 16, 97 + CHAMELEON_SLOT_ROW_HEIGHT * 2,
              ui_i18n_font_32(), lv_color_hex(0xD8D9DE));
    s_slot_view.lf_name = add_label(card, "--", 132,
                                    97 + CHAMELEON_SLOT_ROW_HEIGHT * 2,
                                    ui_i18n_font_32(), lv_color_white());

    s_slot_view.lf_type = add_label(card, "--", 16, LF_DETAILS_Y,
                                    ui_i18n_font_32(), lv_color_hex(0xD8D9DE));
    s_slot_view.lf_uid = add_label(card, "--", 132, LF_DETAILS_Y,
                                   ui_i18n_font_32(), lv_color_white());
    refresh_slot_view(0);

    add_label(content, ui_i18n_get(UI_STR_NRF_SETTINGS), 18, 498,
              ui_i18n_font_35(), lv_color_hex(0xB8BBC4));

    lv_obj_t *settings = add_card(content, 550, 448);
    add_label(settings, ui_i18n_get(UI_STR_NRF_MODE), 16, 19,
              ui_i18n_font_32(), lv_color_hex(0xD8D9DE));
    char mode_options[48];
    snprintf(mode_options, sizeof(mode_options), "%s\n%s",
             ui_i18n_get(UI_STR_CHAMELEON), ui_i18n_get(UI_STR_READER));
    add_dropdown(settings, mode_options, 12, 220);
    lv_obj_t *usb_nrf_switch =
        add_switch_row(settings, ui_i18n_get(UI_STR_SWITCH_USB_TO_NRF), 80,
                       usb_nrf_switch_is_enabled());
    lv_obj_add_event_cb(usb_nrf_switch, usb_nrf_switch_changed, LV_EVENT_VALUE_CHANGED, NULL);
    chameleon_mode_t current_mode = CHAMELEON_MODE_TAG;
    device_status = 0;
    ret = chameleon_get_mode(&current_mode, &device_status);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to read Chameleon mode: %s, status=0x%04x",
                 esp_err_to_name(ret), device_status);
    }
    lv_obj_t *emulation_switch =
        add_switch_row(settings, ui_i18n_get(UI_STR_CHAMELEON_EMULATION), 176,
                       current_mode == CHAMELEON_MODE_TAG);
    lv_obj_add_event_cb(emulation_switch, chameleon_emulation_changed,
                        LV_EVENT_VALUE_CHANGED, NULL);
    add_switch_row(settings, ui_i18n_get(UI_STR_START_WHEN_LOCKED), 272, true);
    add_label(settings, ui_i18n_get(UI_STR_LOCK_POLLING), 16, 387,
              ui_i18n_font_32(), lv_color_hex(0xD8D9DE));
    add_dropdown(settings, "80 ms\n100 ms\n200 ms\n500 ms", 380, 170);
}

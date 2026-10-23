#include "ui_usb_vid_setup.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "esp_err.h"
#include "lvgl.h"
#include "ui/i18n/ui_i18n.h"
#include "ui_nav.h"
#include "ui_status_bar.h"
#include "usb/usb_msc.h"

#define USB_VID_SETUP_WIDTH 480

static lv_obj_t *s_current_vid;
static lv_obj_t *s_vid_field;
static lv_obj_t *s_status;

static void set_status(ui_string_id_t id, lv_color_t color)
{
    lv_label_set_text(s_status, ui_i18n_get(id));
    lv_obj_set_style_text_color(s_status, color, 0);
}

static void set_current_vid(uint16_t vid)
{
    lv_label_set_text_fmt(s_current_vid, "0x%04X", (unsigned int)vid);
}

static void set_vid_field(uint16_t vid)
{
    char text[5];
    lv_snprintf(text, sizeof(text), "%04X", (unsigned int)vid);
    lv_textarea_set_text(s_vid_field, text);
    lv_textarea_set_cursor_pos(s_vid_field, LV_TEXTAREA_CURSOR_LAST);
}

static bool parse_vid(const char *text, uint16_t *vid)
{
    if (!text || !vid || strlen(text) != 4U) {
        return false;
    }

    uint16_t value = 0;
    for (size_t i = 0; i < 4U; ++i) {
        uint8_t digit;
        if (text[i] >= '0' && text[i] <= '9') {
            digit = (uint8_t)(text[i] - '0');
        } else if (text[i] >= 'A' && text[i] <= 'F') {
            digit = (uint8_t)(text[i] - 'A' + 10);
        } else if (text[i] >= 'a' && text[i] <= 'f') {
            digit = (uint8_t)(text[i] - 'a' + 10);
        } else {
            return false;
        }
        value = (uint16_t)((value << 4) | digit);
    }

    if (value == 0U || value == UINT16_MAX) {
        return false;
    }

    *vid = value;
    return true;
}

static void save_vid_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }

    uint16_t vid;
    if (!parse_vid(lv_textarea_get_text(s_vid_field), &vid)) {
        set_status(UI_STR_USB_VID_INVALID, lv_color_hex(0xFF8B92));
        return;
    }

    esp_err_t ret = usb_hid_set_vid(vid);
    if (ret == ESP_OK) {
        set_current_vid(vid);
        set_vid_field(vid);
        set_status(UI_STR_USB_VID_SAVED, lv_color_hex(0xA2ED8B));
    } else if (ret == ESP_ERR_INVALID_ARG) {
        set_status(UI_STR_USB_VID_INVALID, lv_color_hex(0xFF8B92));
    } else {
        set_status(UI_STR_FAILED, lv_color_hex(0xFF8B92));
    }
}

static void vid_field_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_VALUE_CHANGED) {
        set_status(UI_STR_USB_VID_RANGE, lv_color_hex(0x9EA3AD));
    }
}

static void keypad_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_VALUE_CHANGED) {
        return;
    }

    lv_obj_t *keypad = lv_event_get_target_obj(event);
    uint32_t id = lv_buttonmatrix_get_selected_button(keypad);
    const char *key = lv_buttonmatrix_get_button_text(keypad, id);
    if (!key) {
        return;
    }

    if (strcmp(key, LV_SYMBOL_BACKSPACE) == 0) {
        lv_textarea_delete_char(s_vid_field);
    } else {
        lv_textarea_add_text(s_vid_field, key);
    }
}

static void back_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_CLICKED) {
        ui_nav_go_back();
    }
}

static lv_obj_t *create_value_row(lv_obj_t *screen, int32_t y, int32_t height)
{
    lv_obj_t *row = lv_obj_create(screen);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, USB_VID_SETUP_WIDTH - 50, height);
    lv_obj_align(row, LV_ALIGN_TOP_MID, 0, y);
    lv_obj_set_style_bg_color(row, lv_color_hex(0x292A32), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(row, 4, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    return row;
}

void ui_usb_vid_setup_show(void)
{
    lv_obj_t *screen = lv_screen_active();
    lv_obj_clean(screen);
    ui_nav_enable_right_swipe(screen);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x11141A), 0);
    lv_obj_set_style_pad_all(screen, 0, 0);
    ui_status_bar_add(screen, USB_VID_SETUP_WIDTH);

    lv_obj_t *back = lv_button_create(screen);
    lv_obj_set_size(back, 48, 48);
    lv_obj_align(back, LV_ALIGN_TOP_LEFT, 16, 66);
    lv_obj_set_style_bg_opa(back, LV_OPA_TRANSP, 0);
    lv_obj_set_style_shadow_width(back, 0, 0);
    lv_obj_add_event_cb(back, back_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *back_icon = lv_label_create(back);
    lv_label_set_text(back_icon, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(back_icon, lv_color_white(), 0);
    lv_obj_set_style_text_font(back_icon, &lv_font_montserrat_28, 0);
    lv_obj_center(back_icon);

    lv_obj_t *title = lv_label_create(screen);
    lv_label_set_text(title, ui_i18n_get(UI_STR_USB_VID));
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_set_style_text_font(title, ui_i18n_font_28(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 76, 72);

    lv_obj_t *current_row = create_value_row(screen, 122, 58);
    lv_obj_t *current_label = lv_label_create(current_row);
    lv_label_set_text(current_label, ui_i18n_get(UI_STR_CURRENT_VID));
    lv_obj_set_style_text_color(current_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(current_label, ui_i18n_font_28(), 0);
    lv_obj_align(current_label, LV_ALIGN_LEFT_MID, 14, 0);

    s_current_vid = lv_label_create(current_row);
    lv_obj_set_style_text_color(s_current_vid, lv_color_hex(0x7CC7FF), 0);
    lv_obj_set_style_text_font(s_current_vid, &lv_font_montserrat_28, 0);
    lv_obj_align(s_current_vid, LV_ALIGN_RIGHT_MID, -16, 0);

    lv_obj_t *edit_row = create_value_row(screen, 190, 64);
    lv_obj_t *new_label = lv_label_create(edit_row);
    lv_label_set_text(new_label, ui_i18n_get(UI_STR_NEW_VID));
    lv_obj_set_style_text_color(new_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(new_label, ui_i18n_font_28(), 0);
    lv_obj_align(new_label, LV_ALIGN_LEFT_MID, 14, 0);

    s_vid_field = lv_textarea_create(edit_row);
    lv_obj_set_size(s_vid_field, 130, 48);
    lv_obj_align(s_vid_field, LV_ALIGN_LEFT_MID, 148, 0);
    lv_textarea_set_one_line(s_vid_field, true);
    lv_textarea_set_accepted_chars(s_vid_field, "0123456789ABCDEFabcdef");
    lv_textarea_set_max_length(s_vid_field, 4);
    lv_obj_set_style_bg_color(s_vid_field, lv_color_hex(0x3A3C46), 0);
    lv_obj_set_style_border_color(s_vid_field, lv_color_hex(0x555965), 0);
    lv_obj_set_style_border_width(s_vid_field, 1, 0);
    lv_obj_set_style_radius(s_vid_field, 4, 0);
    lv_obj_set_style_text_color(s_vid_field, lv_color_white(), 0);
    lv_obj_set_style_text_font(s_vid_field, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_align(s_vid_field, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_left(s_vid_field, 8, 0);
    lv_obj_set_style_pad_right(s_vid_field, 8, 0);

    lv_obj_t *save = lv_button_create(edit_row);
    lv_obj_set_size(save, 112, 48);
    lv_obj_align(save, LV_ALIGN_RIGHT_MID, -14, 0);
    lv_obj_set_style_bg_color(save, lv_color_hex(0x2997E8), 0);
    lv_obj_set_style_bg_color(save, lv_color_hex(0x197FC7), LV_STATE_PRESSED);
    lv_obj_set_style_radius(save, 6, 0);
    lv_obj_set_style_shadow_width(save, 0, 0);
    lv_obj_add_event_cb(save, save_vid_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *save_label = lv_label_create(save);
    lv_label_set_text(save_label, ui_i18n_get(UI_STR_SET));
    lv_obj_set_style_text_color(save_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(save_label, ui_i18n_font_24(), 0);
    lv_obj_center(save_label);

    s_status = lv_label_create(screen);
    lv_obj_set_width(s_status, USB_VID_SETUP_WIDTH - 50);
    lv_label_set_long_mode(s_status, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(s_status, ui_i18n_font_24(), 0);
    lv_obj_align(s_status, LV_ALIGN_TOP_LEFT, 25, 262);

    uint16_t current_vid = usb_hid_get_vid();
    set_current_vid(current_vid);
    set_vid_field(current_vid);
    lv_obj_add_event_cb(s_vid_field, vid_field_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
    set_status(UI_STR_USB_VID_RANGE, lv_color_hex(0x9EA3AD));

    static const char *const keys[] = {
        "1", "2", "3", "A", "B", "\n",
        "4", "5", "6", "C", "D", "\n",
        "7", "8", "9", "E", "F", "\n",
        "0", LV_SYMBOL_BACKSPACE, ""
    };
    lv_obj_t *keypad = lv_buttonmatrix_create(screen);
    lv_buttonmatrix_set_map(keypad, keys);
    lv_obj_set_size(keypad, USB_VID_SETUP_WIDTH - 12, 336);
    lv_obj_align(keypad, LV_ALIGN_BOTTOM_MID, 0, -4);
    lv_obj_set_style_bg_opa(keypad, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(keypad, 0, 0);
    lv_obj_set_style_pad_all(keypad, 0, 0);
    lv_obj_set_style_pad_row(keypad, 4, 0);
    lv_obj_set_style_pad_column(keypad, 4, 0);
    lv_obj_set_style_radius(keypad, 3, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(keypad, lv_color_hex(0x292A32), LV_PART_ITEMS);
    lv_obj_set_style_bg_color(keypad, lv_color_hex(0x3A3C46), LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_border_color(keypad, lv_color_hex(0x1A1B22), LV_PART_ITEMS);
    lv_obj_set_style_border_width(keypad, 1, LV_PART_ITEMS);
    lv_obj_set_style_text_color(keypad, lv_color_white(), LV_PART_ITEMS);
    lv_obj_set_style_text_font(keypad, &lv_font_montserrat_32, LV_PART_ITEMS);
    lv_obj_add_event_cb(keypad, keypad_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
}

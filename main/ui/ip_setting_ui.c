#include "ip_setting_ui.h"

#include <string.h>

#include "esp_err.h"
#include "lvgl.h"
#include "ui/i18n/ui_i18n.h"
#include "ui_nav.h"
#include "ui_status_bar.h"
#include "usb_ncm/usb_ncm.h"

#define IP_SETTING_WIDTH 480

static lv_obj_t *s_ip_field;
static lv_obj_t *s_status;

static void set_status(ui_string_id_t id, lv_color_t color)
{
    lv_label_set_text(s_status, ui_i18n_get(id));
    lv_obj_set_style_text_color(s_status, color, 0);
}

static void save_ip_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }

    esp_err_t ret = usb_ncm_set_ip_address(lv_textarea_get_text(s_ip_field));
    if (ret == ESP_OK) {
        set_status(UI_STR_NIC_IP_SAVED, lv_color_hex(0xA2ED8B));
    } else if (ret == ESP_ERR_INVALID_ARG) {
        set_status(UI_STR_NIC_IP_INVALID, lv_color_hex(0xFF8B92));
    } else {
        set_status(UI_STR_FAILED, lv_color_hex(0xFF8B92));
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
        lv_textarea_delete_char(s_ip_field);
    } else {
        lv_textarea_add_text(s_ip_field, key);
    }
    lv_label_set_text(s_status, "IPv4  /24  DHCP");
    lv_obj_set_style_text_color(s_status, lv_color_hex(0x9EA3AD), 0);
}

static void back_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_CLICKED) {
        ui_nav_go_back();
    }
}

void ip_setting_ui_show(void)
{
    lv_obj_t *screen = lv_screen_active();
    lv_obj_clean(screen);
    ui_nav_enable_right_swipe(screen);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x11141A), 0);
    lv_obj_set_style_pad_all(screen, 0, 0);
    ui_status_bar_add(screen, IP_SETTING_WIDTH);

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
    lv_label_set_text(title, ui_i18n_get(UI_STR_NIC_IP));
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_set_style_text_font(title, ui_i18n_font_28(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 76, 72);

    s_status = lv_label_create(screen);
    lv_label_set_text(s_status, "IPv4  /24  DHCP");
    lv_obj_set_width(s_status, IP_SETTING_WIDTH - 50);
    lv_label_set_long_mode(s_status, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(s_status, lv_color_hex(0x9EA3AD), 0);
    lv_obj_set_style_text_font(s_status, ui_i18n_font_24(), 0);
    lv_obj_align(s_status, LV_ALIGN_TOP_LEFT, 25, 116);

    lv_obj_t *row = lv_obj_create(screen);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, IP_SETTING_WIDTH - 50, 72);
    lv_obj_align(row, LV_ALIGN_TOP_MID, 0, 180);
    lv_obj_set_style_bg_color(row, lv_color_hex(0x292A32), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(row, 4, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *label = lv_label_create(row);
    lv_label_set_text(label, ui_i18n_get(UI_STR_IP_ADDRESS));
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_set_style_text_font(label, ui_i18n_font_28(), 0);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, 14, 0);

    s_ip_field = lv_textarea_create(row);
    lv_obj_set_size(s_ip_field, 230, 56);
    lv_obj_align(s_ip_field, LV_ALIGN_LEFT_MID, 58, 0);
    lv_textarea_set_one_line(s_ip_field, true);
    lv_textarea_set_accepted_chars(s_ip_field, "0123456789.");
    lv_textarea_set_max_length(s_ip_field, USB_NCM_IP_ADDRESS_LEN - 1);
    char ip_address[USB_NCM_IP_ADDRESS_LEN];
    usb_ncm_get_ip_address(ip_address);
    lv_textarea_set_text(s_ip_field, ip_address);
    lv_obj_set_style_bg_color(s_ip_field, lv_color_hex(0x3A3C46), 0);
    lv_obj_set_style_border_color(s_ip_field, lv_color_hex(0x555965), 0);
    lv_obj_set_style_border_width(s_ip_field, 1, 0);
    lv_obj_set_style_radius(s_ip_field, 4, 0);
    lv_obj_set_style_text_color(s_ip_field, lv_color_white(), 0);
    lv_obj_set_style_text_font(s_ip_field, &lv_font_montserrat_24, 0);
    lv_obj_set_style_pad_left(s_ip_field, 10, 0);
    lv_obj_set_style_pad_right(s_ip_field, 8, 0);

    lv_obj_t *save = lv_button_create(row);
    lv_obj_set_size(save, 112, 56);
    lv_obj_align(save, LV_ALIGN_RIGHT_MID, -14, 0);
    lv_obj_set_style_bg_color(save, lv_color_hex(0x2997E8), 0);
    lv_obj_set_style_bg_color(save, lv_color_hex(0x197FC7), LV_STATE_PRESSED);
    lv_obj_set_style_radius(save, 6, 0);
    lv_obj_set_style_shadow_width(save, 0, 0);
    lv_obj_add_event_cb(save, save_ip_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *save_label = lv_label_create(save);
    lv_label_set_text(save_label, ui_i18n_get(UI_STR_SET));
    lv_obj_set_style_text_color(save_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(save_label, ui_i18n_font_24(), 0);
    lv_obj_center(save_label);

    static const char *const keys[] = {
        "1", "2", "3", "\n",
        "4", "5", "6", "\n",
        "7", "8", "9", "\n",
        ".", "0", LV_SYMBOL_BACKSPACE, ""
    };
    lv_obj_t *keypad = lv_buttonmatrix_create(screen);
    lv_buttonmatrix_set_map(keypad, keys);
    lv_obj_set_size(keypad, IP_SETTING_WIDTH - 12, 376);
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

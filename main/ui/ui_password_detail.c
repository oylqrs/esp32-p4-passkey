#include "ui_password_detail.h"

#include <stdio.h>

#include "esp_err.h"
#include "esp_log.h"
#include "kdbx/kdbx_loader.h"
#include "lvgl.h"
#include "ui_nav.h"
#include "ui_password_manager.h"
#include "usb_hid_keyboard.h"

#define PASSWORD_DETAIL_WIDTH 480
#define PASSWORD_DETAIL_PANEL_Y 92
#define PASSWORD_DETAIL_PANEL_HEIGHT 532

static const char *TAG = "ui_password_detail";
static size_t s_entry_index;

static void field_send_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }

    const char *value_text = lv_event_get_user_data(event);
    if (!value_text || !value_text[0]) {
        return;
    }

    esp_err_t ret = usb_hid_keyboard_type_text(value_text);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to type password field: %s", esp_err_to_name(ret));
    }
}

static void add_field(lv_obj_t *parent,
                      const char *symbol,
                      const char *label_text,
                      const char *value_text,
                      bool password_mode,
                      bool send_on_click,
                      lv_color_t accent)
{
    lv_obj_t *field = lv_obj_create(parent);
    lv_obj_remove_style_all(field);
    lv_obj_clear_flag(field, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_CHAIN);
    lv_obj_set_width(field, PASSWORD_DETAIL_WIDTH - 82);
    lv_obj_set_height(field, 112);
    lv_obj_set_style_bg_color(field, lv_color_hex(0x20232B), 0);
    lv_obj_set_style_bg_opa(field, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(field, lv_color_hex(0x292C36), LV_STATE_PRESSED);
    lv_obj_set_style_radius(field, 8, 0);
    lv_obj_set_style_border_color(field, lv_color_hex(0x343844), 0);
    lv_obj_set_style_border_width(field, 1, 0);
    lv_obj_set_style_border_color(field, accent, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(field, 2, LV_STATE_PRESSED);
    lv_obj_set_style_pad_left(field, 16, 0);
    lv_obj_set_style_pad_right(field, 16, 0);
    lv_obj_set_style_pad_top(field, 12, 0);
    lv_obj_set_style_pad_bottom(field, 12, 0);

    if (send_on_click && value_text && value_text[0]) {
        lv_obj_add_flag(field, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(field, field_send_event_cb, LV_EVENT_CLICKED, (void *)value_text);
    }

    lv_obj_t *icon_box = lv_obj_create(field);
    lv_obj_remove_style_all(icon_box);
    lv_obj_clear_flag(icon_box, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_CHAIN);
    lv_obj_set_size(icon_box, 56, 56);
    lv_obj_align(icon_box, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_bg_color(icon_box, accent, 0);
    lv_obj_set_style_bg_opa(icon_box, LV_OPA_20, 0);
    lv_obj_set_style_radius(icon_box, 28, 0);
    lv_obj_clear_flag(icon_box, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *icon = lv_label_create(icon_box);
    lv_label_set_text(icon, symbol);
    lv_obj_set_style_text_color(icon, accent, 0);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_28, 0);
    lv_obj_center(icon);

    lv_obj_t *content = lv_obj_create(field);
    lv_obj_remove_style_all(content);
    lv_obj_clear_flag(content, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_CHAIN);
    lv_obj_set_size(content, 270, 82);
    lv_obj_align(content, LV_ALIGN_LEFT_MID, 72, 0);
    lv_obj_set_style_pad_all(content, 0, 0);
    lv_obj_clear_flag(content, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *label = lv_label_create(field);
    lv_label_set_text(label, label_text);
    lv_obj_set_style_text_color(label, lv_color_hex(0x9EA3AD), 0);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_16, 0);
    lv_obj_align_to(label, content, LV_ALIGN_TOP_LEFT, 0, 0);
    if (send_on_click && value_text && value_text[0]) {
        lv_obj_add_flag(label, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(label, field_send_event_cb, LV_EVENT_CLICKED, (void *)value_text);
    }

    lv_obj_t *value = lv_label_create(content);
    lv_label_set_text(value, value_text && value_text[0] ? value_text : "-");
    lv_obj_set_width(value, 270);
    lv_label_set_long_mode(value, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(value, password_mode ? lv_color_hex(0xFFC958) : lv_color_white(), 0);
    lv_obj_set_style_text_font(value, &lv_font_montserrat_24, 0);
    lv_obj_align(value, LV_ALIGN_TOP_LEFT, 0, 28);
    if (send_on_click && value_text && value_text[0]) {
        lv_obj_add_flag(value, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(value, field_send_event_cb, LV_EVENT_CLICKED, (void *)value_text);
    }

    if (send_on_click && value_text && value_text[0]) {
        lv_obj_t *send_icon = lv_label_create(field);
        lv_label_set_text(send_icon, LV_SYMBOL_COPY);
        lv_obj_set_style_text_color(send_icon, lv_color_hex(0x7F8592), 0);
        lv_obj_set_style_text_font(send_icon, &lv_font_montserrat_24, 0);
        lv_obj_align(send_icon, LV_ALIGN_RIGHT_MID, 0, 0);
        lv_obj_add_flag(send_icon, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(send_icon, field_send_event_cb, LV_EVENT_CLICKED, (void *)value_text);
    }
}

void ui_password_detail_show(void)
{
    const kdbx_entry_t *entry = kdbx_get_entry(s_entry_index);

    lv_obj_t *screen = lv_screen_active();
    lv_obj_clean(screen);
    ui_nav_enable_right_swipe(screen);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x11141A), 0);
    lv_obj_set_style_pad_all(screen, 0, 0);

    lv_obj_t *header = lv_obj_create(screen);
    lv_obj_remove_style_all(header);
    lv_obj_set_size(header, PASSWORD_DETAIL_WIDTH - 48, 42);
    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 20);
    lv_obj_add_flag(header, LV_OBJ_FLAG_GESTURE_BUBBLE);

    lv_obj_t *header_icon = lv_label_create(header);
    lv_label_set_text(header_icon, LV_SYMBOL_KEYBOARD);
    lv_obj_set_style_text_color(header_icon, lv_color_hex(0x6DD6F2), 0);
    lv_obj_set_style_text_font(header_icon, &lv_font_montserrat_24, 0);
    lv_obj_align(header_icon, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_add_flag(header_icon, LV_OBJ_FLAG_GESTURE_BUBBLE);

    lv_obj_t *title = lv_label_create(screen);
    lv_label_set_text(title, entry ? entry->title : "Password Detail");
    lv_obj_set_width(title, PASSWORD_DETAIL_WIDTH - 80);
    lv_label_set_long_mode(title, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 56, 22);
    lv_obj_add_flag(title, LV_OBJ_FLAG_GESTURE_BUBBLE);

    lv_obj_t *subtitle = lv_label_create(screen);
    lv_label_set_text(subtitle, "KDBX credential");
    lv_obj_set_style_text_color(subtitle, lv_color_hex(0x7F8592), 0);
    lv_obj_set_style_text_font(subtitle, &lv_font_montserrat_16, 0);
    lv_obj_align(subtitle, LV_ALIGN_TOP_LEFT, 56, 52);
    lv_obj_add_flag(subtitle, LV_OBJ_FLAG_GESTURE_BUBBLE);

    lv_obj_t *panel = lv_obj_create(screen);
    lv_obj_remove_style_all(panel);
    lv_obj_set_size(panel, PASSWORD_DETAIL_WIDTH - 48, PASSWORD_DETAIL_PANEL_HEIGHT);
    lv_obj_align(panel, LV_ALIGN_TOP_MID, 0, PASSWORD_DETAIL_PANEL_Y);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x181B22), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(panel, 10, 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(0x2B2E38), 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_pad_left(panel, 17, 0);
    lv_obj_set_style_pad_right(panel, 17, 0);
    lv_obj_set_style_pad_top(panel, 18, 0);
    lv_obj_set_style_pad_bottom(panel, 18, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_CHAIN);
    lv_obj_add_flag(panel, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(panel, 12, 0);

    if (!entry) {
        add_field(panel, LV_SYMBOL_CLOSE, "Error", "Entry not found", false, false,
                  lv_color_hex(0xF27A7A));
        return;
    }

    add_field(panel, LV_SYMBOL_EDIT, "Username", entry->username, false, true,
              lv_color_hex(0xBAEEFF));
    add_field(panel, LV_SYMBOL_DIRECTORY, "URL", entry->url, false, true,
              lv_color_hex(0x6DD6F2));
    add_field(panel, LV_SYMBOL_EYE_CLOSE,
              entry->password_protected ? "Password (protected)" : "Password",
              entry->password,
              true,
              true,
              lv_color_hex(0xFFC958));
}

void ui_password_detail_open(size_t entry_index)
{
    ui_password_detail_set_entry(entry_index);
    ui_nav_push(ui_password_manager_show, ui_password_detail_show);
}

void ui_password_detail_set_entry(size_t entry_index)
{
    s_entry_index = entry_index;
}

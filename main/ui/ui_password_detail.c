#include "ui_password_detail.h"

#include <stdio.h>
#include <stdint.h>

#include "PCF85063/pcf85063.h"
#include "Qrcode/qrcode_text.h"
#include "hid_keyboard.h"
#include "kdbx/kdbx_loader.h"
#include "kdbx/kdbx_totp.h"
#include "lvgl.h"
#include "setup_ui.h"
#include "timezone_setup_ui.h"
#include "ui/i18n/ui_i18n.h"
#include "ui_nav.h"
#include "ui_password_manager.h"
#include "ui_status_bar.h"

#define PASSWORD_DETAIL_WIDTH        480
#define PASSWORD_DETAIL_PANEL_WIDTH  PASSWORD_DETAIL_WIDTH
#define PASSWORD_DETAIL_PANEL_HEIGHT (640 - UI_STATUS_BAR_HEIGHT)
#define PASSWORD_DETAIL_PANEL_Y      UI_STATUS_BAR_HEIGHT
#define PASSWORD_DETAIL_PANEL_PAD_X  16
#define PASSWORD_DETAIL_LABEL_MAX_WIDTH 170
#define PASSWORD_DETAIL_FIELD_GAP    30
#define PASSWORD_DETAIL_CONTENT_WIDTH (PASSWORD_DETAIL_PANEL_WIDTH - \
                                       PASSWORD_DETAIL_PANEL_PAD_X * 2)
#define PASSWORD_DETAIL_ROW_HEIGHT   88
#define PASSWORD_DETAIL_ROW_PAD_Y    10
#define PASSWORD_DETAIL_QR_SIZE      360
#define PASSWORD_DETAIL_QR_TEXT_MAX  128
#define PASSWORD_DETAIL_QR_TITLE_MAX 64
#define PASSWORD_DETAIL_FIELD_COUNT  6
#define PASSWORD_DETAIL_DRAG_THRESHOLD 10

typedef struct {
    const char *title;
    const char *send_value;
    const char *qr_value;
} password_field_event_data_t;

static size_t s_entry_index;
static password_field_event_data_t s_field_event_data[PASSWORD_DETAIL_FIELD_COUNT];
static size_t s_field_event_count;
static bool s_qr_opening;
static char s_qr_text[PASSWORD_DETAIL_QR_TEXT_MAX];
static char s_qr_title[PASSWORD_DETAIL_QR_TITLE_MAX];
static lv_obj_t *s_totp_label;
static lv_timer_t *s_totp_timer;
static char s_totp_code[9];
static lv_obj_t *s_pressed_field;
static lv_point_t s_field_press_point;
static bool s_field_dragged;

static int64_t days_from_civil(int year, unsigned month, unsigned day)
{
    year -= month <= 2;
    int era = (year >= 0 ? year : year - 399) / 400;
    unsigned year_of_era = (unsigned)(year - era * 400);
    unsigned adjusted_month = month > 2 ? month - 3 : month + 9;
    unsigned day_of_year = (153U * adjusted_month + 2U) / 5U + day - 1U;
    unsigned day_of_era = year_of_era * 365U + year_of_era / 4U -
                          year_of_era / 100U + day_of_year;
    return (int64_t)era * 146097 + (int64_t)day_of_era - 719468;
}

static bool current_unix_time(uint64_t *unix_time)
{
    pcf85063_time_t rtc;
    if (!unix_time || pcf85063_get_time(&rtc) != ESP_OK || rtc.voltage_low ||
        rtc.year < 2000 || rtc.month < 1 || rtc.month > 12 ||
        rtc.day < 1 || rtc.day > 31 || rtc.hour > 23 ||
        rtc.minute > 59 || rtc.second > 59) {
        return false;
    }

    int64_t local_seconds = days_from_civil(rtc.year, rtc.month, rtc.day) * 86400 +
                            rtc.hour * 3600 + rtc.minute * 60 + rtc.second;
    int64_t utc_seconds = local_seconds -
                          (int64_t)timezone_setup_get_utc_offset_minutes() * 60;
    if (utc_seconds < 0) return false;
    *unix_time = (uint64_t)utc_seconds;
    return true;
}

static void totp_update(void)
{
    const kdbx_entry_t *entry = kdbx_get_entry(s_entry_index);
    uint64_t unix_time;
    uint32_t remaining;
    if (!s_totp_label || !entry || !current_unix_time(&unix_time) ||
        kdbx_totp_generate_at(entry, unix_time, s_totp_code,
                              sizeof(s_totp_code), &remaining) != ESP_OK) {
        s_totp_code[0] = '\0';
        if (s_totp_label) lv_label_set_text(s_totp_label, "------");
        return;
    }
    lv_label_set_text_fmt(s_totp_label, "%s  (%us)", s_totp_code,
                          (unsigned)remaining);
}

static void totp_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    totp_update();
}

static void totp_label_delete_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_DELETE) return;
    s_totp_label = NULL;
    if (s_totp_timer) {
        lv_timer_delete(s_totp_timer);
        s_totp_timer = NULL;
    }
}

static void password_qrcode_show(void)
{
    lv_obj_t *screen = lv_screen_active();
    lv_obj_clean(screen);
    ui_nav_enable_right_swipe(screen);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x0D1016), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(screen, 0, 0);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    ui_status_bar_add(screen, PASSWORD_DETAIL_WIDTH);

    lv_obj_t *title = lv_label_create(screen);
    lv_label_set_text(title, s_qr_title);
    lv_obj_set_width(title, PASSWORD_DETAIL_WIDTH - 32);
    lv_label_set_long_mode(title, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_set_style_text_font(title, ui_i18n_font_35(), 0);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, UI_STATUS_BAR_HEIGHT + 12);
    lv_obj_add_flag(title, LV_OBJ_FLAG_GESTURE_BUBBLE);

    qrcode_text_config_t config = {
        .size = PASSWORD_DETAIL_QR_SIZE,
        .quiet_zone = 14,
        .dark_color = LV_COLOR_MAKE(0x00, 0x00, 0x00),
        .light_color = LV_COLOR_MAKE(0xFF, 0xFF, 0xFF),
    };
    lv_obj_t *qrcode = qrcode_text_create_with_config(screen, s_qr_text, &config);
    if (qrcode) {
        lv_obj_align(qrcode, LV_ALIGN_TOP_MID, 0, UI_STATUS_BAR_HEIGHT + 68);
        lv_obj_add_flag(qrcode, LV_OBJ_FLAG_GESTURE_BUBBLE);
    } else {
        lv_obj_t *error = lv_label_create(screen);
        lv_label_set_text(error, ui_i18n_get(UI_STR_ERROR));
        lv_obj_set_style_text_color(error, lv_color_hex(0xFF6B6B), 0);
        lv_obj_set_style_text_font(error, ui_i18n_font_35(), 0);
        lv_obj_center(error);
        lv_obj_add_flag(error, LV_OBJ_FLAG_GESTURE_BUBBLE);
    }
}

static void field_event_cb(lv_event_t *event)
{
    password_field_event_data_t *data = lv_event_get_user_data(event);
    if (!data) {
        return;
    }

    lv_event_code_t code = lv_event_get_code(event);
    lv_obj_t *target = lv_event_get_target(event);

    if (code == LV_EVENT_PRESSED) {
        lv_indev_t *indev = lv_indev_active();
        s_pressed_field = target;
        s_field_dragged = false;
        if (indev) {
            lv_indev_get_point(indev, &s_field_press_point);
        }
        return;
    }

    if (code == LV_EVENT_PRESSING && target == s_pressed_field) {
        lv_indev_t *indev = lv_indev_active();
        if (indev) {
            lv_point_t point;
            lv_indev_get_point(indev, &point);
            int32_t delta_x = point.x - s_field_press_point.x;
            int32_t delta_y = point.y - s_field_press_point.y;
            if (delta_x > PASSWORD_DETAIL_DRAG_THRESHOLD ||
                delta_x < -PASSWORD_DETAIL_DRAG_THRESHOLD ||
                delta_y > PASSWORD_DETAIL_DRAG_THRESHOLD ||
                delta_y < -PASSWORD_DETAIL_DRAG_THRESHOLD) {
                s_field_dragged = true;
            }
        }
        return;
    }

    if (code == LV_EVENT_PRESS_LOST) {
        if (target == s_pressed_field) {
            s_pressed_field = NULL;
            s_field_dragged = false;
        }
        return;
    }

    if (code == LV_EVENT_LONG_PRESSED) {
        if (target != s_pressed_field || s_field_dragged || s_qr_opening ||
            !data->qr_value || !data->qr_value[0]) {
            return;
        }

        snprintf(s_qr_title, sizeof(s_qr_title), "%s", data->title);
        snprintf(s_qr_text, sizeof(s_qr_text), "%s", data->qr_value);
        s_pressed_field = NULL;
        s_field_dragged = false;
        s_qr_opening = true;
        ui_nav_push(ui_password_detail_show, password_qrcode_show);
        return;
    }

    if (code == LV_EVENT_CLICKED) {
        bool ignore_click = target != s_pressed_field || s_field_dragged;
        s_pressed_field = NULL;
        s_field_dragged = false;
        if (!ignore_click && !s_qr_opening &&
            data->send_value && data->send_value[0]) {
            hid_type_string((char *)data->send_value);
        }
    }
}

static lv_obj_t *add_field(lv_obj_t *parent,
                           const char *label_text,
                           const char *value_text,
                           const char *send_value,
                           const char *qr_value,
                           int32_t height)
{
    const char *display_value = value_text && value_text[0] ? value_text : "-";
    const lv_font_t *font = ui_i18n_font_35();
    lv_point_t label_size;
    lv_point_t value_size;
    lv_text_get_size(&label_size,
                     label_text,
                     font,
                     0,
                     2,
                     PASSWORD_DETAIL_LABEL_MAX_WIDTH,
                     LV_TEXT_FLAG_NONE);
    int32_t label_width = LV_MIN(label_size.x, PASSWORD_DETAIL_LABEL_MAX_WIDTH);
    int32_t value_x = label_width + PASSWORD_DETAIL_FIELD_GAP;
    int32_t value_width = PASSWORD_DETAIL_CONTENT_WIDTH - value_x;
    lv_text_get_size(&value_size,
                     display_value,
                     font,
                     0,
                     2,
                     value_width,
                     LV_TEXT_FLAG_NONE);
    int32_t text_height = LV_MAX(label_size.y, value_size.y);
    int32_t field_height = LV_MAX(height, text_height + 20);

    lv_obj_t *field = lv_obj_create(parent);
    lv_obj_remove_style_all(field);
    lv_obj_clear_flag(field, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_CHAIN);
    lv_obj_set_size(field, LV_PCT(100), field_height);
    lv_obj_set_style_bg_opa(field, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(field, lv_color_hex(0x303139), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(field, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_radius(field, 4, LV_STATE_PRESSED);
    lv_obj_set_style_pad_all(field, 0, 0);
    lv_obj_add_flag(field, LV_OBJ_FLAG_GESTURE_BUBBLE);

    if (send_value || qr_value) {
        lv_obj_add_flag(field, LV_OBJ_FLAG_CLICKABLE);
        if (s_field_event_count < PASSWORD_DETAIL_FIELD_COUNT) {
            password_field_event_data_t *data =
                &s_field_event_data[s_field_event_count++];
            data->title = label_text;
            data->send_value = send_value;
            data->qr_value = qr_value;
            lv_obj_add_event_cb(field, field_event_cb, LV_EVENT_ALL, data);
        }
    }

    lv_obj_t *label = lv_label_create(field);
    lv_label_set_text(label, label_text);
    lv_obj_set_width(label, label_width);
    lv_obj_set_height(label, LV_SIZE_CONTENT);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_line_space(label, 2, 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, 0, PASSWORD_DETAIL_ROW_PAD_Y);
    lv_obj_add_flag(label, LV_OBJ_FLAG_GESTURE_BUBBLE);

    lv_obj_t *value = lv_label_create(field);
    lv_label_set_text(value, display_value);
    lv_obj_set_width(value, value_width);
    lv_obj_set_height(value, LV_SIZE_CONTENT);
    lv_label_set_long_mode(value, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(value, lv_color_hex(0xD6D7DC), 0);
    lv_obj_set_style_text_font(value, font, 0);
    lv_obj_set_style_text_line_space(value, 2, 0);
    lv_obj_set_style_text_align(value, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_align(value, LV_ALIGN_TOP_LEFT, value_x, PASSWORD_DETAIL_ROW_PAD_Y);
    lv_obj_add_flag(value, LV_OBJ_FLAG_GESTURE_BUBBLE);
    return value;
}

void ui_password_detail_show(void)
{
    const kdbx_entry_t *entry = kdbx_get_entry(s_entry_index);
    if (s_totp_timer) {
        lv_timer_delete(s_totp_timer);
        s_totp_timer = NULL;
    }
    s_totp_label = NULL;
    s_totp_code[0] = '\0';
    s_field_event_count = 0;
    s_qr_opening = false;
    s_pressed_field = NULL;
    s_field_dragged = false;

    lv_obj_t *screen = lv_screen_active();
    lv_obj_clean(screen);
    ui_nav_enable_right_swipe(screen);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x0D1016), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(screen, 0, 0);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    ui_status_bar_add(screen, PASSWORD_DETAIL_WIDTH);

    lv_obj_t *panel = lv_obj_create(screen);
    lv_obj_remove_style_all(panel);
    lv_obj_set_size(panel, PASSWORD_DETAIL_PANEL_WIDTH, PASSWORD_DETAIL_PANEL_HEIGHT);
    lv_obj_align(panel, LV_ALIGN_TOP_MID, 0, PASSWORD_DETAIL_PANEL_Y);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x25262E), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(panel, 0, 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_style_pad_left(panel, PASSWORD_DETAIL_PANEL_PAD_X, 0);
    lv_obj_set_style_pad_right(panel, PASSWORD_DETAIL_PANEL_PAD_X, 0);
    lv_obj_set_style_pad_top(panel, 4, 0);
    lv_obj_set_style_pad_bottom(panel, 12, 0);
    lv_obj_add_flag(panel, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(panel, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(panel, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(panel, 0, 0);

    if (!entry) {
        add_field(panel,
                  ui_i18n_get(UI_STR_ERROR),
                  ui_i18n_get(UI_STR_ENTRY_NOT_FOUND),
                  NULL,
                  NULL,
                  PASSWORD_DETAIL_ROW_HEIGHT);
        return;
    }

    const char *password_display = setup_ui_get_hide_password() ? "********" : entry->password;
    add_field(panel, ui_i18n_get(UI_STR_FIELD_TITLE), entry->title, NULL, entry->title,
              PASSWORD_DETAIL_ROW_HEIGHT);
    add_field(panel, ui_i18n_get(UI_STR_ACCOUNT), entry->username, entry->username,
              entry->username,
              PASSWORD_DETAIL_ROW_HEIGHT);
    add_field(panel, ui_i18n_get(UI_STR_PASSWORD), password_display, entry->password,
              entry->password,
              PASSWORD_DETAIL_ROW_HEIGHT);
    if (kdbx_totp_is_configured(entry)) {
        s_totp_label = add_field(panel, "TOTP", "------", s_totp_code, s_totp_code,
                                 PASSWORD_DETAIL_ROW_HEIGHT);
        lv_obj_add_event_cb(s_totp_label, totp_label_delete_cb, LV_EVENT_DELETE, NULL);
        totp_update();
        s_totp_timer = lv_timer_create(totp_timer_cb, 1000, NULL);
    }
    add_field(panel, ui_i18n_get(UI_STR_GROUP), entry->group, NULL, entry->group,
              PASSWORD_DETAIL_ROW_HEIGHT);
    add_field(panel, ui_i18n_get(UI_STR_WEBSITE), entry->url, entry->url, entry->url,
              PASSWORD_DETAIL_ROW_HEIGHT);
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

#include "date_time_setting_ui.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "PCF85063/pcf85063.h"
#include "esp_err.h"
#include "esp_log.h"
#include "log_storage.h"
#include "lvgl.h"
#include "ui/i18n/ui_i18n.h"
#include "ui_nav.h"
#include "ui_status_bar.h"

#define DATE_TIME_WIDTH 480
#define YEAR_FIRST      2000
#define YEAR_LAST       2099

static const char *TAG = "date_time_ui";
static lv_obj_t *s_year;
static lv_obj_t *s_month;
static lv_obj_t *s_day;
static lv_obj_t *s_hour;
static lv_obj_t *s_minute;
static lv_obj_t *s_second;
static lv_obj_t *s_status;

static char s_year_options[500];
static char s_month_options[36];
static char s_day_options[93];
static char s_hour_options[72];
static char s_minute_options[180];
static char s_second_options[180];

static void make_number_options(char *buffer, size_t size,
                                unsigned first, unsigned last, unsigned digits)
{
    size_t used = 0;
    buffer[0] = '\0';
    for (unsigned value = first; value <= last; value++) {
        int written = snprintf(buffer + used, size - used, "%0*u%s",
                               digits, value, value == last ? "" : "\n");
        if (written < 0 || (size_t)written >= size - used) {
            break;
        }
        used += (size_t)written;
    }
}

static bool is_leap_year(unsigned year)
{
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

static unsigned days_in_month(unsigned year, unsigned month)
{
    static const uint8_t days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month == 2 && is_leap_year(year)) {
        return 29;
    }
    return days[month - 1];
}

static uint8_t weekday_for_date(unsigned year, unsigned month, unsigned day)
{
    static const uint8_t offsets[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    if (month < 3) {
        year--;
    }
    return (uint8_t)((year + year / 4 - year / 100 + year / 400 +
                      offsets[month - 1] + day) % 7);
}

static void refresh_day_options(void)
{
    unsigned year = YEAR_FIRST + lv_roller_get_selected(s_year);
    unsigned month = 1 + lv_roller_get_selected(s_month);
    unsigned selected_day = 1 + lv_roller_get_selected(s_day);
    unsigned maximum = days_in_month(year, month);
    if (selected_day > maximum) {
        selected_day = maximum;
    }
    make_number_options(s_day_options, sizeof(s_day_options), 1, maximum, 2);
    lv_roller_set_options(s_day, s_day_options, LV_ROLLER_MODE_NORMAL);
    lv_roller_set_selected(s_day, selected_day - 1, LV_ANIM_OFF);
}

static void date_changed_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_VALUE_CHANGED) {
        refresh_day_options();
    }
}

static lv_obj_t *create_roller(lv_obj_t *row, int32_t width, int32_t x,
                               const char *options, uint32_t selected)
{
    lv_obj_t *roller = lv_roller_create(row);
    lv_roller_set_options(roller, options, LV_ROLLER_MODE_NORMAL);
    lv_roller_set_visible_row_count(roller, 3);
    lv_roller_set_selected(roller, selected, LV_ANIM_OFF);
    lv_obj_set_size(roller, width, 106);
    lv_obj_align(roller, LV_ALIGN_RIGHT_MID, x, 0);
    lv_obj_set_style_bg_color(roller, lv_color_hex(0x292A32), 0);
    lv_obj_set_style_bg_opa(roller, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(roller, 0, 0);
    lv_obj_set_style_radius(roller, 4, 0);
    lv_obj_set_style_text_color(roller, lv_color_hex(0x8D919C), 0);
    lv_obj_set_style_text_font(roller, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_align(roller, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_bg_color(roller, lv_color_hex(0x2997E8), LV_PART_SELECTED);
    lv_obj_set_style_text_color(roller, lv_color_white(), LV_PART_SELECTED);
    lv_obj_set_style_text_font(roller, &lv_font_montserrat_28, LV_PART_SELECTED);
    return roller;
}

static lv_obj_t *create_row(lv_obj_t *parent, int32_t y, ui_string_id_t label_id)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, DATE_TIME_WIDTH - 50, 116);
    lv_obj_align(row, LV_ALIGN_TOP_MID, 0, y);
    lv_obj_set_style_bg_color(row, lv_color_hex(0x292A32), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(row, 4, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *label = lv_label_create(row);
    lv_label_set_text(label, ui_i18n_get(label_id));
    lv_obj_set_width(label, 92);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_set_style_text_font(label, ui_i18n_font_28(), 0);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, 14, 0);
    return row;
}

static void save_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }

    pcf85063_time_t time = {
        .year = YEAR_FIRST + lv_roller_get_selected(s_year),
        .month = 1 + lv_roller_get_selected(s_month),
        .day = 1 + lv_roller_get_selected(s_day),
        .hour = lv_roller_get_selected(s_hour),
        .minute = lv_roller_get_selected(s_minute),
        .second = lv_roller_get_selected(s_second),
    };
    time.weekday = weekday_for_date(time.year, time.month, time.day);
    esp_err_t ret = pcf85063_set_time(&time);
    if (ret == ESP_OK) {
        log_storage_refresh_time();
        lv_label_set_text(s_status, ui_i18n_get(UI_STR_DATE_TIME_SAVED));
        lv_obj_set_style_text_color(s_status, lv_color_hex(0xA2ED8B), 0);
    } else {
        ESP_LOGW(TAG, "Failed to set RTC: %s", esp_err_to_name(ret));
        lv_label_set_text(s_status, ui_i18n_get(UI_STR_DATE_TIME_FAILED));
        lv_obj_set_style_text_color(s_status, lv_color_hex(0xFF8B92), 0);
    }
}

static void back_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_CLICKED) {
        ui_nav_go_back();
    }
}

void date_time_setting_ui_show(void)
{
    pcf85063_time_t time = {
        .year = 2026, .month = 1, .day = 1,
    };
    if (pcf85063_get_time(&time) != ESP_OK) {
        time.hour = 0;
        time.minute = 0;
        time.second = 0;
    }

    make_number_options(s_year_options, sizeof(s_year_options), YEAR_FIRST, YEAR_LAST, 4);
    make_number_options(s_month_options, sizeof(s_month_options), 1, 12, 2);
    make_number_options(s_hour_options, sizeof(s_hour_options), 0, 23, 2);
    make_number_options(s_minute_options, sizeof(s_minute_options), 0, 59, 2);
    make_number_options(s_second_options, sizeof(s_second_options), 0, 59, 2);
    make_number_options(s_day_options, sizeof(s_day_options), 1,
                        days_in_month(time.year, time.month), 2);

    lv_obj_t *screen = lv_screen_active();
    lv_obj_clean(screen);
    ui_nav_enable_right_swipe(screen);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x11141A), 0);
    lv_obj_set_style_pad_all(screen, 0, 0);
    ui_status_bar_add(screen, DATE_TIME_WIDTH);

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
    lv_label_set_text(title, ui_i18n_get(UI_STR_DATE_TIME_SETTING));
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_set_style_text_font(title, ui_i18n_font_28(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 76, 72);

    lv_obj_t *year_row = create_row(screen, 124, UI_STR_YEAR);
    s_year = create_roller(year_row, 278, -14, s_year_options, time.year - YEAR_FIRST);
    lv_obj_add_event_cb(s_year, date_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *date_row = create_row(screen, 246, UI_STR_MONTH_DAY);
    s_month = create_roller(date_row, 132, -160, s_month_options, time.month - 1);
    s_day = create_roller(date_row, 132, -14, s_day_options, time.day - 1);
    lv_obj_add_event_cb(s_month, date_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *time_row = create_row(screen, 368, UI_STR_HOUR_MINUTE_SECOND);
    s_hour = create_roller(time_row, 84, -208, s_hour_options, time.hour);
    s_minute = create_roller(time_row, 84, -111, s_minute_options, time.minute);
    s_second = create_roller(time_row, 84, -14, s_second_options, time.second);

    s_status = lv_label_create(screen);
    lv_label_set_text(s_status, "");
    lv_obj_set_width(s_status, 240);
    lv_obj_set_style_text_align(s_status, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_text_font(s_status, ui_i18n_font_16(), 0);
    lv_obj_align(s_status, LV_ALIGN_BOTTOM_LEFT, 25, -31);

    lv_obj_t *save = lv_button_create(screen);
    lv_obj_set_size(save, 132, 58);
    lv_obj_align(save, LV_ALIGN_BOTTOM_RIGHT, -25, -14);
    lv_obj_set_style_bg_color(save, lv_color_hex(0x2997E8), 0);
    lv_obj_set_style_bg_color(save, lv_color_hex(0x197FC7), LV_STATE_PRESSED);
    lv_obj_set_style_radius(save, 6, 0);
    lv_obj_set_style_shadow_width(save, 0, 0);
    lv_obj_add_event_cb(save, save_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *save_label = lv_label_create(save);
    lv_label_set_text(save_label, ui_i18n_get(UI_STR_SAVE));
    lv_obj_set_style_text_color(save_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(save_label, ui_i18n_font_24(), 0);
    lv_obj_center(save_label);
}

#include "timezone_setup_ui.h"

#include <stdbool.h>
#include <stdint.h>

#include "PCF85063/pcf85063.h"
#include "esp_err.h"
#include "esp_log.h"
#include "log_storage.h"
#include "lvgl.h"
#include "nvs.h"
#include "ui/i18n/ui_i18n.h"
#include "ui/ttf/ui_fonts.h"
#include "ui_nav.h"
#include "ui_status_bar.h"

#define TIMEZONE_UI_WIDTH             480
#define TIMEZONE_NVS_NAMESPACE        "timezone_ui"
#define TIMEZONE_NVS_SELECTED_KEY     "selected"
#define TIMEZONE_NVS_SHOW_ABBR_KEY    "show_abbr"
#define TIMEZONE_OPTION_COUNT         10
#define TIMEZONE_DEFAULT_INDEX        6

static const char *TAG = "timezone_ui";

static const int16_t s_utc_offset_minutes[TIMEZONE_OPTION_COUNT] = {
    -8 * 60, -5 * 60, 0, 1 * 60, 3 * 60,
    5 * 60 + 30, 8 * 60, 9 * 60, 10 * 60, 12 * 60,
};

static const char *const s_abbreviations[TIMEZONE_OPTION_COUNT] = {
    "PST", "EST", "GMT", "CET", "MSK",
    "IST", "CST", "JST", "AEST", "NZST",
};

static const char *const s_options_zh =
    "PST  | UTC-8     | 洛杉矶\n"
    "EST  | UTC-5     | 纽约\n"
    "GMT  | UTC+0     | 伦敦\n"
    "CET  | UTC+1     | 柏林、巴黎\n"
    "MSK  | UTC+3     | 莫斯科\n"
    "IST  | UTC+5:30  | 新德里\n"
    "CST  | UTC+8     | 北京、上海\n"
    "JST  | UTC+9     | 东京\n"
    "AEST | UTC+10    | 悉尼\n"
    "NZST | UTC+12    | 惠灵顿";

static const char *const s_options_en =
    "PST  | UTC-8     | Los Angeles\n"
    "EST  | UTC-5     | New York\n"
    "GMT  | UTC+0     | London\n"
    "CET  | UTC+1     | Berlin, Paris\n"
    "MSK  | UTC+3     | Moscow\n"
    "IST  | UTC+5:30  | New Delhi\n"
    "CST  | UTC+8     | Beijing, Shanghai\n"
    "JST  | UTC+9     | Tokyo\n"
    "AEST | UTC+10    | Sydney\n"
    "NZST | UTC+12    | Wellington";

static lv_obj_t *s_status;
static uint8_t s_selected = TIMEZONE_DEFAULT_INDEX;
static bool s_selected_loaded;
static bool s_show_abbreviation = true;
static bool s_show_abbreviation_loaded;

static bool is_leap_year(uint16_t year)
{
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

static uint8_t days_in_month(uint16_t year, uint8_t month)
{
    static const uint8_t days[] = {
        31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31,
    };
    return month == 2 && is_leap_year(year) ? 29 : days[month - 1];
}

static uint8_t weekday_for_date(uint16_t year, uint8_t month, uint8_t day)
{
    static const uint8_t offsets[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    uint16_t adjusted_year = month < 3 ? year - 1 : year;
    return (uint8_t)((adjusted_year + adjusted_year / 4 - adjusted_year / 100 +
                      adjusted_year / 400 + offsets[month - 1] + day) % 7);
}

static esp_err_t shift_time_minutes(pcf85063_time_t *time, int16_t delta_minutes)
{
    if (!time || time->year < 2000 || time->year > 2099 ||
        time->month < 1 || time->month > 12 || time->day < 1 ||
        time->day > days_in_month(time->year, time->month) ||
        time->hour > 23 || time->minute > 59) {
        return ESP_ERR_INVALID_ARG;
    }

    int32_t minutes = (int32_t)time->hour * 60 + time->minute + delta_minutes;
    int8_t day_delta = 0;
    while (minutes < 0) {
        minutes += 24 * 60;
        day_delta--;
    }
    while (minutes >= 24 * 60) {
        minutes -= 24 * 60;
        day_delta++;
    }

    while (day_delta < 0) {
        if (time->day > 1) {
            time->day--;
        } else {
            if (time->month > 1) {
                time->month--;
            } else {
                if (time->year == 2000) {
                    return ESP_ERR_INVALID_STATE;
                }
                time->year--;
                time->month = 12;
            }
            time->day = days_in_month(time->year, time->month);
        }
        day_delta++;
    }

    while (day_delta > 0) {
        uint8_t maximum_day = days_in_month(time->year, time->month);
        if (time->day < maximum_day) {
            time->day++;
        } else {
            time->day = 1;
            if (time->month < 12) {
                time->month++;
            } else {
                if (time->year == 2099) {
                    return ESP_ERR_INVALID_STATE;
                }
                time->year++;
                time->month = 1;
            }
        }
        day_delta--;
    }

    time->hour = (uint8_t)(minutes / 60);
    time->minute = (uint8_t)(minutes % 60);
    time->weekday = weekday_for_date(time->year, time->month, time->day);
    return ESP_OK;
}

static uint8_t timezone_read_selected(void)
{
    if (s_selected_loaded) {
        return s_selected;
    }
    s_selected_loaded = true;

    nvs_handle_t nvs;
    if (nvs_open(TIMEZONE_NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        return s_selected;
    }

    uint8_t selected = s_selected;
    esp_err_t ret = nvs_get_u8(nvs, TIMEZONE_NVS_SELECTED_KEY, &selected);
    nvs_close(nvs);
    if (ret == ESP_OK && selected < TIMEZONE_OPTION_COUNT) {
        s_selected = selected;
    }
    return s_selected;
}

static esp_err_t timezone_save_selected(uint8_t selected)
{
    if (selected >= TIMEZONE_OPTION_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(TIMEZONE_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = nvs_set_u8(nvs, TIMEZONE_NVS_SELECTED_KEY, selected);
    if (ret == ESP_OK) {
        ret = nvs_commit(nvs);
    }
    nvs_close(nvs);
    if (ret == ESP_OK) {
        s_selected = selected;
        s_selected_loaded = true;
    }
    return ret;
}

const char *timezone_setup_get_abbreviation(void)
{
    return s_abbreviations[timezone_read_selected()];
}

int16_t timezone_setup_get_utc_offset_minutes(void)
{
    return s_utc_offset_minutes[timezone_read_selected()];
}

bool timezone_setup_show_abbreviation(void)
{
    if (s_show_abbreviation_loaded) {
        return s_show_abbreviation;
    }
    s_show_abbreviation_loaded = true;

    nvs_handle_t nvs;
    if (nvs_open(TIMEZONE_NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        return s_show_abbreviation;
    }
    uint8_t enabled = 1;
    if (nvs_get_u8(nvs, TIMEZONE_NVS_SHOW_ABBR_KEY, &enabled) == ESP_OK) {
        s_show_abbreviation = enabled != 0;
    }
    nvs_close(nvs);
    return s_show_abbreviation;
}

static void show_abbreviation_changed_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_VALUE_CHANGED) {
        return;
    }

    lv_obj_t *sw = lv_event_get_target_obj(event);
    bool enabled = lv_obj_has_state(sw, LV_STATE_CHECKED);
    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(TIMEZONE_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (ret == ESP_OK) {
        ret = nvs_set_u8(nvs, TIMEZONE_NVS_SHOW_ABBR_KEY, enabled ? 1 : 0);
        if (ret == ESP_OK) {
            ret = nvs_commit(nvs);
        }
        nvs_close(nvs);
    }
    if (ret == ESP_OK) {
        s_show_abbreviation = enabled;
        s_show_abbreviation_loaded = true;
        return;
    }

    ESP_LOGW(TAG, "Failed to save time-zone visibility: %s", esp_err_to_name(ret));
    if (enabled) {
        lv_obj_remove_state(sw, LV_STATE_CHECKED);
    } else {
        lv_obj_add_state(sw, LV_STATE_CHECKED);
    }
}

static esp_err_t timezone_apply_selected(uint8_t previous, uint8_t selected)
{
    if (previous >= TIMEZONE_OPTION_COUNT || selected >= TIMEZONE_OPTION_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    if (previous == selected) {
        return timezone_save_selected(selected);
    }

    pcf85063_time_t original_time;
    esp_err_t ret = pcf85063_get_time(&original_time);
    if (ret != ESP_OK) {
        return ret;
    }

    pcf85063_time_t adjusted_time = original_time;
    int16_t delta_minutes = s_utc_offset_minutes[selected] -
                            s_utc_offset_minutes[previous];
    ret = shift_time_minutes(&adjusted_time, delta_minutes);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = pcf85063_set_time(&adjusted_time);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = timezone_save_selected(selected);
    if (ret != ESP_OK) {
        esp_err_t rollback_ret = pcf85063_set_time(&original_time);
        if (rollback_ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to restore RTC after NVS error: %s",
                     esp_err_to_name(rollback_ret));
        }
        return ret;
    }

    log_storage_refresh_time();
    ESP_LOGI(TAG, "Time zone changed: index=%u -> %u, RTC adjusted by %d minutes",
             (unsigned int)previous, (unsigned int)selected, (int)delta_minutes);
    return ESP_OK;
}

static void back_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_CLICKED) {
        ui_nav_go_back();
    }
}

static void timezone_changed_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_VALUE_CHANGED) {
        return;
    }

    lv_obj_t *dropdown = lv_event_get_target_obj(event);
    uint8_t previous = timezone_read_selected();
    uint8_t selected = (uint8_t)lv_dropdown_get_selected(dropdown);
    esp_err_t ret = timezone_apply_selected(previous, selected);
    if (ret == ESP_OK) {
        lv_label_set_text(s_status, ui_i18n_get(UI_STR_TIMEZONE_SAVED));
        lv_obj_set_style_text_color(s_status, lv_color_hex(0xA2ED8B), 0);
    } else {
        lv_dropdown_set_selected(dropdown, previous);
        ESP_LOGW(TAG, "Failed to update time zone: %s", esp_err_to_name(ret));
        lv_label_set_text(s_status, ui_i18n_get(UI_STR_TIMEZONE_FAILED));
        lv_obj_set_style_text_color(s_status, lv_color_hex(0xFF8B92), 0);
    }
}

void timezone_setup_ui_show(void)
{
    lv_obj_t *screen = lv_screen_active();
    lv_obj_clean(screen);
    ui_nav_enable_right_swipe(screen);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x11141A), 0);
    lv_obj_set_style_pad_all(screen, 0, 0);
    ui_status_bar_add(screen, TIMEZONE_UI_WIDTH);

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
    lv_label_set_text(title, ui_i18n_get(UI_STR_TIMEZONE_SETTING));
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_set_style_text_font(title, ui_i18n_font_28(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 76, 72);

    lv_obj_t *dropdown = lv_dropdown_create(screen);
    lv_dropdown_set_options(dropdown,
                            ui_i18n_get_language() == UI_LANGUAGE_CHINESE
                                ? s_options_zh
                                : s_options_en);
    lv_dropdown_set_selected(dropdown, timezone_read_selected());
    lv_obj_set_size(dropdown, TIMEZONE_UI_WIDTH - 50, 64);
    lv_obj_align(dropdown, LV_ALIGN_TOP_MID, 0, 142);
    lv_obj_set_style_bg_color(dropdown, lv_color_hex(0x292A32), 0);
    lv_obj_set_style_bg_opa(dropdown, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(dropdown, 0, 0);
    lv_obj_set_style_radius(dropdown, 6, 0);
    lv_obj_set_style_pad_left(dropdown, 14, 0);
    lv_obj_set_style_pad_right(dropdown, 34, 0);
    lv_obj_set_style_text_color(dropdown, lv_color_white(), 0);
    lv_obj_set_style_text_font(dropdown, &font_timezone_24, 0);

    lv_obj_t *list = lv_dropdown_get_list(dropdown);
    lv_obj_set_style_bg_color(list, lv_color_hex(0x292A32), 0);
    lv_obj_set_style_border_color(list, lv_color_hex(0x4A4D59), 0);
    lv_obj_set_style_border_width(list, 1, 0);
    lv_obj_set_style_text_color(list, lv_color_white(), 0);
    lv_obj_set_style_text_font(list, &font_timezone_24, 0);
    lv_obj_set_style_bg_color(list, lv_color_hex(0x2997E8), LV_PART_SELECTED);
    lv_obj_set_style_text_color(list, lv_color_white(), LV_PART_SELECTED);

    lv_obj_add_event_cb(dropdown, timezone_changed_cb,
                        LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *show_row = lv_obj_create(screen);
    lv_obj_set_size(show_row, TIMEZONE_UI_WIDTH - 50, 80);
    lv_obj_align(show_row, LV_ALIGN_TOP_MID, 0, 224);
    lv_obj_set_style_bg_color(show_row, lv_color_hex(0x292A32), 0);
    lv_obj_set_style_bg_opa(show_row, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(show_row, 0, 0);
    lv_obj_set_style_radius(show_row, 6, 0);
    lv_obj_set_style_pad_all(show_row, 0, 0);
    lv_obj_clear_flag(show_row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *show_label = lv_label_create(show_row);
    lv_label_set_text(show_label, ui_i18n_get(UI_STR_SHOW_TIMEZONE));
    lv_obj_set_style_text_color(show_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(show_label, ui_i18n_font_28(), 0);
    lv_obj_align(show_label, LV_ALIGN_LEFT_MID, 16, 0);

    lv_obj_t *show_switch = lv_switch_create(show_row);
    lv_obj_set_size(show_switch, 82, 44);
    lv_obj_align(show_switch, LV_ALIGN_RIGHT_MID, -16, 0);
    lv_obj_set_style_bg_color(show_switch, lv_color_hex(0x3A3C46), LV_PART_MAIN);
    lv_obj_set_style_bg_color(show_switch, lv_color_hex(0x2997E8),
                              LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(show_switch, lv_color_white(), LV_PART_KNOB);
    if (timezone_setup_show_abbreviation()) {
        lv_obj_add_state(show_switch, LV_STATE_CHECKED);
    }
    lv_obj_add_event_cb(show_switch, show_abbreviation_changed_cb,
                        LV_EVENT_VALUE_CHANGED, NULL);

    s_status = lv_label_create(screen);
    lv_label_set_text(s_status, "");
    lv_obj_set_style_text_font(s_status, ui_i18n_font_16(), 0);
    lv_obj_align(s_status, LV_ALIGN_TOP_LEFT, 25, 320);
}

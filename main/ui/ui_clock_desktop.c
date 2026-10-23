#include "ui_clock_desktop.h"

#include <stdio.h>

#include "ADC/battery_monitor.h"
#include "PCF85063/pcf85063.h"
#include "lvgl.h"
#include "setup_ui.h"
#include "ui/unlock_pin_ui.h"
#include "ui_nav.h"
#include "usb_hid_keyboard.h"
/*160*4 */
#define CLOCK_DESKTOP_WIDTH  480
#define CLOCK_DESKTOP_HEIGHT 640
#define CLOCK_TIME_SCALE     512
#define CLOCK_DATE_SCALE     512
#define CLOCK_YEAR_SCALE     512
#define CLOCK_LEFT_MARGIN    10

#define CLOCK_TIME_Y         56
#define CLOCK_YEAR_Y         200
#define CLOCK_MONTH_Y        344
#define CLOCK_DAY_Y          488
#define CLOCK_HM_MD_TIME_Y   80
#define CLOCK_HM_MD_MONTH_Y  272
#define CLOCK_HM_MD_DAY_Y    440
#define CLOCK_HM_TIME_Y      224
#define CLOCK_BATTERY_REFRESH_MS 1000
#define CLOCK_RTC_REFRESH_MS 1000
#define CLOCK_STATUS_TEXT_COLOR 0x9EA3AD
#define CLOCK_USB_COLOR         0x34C759

LV_FONT_DECLARE(font_clock_96);

static lv_obj_t *s_battery_icon;
static lv_obj_t *s_battery_label;
static lv_timer_t *s_battery_timer;
static lv_obj_t *s_time_label;
static lv_obj_t *s_year_label;
static lv_obj_t *s_month_label;
static lv_obj_t *s_day_label;
static lv_timer_t *s_rtc_timer;

static void clock_rtc_update(void)
{
    if (!s_time_label) {
        return;
    }

    pcf85063_time_t time;
    esp_err_t ret = pcf85063_get_time(&time);
    if (ret != ESP_OK) {
        lv_label_set_text(s_time_label, "--:--");
        if (s_year_label) lv_label_set_text(s_year_label, "----");
        if (s_month_label) lv_label_set_text(s_month_label, "--");
        if (s_day_label) lv_label_set_text(s_day_label, "--");
        return;
    }

    lv_label_set_text_fmt(s_time_label, "%02u:%02u", time.hour, time.minute);
    if (s_year_label) lv_label_set_text_fmt(s_year_label, "%04u", time.year);
    if (s_month_label) lv_label_set_text_fmt(s_month_label, "%02u", time.month);
    if (s_day_label) lv_label_set_text_fmt(s_day_label, "%02u", time.day);
}

static void clock_rtc_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    clock_rtc_update();
}

static void clock_battery_update(void)
{
    if (!s_battery_icon || !s_battery_label) {
        return;
    }

    int percent = battery_monitor_get_percent();
    if (percent >= 0) {
        lv_label_set_text_fmt(s_battery_label, "%d%%", percent);
    } else {
        lv_label_set_text(s_battery_label, "--");
    }
    lv_obj_set_style_text_color(
        s_battery_icon,
        lv_color_hex(usb_hid_keyboard_is_ready() ? CLOCK_USB_COLOR :
                                                   CLOCK_STATUS_TEXT_COLOR),
        0);
}

static void clock_battery_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    clock_battery_update();
}

static void clock_battery_delete_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_DELETE) {
        return;
    }

    if (s_battery_timer) {
        lv_timer_delete(s_battery_timer);
        s_battery_timer = NULL;
    }
    if (s_rtc_timer) {
        lv_timer_delete(s_rtc_timer);
        s_rtc_timer = NULL;
    }
    s_battery_icon = NULL;
    s_battery_label = NULL;
    s_time_label = NULL;
    s_year_label = NULL;
    s_month_label = NULL;
    s_day_label = NULL;
}

static void create_battery_status(lv_obj_t *screen)
{
    lv_obj_t *status = lv_obj_create(screen);
    lv_obj_remove_style_all(status);
    lv_obj_set_size(status, CLOCK_DESKTOP_WIDTH, 56);
    lv_obj_align(status, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_opa(status, LV_OPA_TRANSP, 0);
    lv_obj_add_flag(status, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(status, clock_battery_delete_cb, LV_EVENT_DELETE, NULL);

    lv_obj_t *battery_group = lv_obj_create(status);
    lv_obj_remove_style_all(battery_group);
    lv_obj_set_size(battery_group, 116, 56);
    lv_obj_align(battery_group, LV_ALIGN_RIGHT_MID, -8, 0);
    lv_obj_set_flex_flow(battery_group, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(battery_group, LV_FLEX_ALIGN_END,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(battery_group, 6, 0);
    lv_obj_clear_flag(battery_group, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(battery_group, LV_OBJ_FLAG_GESTURE_BUBBLE);

    s_battery_icon = lv_label_create(battery_group);
    lv_label_set_text(s_battery_icon, LV_SYMBOL_BATTERY_FULL);
    lv_obj_set_style_text_font(s_battery_icon, &lv_font_montserrat_28, 0);
    lv_obj_add_flag(s_battery_icon, LV_OBJ_FLAG_GESTURE_BUBBLE);

    s_battery_label = lv_label_create(battery_group);
    lv_obj_set_style_text_color(s_battery_label,
                                lv_color_hex(CLOCK_STATUS_TEXT_COLOR), 0);
    lv_obj_set_style_text_font(s_battery_label, &lv_font_montserrat_28, 0);
    lv_obj_add_flag(s_battery_label, LV_OBJ_FLAG_GESTURE_BUBBLE);
    clock_battery_update();

    s_battery_timer = lv_timer_create(clock_battery_timer_cb,
                                      CLOCK_BATTERY_REFRESH_MS,
                                      NULL);
}

static void desktop_gesture_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_GESTURE) {
        return;
    }

    lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_active());
    if (dir == LV_DIR_LEFT || dir == LV_DIR_RIGHT ||
        dir == LV_DIR_TOP || dir == LV_DIR_BOTTOM) {
        lv_obj_t *screen = lv_event_get_current_target(event);
        if (screen) {
            lv_obj_remove_event_cb(screen, desktop_gesture_event_cb);
        }
        ui_nav_replace(unlock_pin_ui_show);
    }
}

static lv_obj_t *create_clock_label(lv_obj_t *parent,
                                    const char *text,
                                    const lv_font_t *font,
                                    lv_color_t color,
                                    int32_t x,
                                    int32_t y,
                                    int32_t scale)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, color, 0);
    lv_obj_set_style_text_letter_space(label, 0, 0);
    lv_obj_set_style_transform_scale(label, scale, 0);
    lv_obj_set_style_transform_pivot_x(label, 0, 0);
    lv_obj_set_style_transform_pivot_y(label, 0, 0);
    lv_obj_set_pos(label, x, y);
    lv_obj_add_flag(label, LV_OBJ_FLAG_GESTURE_BUBBLE);
    return label;
}

void ui_clock_desktop_show(void)
{
    lv_obj_t *screen = lv_screen_active();
    lv_obj_clean(screen);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x0D1117), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(screen, 0, 0);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(screen, desktop_gesture_event_cb, LV_EVENT_GESTURE, NULL);

    lv_obj_t *content = lv_obj_create(screen);
    lv_obj_remove_style_all(content);
    lv_obj_set_size(content, CLOCK_DESKTOP_WIDTH, CLOCK_DESKTOP_HEIGHT);
    lv_obj_align(content, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(content, lv_color_hex(0x0D1117), 0);
    lv_obj_set_style_bg_opa(content, LV_OPA_COVER, 0);
    lv_obj_clear_flag(content, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_CHAIN);
    lv_obj_add_flag(content, LV_OBJ_FLAG_GESTURE_BUBBLE);

    setup_lock_clock_mode_t mode = setup_ui_get_lock_clock_mode();
    int32_t time_y = CLOCK_TIME_Y;
    if (mode == SETUP_LOCK_CLOCK_HM_MD) {
        time_y = CLOCK_HM_MD_TIME_Y;
    } else if (mode == SETUP_LOCK_CLOCK_HM) {
        time_y = CLOCK_HM_TIME_Y;
    }

    if (mode != SETUP_LOCK_CLOCK_OFF) {
        s_time_label = create_clock_label(content, "--:--", &font_clock_96,
                                          lv_color_hex(0xF5F5F5),
                                          CLOCK_LEFT_MARGIN, time_y, CLOCK_TIME_SCALE);
    }
    if (mode == SETUP_LOCK_CLOCK_HM_YMD) {
        s_year_label = create_clock_label(content, "----", &font_clock_96,
                                          lv_color_hex(0x747474),
                                          CLOCK_LEFT_MARGIN, CLOCK_YEAR_Y, CLOCK_YEAR_SCALE);
        s_month_label = create_clock_label(content, "--", &font_clock_96,
                                           lv_color_hex(0xA0A0A0),
                                           CLOCK_LEFT_MARGIN, CLOCK_MONTH_Y, CLOCK_DATE_SCALE);
        s_day_label = create_clock_label(content, "--", &font_clock_96,
                                         lv_color_hex(0xA0A0A0),
                                         CLOCK_LEFT_MARGIN, CLOCK_DAY_Y, CLOCK_DATE_SCALE);
    } else if (mode == SETUP_LOCK_CLOCK_HM_MD) {
        s_month_label = create_clock_label(content, "--", &font_clock_96,
                                           lv_color_hex(0xA0A0A0),
                                           CLOCK_LEFT_MARGIN, CLOCK_HM_MD_MONTH_Y,
                                           CLOCK_DATE_SCALE);
        s_day_label = create_clock_label(content, "--", &font_clock_96,
                                         lv_color_hex(0xA0A0A0),
                                         CLOCK_LEFT_MARGIN, CLOCK_HM_MD_DAY_Y,
                                         CLOCK_DATE_SCALE);
    }

    if (s_time_label) {
        clock_rtc_update();
        s_rtc_timer = lv_timer_create(clock_rtc_timer_cb, CLOCK_RTC_REFRESH_MS, NULL);
    }

    create_battery_status(screen);
}

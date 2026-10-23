#include "ui_status_bar.h"

#include <stdio.h>

#include "esp_heap_caps.h"

#include "ADC/battery_monitor.h"
#include "PCF85063/pcf85063.h"
#include "timezone_setup_ui.h"
#include "usb_hid_keyboard.h"

#define UI_STATUS_BAR_BATTERY_REFRESH_MS 1000
#define UI_STATUS_BAR_TEXT_COLOR 0x9EA3AD
#define UI_STATUS_BAR_USB_COLOR  0x34C759
#define UI_STATUS_BAR_LEFT_RESERVED_WIDTH 104
#define UI_STATUS_BAR_RIGHT_MARGIN        8
#define UI_STATUS_BAR_BATTERY_WIDTH       116
#define UI_STATUS_BAR_GROUP_GAP           8

static lv_timer_t *s_battery_timer;
static lv_obj_t *s_battery_icon;
static lv_obj_t *s_battery_label;
static lv_obj_t *s_time_label;
static lv_obj_t *s_refresh_icon;
static lv_obj_t *s_memory_label;
static bool s_refresh_visible;

void ui_status_bar_set_refresh_visible(bool visible)
{
    s_refresh_visible = visible;
    if (!s_refresh_icon) {
        return;
    }

    if (visible) {
        lv_obj_remove_flag(s_refresh_icon, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_refresh_icon, LV_OBJ_FLAG_HIDDEN);
    }
}

static void ui_status_bar_update_memory_label(void)
{
    if (!s_memory_label) return;
    size_t total = heap_caps_get_total_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t free_size = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (total == 0) {
        lv_label_set_text(s_memory_label, "INT --%");
        return;
    }
    unsigned used_percent = (unsigned)(((total - free_size) * 100U + total / 2U) / total);
    lv_label_set_text_fmt(s_memory_label, "INT %u%%", used_percent);
}

static void ui_status_bar_update_time_label(void)
{
    if (!s_time_label) {
        return;
    }

    pcf85063_time_t time;
    bool show_timezone = timezone_setup_show_abbreviation();
    if (pcf85063_get_time(&time) != ESP_OK) {
        if (show_timezone) {
            lv_label_set_text_fmt(s_time_label, "%s --:--",
                                  timezone_setup_get_abbreviation());
        } else {
            lv_label_set_text(s_time_label, "--:--");
        }
        return;
    }

    if (show_timezone) {
        lv_label_set_text_fmt(s_time_label, "%s %02u-%02u %02u:%02u",
                              timezone_setup_get_abbreviation(),
                              time.month, time.day, time.hour, time.minute);
    } else {
        lv_label_set_text_fmt(s_time_label, "%02u-%02u %02u:%02u",
                              time.month, time.day, time.hour, time.minute);
    }
}

static void ui_status_bar_update_battery_label(void)
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
        lv_color_hex(usb_hid_keyboard_is_ready() ? UI_STATUS_BAR_USB_COLOR :
                                                   UI_STATUS_BAR_TEXT_COLOR),
        0);
}

static void ui_status_bar_battery_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    ui_status_bar_update_time_label();
    ui_status_bar_update_battery_label();
    ui_status_bar_update_memory_label();
}

static void ui_status_bar_delete_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_DELETE) {
        return;
    }

    if (s_battery_timer) {
        lv_timer_delete(s_battery_timer);
        s_battery_timer = NULL;
    }
    s_battery_icon = NULL;
    s_battery_label = NULL;
    s_time_label = NULL;
    s_refresh_icon = NULL;
    s_memory_label = NULL;
}

lv_obj_t *ui_status_bar_add(lv_obj_t *screen, int32_t width)
{
    if (s_battery_timer) {
        lv_timer_delete(s_battery_timer);
        s_battery_timer = NULL;
    }
    s_battery_icon = NULL;
    s_battery_label = NULL;
    s_time_label = NULL;
    s_refresh_icon = NULL;
    s_memory_label = NULL;

    lv_obj_t *status = lv_obj_create(screen);
    lv_obj_remove_style_all(status);
    lv_obj_set_size(status, width, UI_STATUS_BAR_HEIGHT);
    lv_obj_align(status, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(status, lv_color_hex(0x0F1117), 0);
    lv_obj_set_style_bg_opa(status, LV_OPA_COVER, 0);
    lv_obj_add_event_cb(status, ui_status_bar_delete_event_cb, LV_EVENT_DELETE, NULL);

    lv_obj_t *memory = lv_label_create(status);
    s_memory_label = memory;
    ui_status_bar_update_memory_label();
    lv_obj_set_style_text_color(memory, lv_color_hex(0x9EA3AD), 0);
    lv_obj_set_style_text_font(memory, &lv_font_montserrat_24, 0);
    lv_obj_align(memory, LV_ALIGN_LEFT_MID, 8, 0);

    lv_obj_t *right_group = lv_obj_create(status);
    lv_obj_remove_style_all(right_group);
    lv_obj_set_size(right_group,
                    width - UI_STATUS_BAR_LEFT_RESERVED_WIDTH -
                        UI_STATUS_BAR_RIGHT_MARGIN -
                        UI_STATUS_BAR_BATTERY_WIDTH -
                        UI_STATUS_BAR_GROUP_GAP,
                    UI_STATUS_BAR_HEIGHT);
    lv_obj_align(right_group, LV_ALIGN_LEFT_MID,
                 UI_STATUS_BAR_LEFT_RESERVED_WIDTH, 0);
    lv_obj_set_flex_flow(right_group, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(right_group, LV_FLEX_ALIGN_END,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(right_group, UI_STATUS_BAR_GROUP_GAP, 0);
    lv_obj_clear_flag(right_group,
                      LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *time_label = lv_label_create(right_group);
    s_time_label = time_label;
    ui_status_bar_update_time_label();
    lv_obj_set_style_text_color(time_label, lv_color_hex(0x9EA3AD), 0);
    lv_obj_set_style_text_font(time_label, &lv_font_montserrat_24, 0);

    s_refresh_icon = lv_label_create(right_group);
    lv_label_set_text(s_refresh_icon, LV_SYMBOL_REFRESH);
    lv_obj_set_style_text_color(s_refresh_icon,
                                lv_color_hex(UI_STATUS_BAR_TEXT_COLOR), 0);
    lv_obj_set_style_text_font(s_refresh_icon, &lv_font_montserrat_28, 0);
    ui_status_bar_set_refresh_visible(s_refresh_visible);

    lv_obj_t *battery_group = lv_obj_create(status);
    lv_obj_remove_style_all(battery_group);
    lv_obj_set_size(battery_group, UI_STATUS_BAR_BATTERY_WIDTH,
                    UI_STATUS_BAR_HEIGHT);
    lv_obj_align(battery_group, LV_ALIGN_RIGHT_MID,
                 -UI_STATUS_BAR_RIGHT_MARGIN, 0);
    lv_obj_set_flex_flow(battery_group, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(battery_group, LV_FLEX_ALIGN_END,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(battery_group, 6, 0);
    lv_obj_clear_flag(battery_group, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    s_battery_icon = lv_label_create(battery_group);
    lv_label_set_text(s_battery_icon, LV_SYMBOL_BATTERY_FULL);
    lv_obj_set_style_text_font(s_battery_icon, &lv_font_montserrat_28, 0);

    s_battery_label = lv_label_create(battery_group);
    lv_obj_set_style_text_color(s_battery_label,
                                lv_color_hex(UI_STATUS_BAR_TEXT_COLOR), 0);
    lv_obj_set_style_text_font(s_battery_label, &lv_font_montserrat_28, 0);
    ui_status_bar_update_battery_label();
    s_battery_timer = lv_timer_create(ui_status_bar_battery_timer_cb,
                                      UI_STATUS_BAR_BATTERY_REFRESH_MS,
                                      NULL);

    return status;
}

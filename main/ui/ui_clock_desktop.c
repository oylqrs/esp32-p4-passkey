#include "ui_clock_desktop.h"

#include <stdio.h>
#include <time.h>

#include "lvgl.h"
#include "ui/UI_SCREEN1.h"
#include "ui_nav.h"

#define CLOCK_DESKTOP_WIDTH  480
#define CLOCK_DESKTOP_HEIGHT 640
#define CLOCK_LEFT_MARGIN    22
#define CLOCK_TEXT_SCALE     512

typedef struct {
    lv_obj_t *time;
    lv_obj_t *year;
    lv_obj_t *month;
    lv_obj_t *day;
} clock_desktop_labels_t;

static lv_timer_t *s_clock_timer;
static clock_desktop_labels_t s_clock_labels;

static void clock_desktop_stop_timer(void)
{
    if (s_clock_timer) {
        lv_timer_delete(s_clock_timer);
        s_clock_timer = NULL;
    }
}

static void clock_desktop_update(void)
{
    time_t now = time(NULL);
    struct tm tm_now;
    localtime_r(&now, &tm_now);

    char text[16];
    snprintf(text, sizeof(text), "%02d:%02d", tm_now.tm_hour, tm_now.tm_min);
    lv_label_set_text(s_clock_labels.time, text);

    snprintf(text, sizeof(text), "%04d", tm_now.tm_year + 1900);
    lv_label_set_text(s_clock_labels.year, text);

    snprintf(text, sizeof(text), "%02d", tm_now.tm_mon + 1);
    lv_label_set_text(s_clock_labels.month, text);

    snprintf(text, sizeof(text), "%02d", tm_now.tm_mday);
    lv_label_set_text(s_clock_labels.day, text);
}

static void clock_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    clock_desktop_update();
}

static void desktop_gesture_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_GESTURE) {
        return;
    }

    lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_active());
    if (dir == LV_DIR_LEFT || dir == LV_DIR_RIGHT || dir == LV_DIR_TOP || dir == LV_DIR_BOTTOM) {
        lv_obj_t *screen = lv_event_get_current_target(event);
        clock_desktop_stop_timer();
        if (screen) {
            lv_obj_remove_event_cb(screen, desktop_gesture_event_cb);
        }
        ui_nav_replace(ui_screen1_show);
    }
}

static lv_obj_t *create_text_line(lv_obj_t *parent,
                                  const char *text,
                                  const lv_font_t *font,
                                  lv_color_t color,
                                  int32_t y)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_width(label, CLOCK_DESKTOP_WIDTH - CLOCK_LEFT_MARGIN * 2);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_text_color(label, color, 0);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_transform_scale(label, CLOCK_TEXT_SCALE, 0);
    lv_obj_set_style_transform_pivot_x(label, 0, 0);
    lv_obj_set_style_transform_pivot_y(label, 0, 0);
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, CLOCK_LEFT_MARGIN, y);
    lv_obj_add_flag(label, LV_OBJ_FLAG_GESTURE_BUBBLE);
    return label;
}

static lv_obj_t *add_strip(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h, lv_color_t color, lv_opa_t opa)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_set_size(obj, w, h);
    lv_obj_align(obj, LV_ALIGN_TOP_LEFT, x, y);
    lv_obj_set_style_bg_color(obj, color, 0);
    lv_obj_set_style_bg_opa(obj, opa, 0);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_CHAIN);
    lv_obj_add_flag(obj, LV_OBJ_FLAG_GESTURE_BUBBLE);
    return obj;
}

static void add_battery_icon(lv_obj_t *parent)
{
    lv_color_t accent = lv_color_hex(0x63E7FF);

    lv_obj_t *body = lv_obj_create(parent);
    lv_obj_remove_style_all(body);
    lv_obj_set_size(body, 26, 12);
    lv_obj_align(body, LV_ALIGN_TOP_RIGHT, -10, 8);
    lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(body, accent, 0);
    lv_obj_set_style_border_width(body, 2, 0);
    lv_obj_set_style_radius(body, 2, 0);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_CHAIN);
    lv_obj_add_flag(body, LV_OBJ_FLAG_GESTURE_BUBBLE);

    lv_obj_t *fill = lv_obj_create(body);
    lv_obj_remove_style_all(fill);
    lv_obj_set_size(fill, 16, 4);
    lv_obj_align(fill, LV_ALIGN_LEFT_MID, 3, 0);
    lv_obj_set_style_bg_color(fill, accent, 0);
    lv_obj_set_style_bg_opa(fill, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(fill, 1, 0);
    lv_obj_clear_flag(fill, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_CHAIN);
    lv_obj_add_flag(fill, LV_OBJ_FLAG_GESTURE_BUBBLE);

    lv_obj_t *cap = lv_obj_create(parent);
    lv_obj_remove_style_all(cap);
    lv_obj_set_size(cap, 4, 6);
    lv_obj_align_to(cap, body, LV_ALIGN_OUT_RIGHT_MID, 1, 0);
    lv_obj_set_style_bg_color(cap, accent, 0);
    lv_obj_set_style_bg_opa(cap, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(cap, 1, 0);
    lv_obj_clear_flag(cap, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_CHAIN);
    lv_obj_add_flag(cap, LV_OBJ_FLAG_GESTURE_BUBBLE);
}

static void add_tech_lines(lv_obj_t *parent)
{
    lv_color_t cyan = lv_color_hex(0x63E7FF);
    lv_color_t dim = lv_color_hex(0x17323A);
    lv_color_t violet = lv_color_hex(0xA78BFA);

    add_strip(parent, 22, 18, 96, 2, cyan, LV_OPA_COVER);
    add_strip(parent, 22, 18, 2, 34, cyan, LV_OPA_COVER);
    add_strip(parent, 376, 18, 60, 2, violet, LV_OPA_COVER);
    add_strip(parent, 434, 18, 2, 24, violet, LV_OPA_COVER);

    add_strip(parent, 22, 130, 180, 1, dim, LV_OPA_COVER);
    add_strip(parent, 22, 168, 180, 1, dim, LV_OPA_COVER);
    add_strip(parent, 22, 206, 180, 1, dim, LV_OPA_COVER);

    add_strip(parent, 10, 136, 2, 356, cyan, LV_OPA_60);
    add_strip(parent, 468, 136, 2, 356, violet, LV_OPA_40);

    for (int32_t y = 254; y < 582; y += 64) {
        add_strip(parent, 22, y, 308, 1, lv_color_hex(0x10242B), LV_OPA_COVER);
    }
}

static void add_hint_text(lv_obj_t *parent)
{
    lv_obj_t *left = lv_label_create(parent);
    lv_label_set_text(left, "LOCKED");
    lv_obj_set_style_text_color(left, lv_color_hex(0x63E7FF), 0);
    lv_obj_set_style_text_font(left, &lv_font_montserrat_14, 0);
    lv_obj_align(left, LV_ALIGN_TOP_LEFT, 22, 22);
    lv_obj_add_flag(left, LV_OBJ_FLAG_GESTURE_BUBBLE);

    lv_obj_t *right = lv_label_create(parent);
    lv_label_set_text(right, "SWIPE TO OPEN");
    lv_obj_set_style_text_color(right, lv_color_hex(0x62707A), 0);
    lv_obj_set_style_text_font(right, &lv_font_montserrat_14, 0);
    lv_obj_align(right, LV_ALIGN_BOTTOM_RIGHT, -22, -18);
    lv_obj_add_flag(right, LV_OBJ_FLAG_GESTURE_BUBBLE);
}

void ui_clock_desktop_show(void)
{
    clock_desktop_stop_timer();

    lv_obj_t *screen = lv_screen_active();
    lv_obj_clean(screen);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x061015), 0);
    lv_obj_set_style_pad_all(screen, 0, 0);
    lv_obj_add_event_cb(screen, desktop_gesture_event_cb, LV_EVENT_GESTURE, NULL);

    lv_obj_t *panel = lv_obj_create(screen);
    lv_obj_remove_style_all(panel);
    lv_obj_set_size(panel, CLOCK_DESKTOP_WIDTH, CLOCK_DESKTOP_HEIGHT);
    lv_obj_align(panel, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x061015), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_CHAIN);
    lv_obj_add_flag(panel, LV_OBJ_FLAG_GESTURE_BUBBLE);

    add_tech_lines(panel);
    add_battery_icon(panel);
    add_hint_text(panel);

    s_clock_labels.time = create_text_line(panel, "00:00", &lv_font_montserrat_48,
                                           lv_color_hex(0xF4FBFF), 36);
    s_clock_labels.year = create_text_line(panel, "1970", &lv_font_montserrat_48,
                                           lv_color_hex(0xA9B6BE), 176);
    s_clock_labels.month = create_text_line(panel, "01", &lv_font_montserrat_48,
                                            lv_color_hex(0x7E8A92), 316);
    s_clock_labels.day = create_text_line(panel, "01", &lv_font_montserrat_48,
                                          lv_color_hex(0x67737B), 456);

    clock_desktop_update();
    s_clock_timer = lv_timer_create(clock_timer_cb, 1000, NULL);
}

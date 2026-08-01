#include "ui_nav.h"

#define UI_NAV_STACK_DEPTH  8

static ui_screen_show_cb_t s_back_stack[UI_NAV_STACK_DEPTH];
static uint8_t s_back_stack_len;
static ui_screen_show_cb_t s_root_screen;

static void right_swipe_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_GESTURE) {
        return;
    }

    if (lv_indev_get_gesture_dir(lv_indev_active()) == LV_DIR_RIGHT) {
        ui_nav_go_back();
    }
}

void ui_nav_push(ui_screen_show_cb_t current_screen, ui_screen_show_cb_t next_screen)
{
    if (!next_screen) {
        return;
    }

    if (current_screen && s_back_stack_len < UI_NAV_STACK_DEPTH) {
        s_back_stack[s_back_stack_len++] = current_screen;
    }

    next_screen();
}

void ui_nav_clear(void)
{
    s_back_stack_len = 0;
}

void ui_nav_replace(ui_screen_show_cb_t next_screen)
{
    ui_nav_clear();
    if (next_screen) {
        next_screen();
    }
}

void ui_nav_set_root(ui_screen_show_cb_t root_screen)
{
    s_root_screen = root_screen;
    ui_nav_clear();
}

void ui_nav_go_back(void)
{
    if (s_back_stack_len == 0) {
        return;
    }

    ui_screen_show_cb_t previous_screen = s_back_stack[--s_back_stack_len];
    if (previous_screen == s_root_screen) {
        ui_nav_clear();
    }
    if (previous_screen) {
        previous_screen();
    }
}

void ui_nav_enable_right_swipe(lv_obj_t *screen)
{
    if (!screen) {
        return;
    }

    lv_obj_remove_event_cb(screen, right_swipe_event_cb);
    lv_obj_add_event_cb(screen, right_swipe_event_cb, LV_EVENT_GESTURE, NULL);
}

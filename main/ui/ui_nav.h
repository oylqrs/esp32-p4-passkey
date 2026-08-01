#pragma once

#include "lvgl.h"

typedef void (*ui_screen_show_cb_t)(void);

void ui_nav_push(ui_screen_show_cb_t current_screen, ui_screen_show_cb_t next_screen);
void ui_nav_clear(void);
void ui_nav_replace(ui_screen_show_cb_t next_screen);
void ui_nav_set_root(ui_screen_show_cb_t root_screen);
void ui_nav_go_back(void);
void ui_nav_enable_right_swipe(lv_obj_t *screen);

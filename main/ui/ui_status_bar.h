#pragma once

#include "lvgl.h"

#define UI_STATUS_BAR_HEIGHT 56 //28

lv_obj_t *ui_status_bar_add(lv_obj_t *screen, int32_t width);
void ui_status_bar_set_refresh_visible(bool visible);

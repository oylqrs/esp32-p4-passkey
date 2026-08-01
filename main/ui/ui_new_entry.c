#include "ui_new_entry.h"

#include <stdbool.h>
#include <string.h>

#include "esp_log.h"
#include "lvgl.h"
#include "ui_nav.h"
#include "UI_SCREEN2.h"

#define NEW_ENTRY_WIDTH 480
#define NEW_ENTRY_FIELD_COUNT 5

static const char *TAG = "ui_new_entry";
static lv_obj_t *s_fields[NEW_ENTRY_FIELD_COUNT];

static const char *s_field_names[NEW_ENTRY_FIELD_COUNT] = {
    "Title",
    "User",
    "Pass",
    "URL",
    "Note",
};

static void save_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }

    const char *title = lv_textarea_get_text(s_fields[0]);
    if (!title || title[0] == '\0') {
        lv_textarea_set_placeholder_text(s_fields[0], "Title is required");
        lv_obj_add_state(s_fields[0], LV_STATE_FOCUSED);
        return;
    }

    ESP_LOGI(TAG,
             "New Entry Save: title=%s user=%s pass=%s url=%s note=%s",
             title,
             lv_textarea_get_text(s_fields[1]),
             lv_textarea_get_text(s_fields[2]),
             lv_textarea_get_text(s_fields[3]),
             lv_textarea_get_text(s_fields[4]));

    ui_nav_go_back();
}

static void field_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED &&
        lv_event_get_code(event) != LV_EVENT_FOCUSED) {
        return;
    }

    lv_obj_t *field = lv_event_get_target_obj(event);
    lv_obj_add_state(field, LV_STATE_FOCUSED);
    lv_textarea_set_cursor_pos(field, LV_TEXTAREA_CURSOR_LAST);
}

static lv_obj_t *add_field(lv_obj_t *parent, size_t index, int32_t y)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, s_field_names[index]);
    lv_obj_set_style_text_color(label, lv_color_hex(0xAAB0BA), 0);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_16, 0);
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, 24, y + 5);

    lv_obj_t *field = lv_textarea_create(parent);
    lv_obj_set_size(field, NEW_ENTRY_WIDTH - 150, 56);
    lv_obj_align(field, LV_ALIGN_TOP_RIGHT, -24, y);
    lv_textarea_set_one_line(field, index != 4);
    lv_textarea_set_max_length(field, index == 4 ? 256 : 128);
    lv_textarea_set_placeholder_text(field, s_field_names[index]);
    lv_obj_set_style_bg_color(field, lv_color_hex(0x292A32), 0);
    lv_obj_set_style_border_color(field, lv_color_hex(0x3A3C46), 0);
    lv_obj_set_style_border_width(field, 1, 0);
    lv_obj_set_style_radius(field, 5, 0);
    lv_obj_set_style_text_color(field, lv_color_white(), 0);
    lv_obj_set_style_text_font(field, &lv_font_montserrat_16, 0);
    lv_obj_set_style_pad_left(field, 12, 0);
    lv_obj_set_style_pad_right(field, 12, 0);
    lv_obj_set_style_pad_top(field, 8, 0);
    lv_obj_set_style_pad_bottom(field, 8, 0);
    lv_obj_add_event_cb(field, field_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(field, field_event_cb, LV_EVENT_FOCUSED, NULL);

    if (index == 2) {
        lv_textarea_set_password_mode(field, true);
    }

    s_fields[index] = field;
    return field;
}

void ui_new_entry_show(void)
{
    memset(s_fields, 0, sizeof(s_fields));

    lv_obj_t *screen = lv_screen_active();
    lv_obj_clean(screen);
    ui_nav_enable_right_swipe(screen);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x11141A), 0);
    lv_obj_set_style_pad_all(screen, 0, 0);

    lv_obj_t *title = lv_label_create(screen);
    lv_label_set_text(title, "New Entry");
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 24, 24);

    const int32_t first_y = 80;
    const int32_t row_gap = 76;
    for (size_t i = 0; i < NEW_ENTRY_FIELD_COUNT; i++) {
        add_field(screen, i, first_y + (int32_t)i * row_gap);
    }

    lv_obj_t *save = lv_button_create(screen);
    lv_obj_set_size(save, 180, 58);
    lv_obj_align(save, LV_ALIGN_BOTTOM_MID, 0, -20);
    lv_obj_set_style_bg_color(save, lv_color_hex(0x5D6CFF), 0);
    lv_obj_set_style_bg_color(save, lv_color_hex(0x7884FF), LV_STATE_PRESSED);
    lv_obj_set_style_radius(save, 6, 0);
    lv_obj_add_event_cb(save, save_event_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *save_label = lv_label_create(save);
    lv_label_set_text(save_label, "Save");
    lv_obj_set_style_text_color(save_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(save_label, &lv_font_montserrat_24, 0);
    lv_obj_center(save_label);
}

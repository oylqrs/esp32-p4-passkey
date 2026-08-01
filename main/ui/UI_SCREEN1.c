#include "UI_SCREEN1.h"
#include "UI_SCREEN2.h"

#include <stdint.h>
#include <string.h>

#include "esp_log.h"
#include "lvgl.h"
#include "setup_ui.h"
#include "ui_nav.h"

#define UI_SCREEN1_WIDTH  480

static lv_obj_t *s_pin_textarea;

static void pin_textarea_event_cb(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_PRESSED || code == LV_EVENT_CLICKED || code == LV_EVENT_FOCUSED) {
        lv_obj_t *textarea = lv_event_get_target_obj(event);
        lv_obj_add_state(textarea, LV_STATE_FOCUSED);
        lv_textarea_set_cursor_pos(textarea, LV_TEXTAREA_CURSOR_LAST);
    }
}

static void pin_keypad_event_cb(lv_event_t *event)
{
    lv_obj_t *button_matrix = lv_event_get_target_obj(event);
    uint32_t button_id = lv_buttonmatrix_get_selected_button(button_matrix);
    const char *key = lv_buttonmatrix_get_button_text(button_matrix, button_id);
    if (!key || !s_pin_textarea) {
        return;
    }

    if (strcmp(key, LV_SYMBOL_BACKSPACE) == 0) {
        lv_obj_add_state(s_pin_textarea, LV_STATE_FOCUSED);
        lv_textarea_delete_char(s_pin_textarea);
    } else if (strcmp(key, LV_SYMBOL_OK) == 0) {
        const char *pin = lv_textarea_get_text(s_pin_textarea);
        ESP_LOGI("PIN_PAGE", "PIN input: %s", pin);
        if (setup_ui_device_password_matches(pin)) {
            ui_nav_replace(ui_screen2_show);
        } else {
            ESP_LOGW("PIN_PAGE", "Wrong PIN");
            lv_textarea_set_text(s_pin_textarea, "");
            lv_obj_add_state(s_pin_textarea, LV_STATE_FOCUSED);
        }
    } else if (key[0] >= '0' && key[0] <= '9') {
        lv_obj_add_state(s_pin_textarea, LV_STATE_FOCUSED);
        lv_textarea_add_char(s_pin_textarea, key[0]);
    }
}

void ui_screen1_show(void)
{
    lv_obj_t *screen = lv_screen_active();
    lv_obj_clean(screen);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x11141A), 0);
    lv_obj_set_style_pad_all(screen, 0, 0);

    lv_obj_t *status = lv_obj_create(screen);
    lv_obj_remove_style_all(status);
    lv_obj_set_size(status, UI_SCREEN1_WIDTH, 28);
    lv_obj_align(status, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(status, lv_color_hex(0x11141A), 0);
    lv_obj_set_style_bg_opa(status, LV_OPA_COVER, 0);

    lv_obj_t *time_label = lv_label_create(status);
    lv_label_set_text(time_label, "07-09 00:06");
    lv_obj_set_style_text_color(time_label, lv_color_hex(0xAAB0BA), 0);
    lv_obj_set_style_text_font(time_label, &lv_font_montserrat_28, 0);
    lv_obj_align(time_label, LV_ALIGN_TOP_MID, 0, 3);

    lv_obj_t *battery = lv_label_create(status);
    lv_label_set_text(battery, LV_SYMBOL_BATTERY_FULL);
    lv_obj_set_style_text_color(battery, lv_color_hex(0xAAB0BA), 0);
    lv_obj_set_style_text_font(battery, &lv_font_montserrat_28, 0);
    lv_obj_align(battery, LV_ALIGN_TOP_RIGHT, -8, 3);

    lv_obj_t *title = lv_label_create(screen);
    lv_label_set_text(title, "Unlock");
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 28, 36);

    s_pin_textarea = lv_textarea_create(screen);
    lv_obj_set_size(s_pin_textarea, UI_SCREEN1_WIDTH - 56, 300);
    lv_obj_align(s_pin_textarea, LV_ALIGN_TOP_MID, 0, 98);
    lv_textarea_set_one_line(s_pin_textarea, true);
    lv_textarea_set_password_mode(s_pin_textarea, true);
    lv_textarea_set_max_length(s_pin_textarea, 12);
    lv_textarea_set_placeholder_text(s_pin_textarea, "Please enter password");
    lv_textarea_set_cursor_click_pos(s_pin_textarea, true);
    lv_obj_add_event_cb(s_pin_textarea, pin_textarea_event_cb, LV_EVENT_ALL, NULL);
    lv_obj_set_style_bg_color(s_pin_textarea, lv_color_hex(0x2B2C34), 0);
    lv_obj_set_style_border_color(s_pin_textarea, lv_color_hex(0x31333C), 0);
    lv_obj_set_style_border_width(s_pin_textarea, 1, 0);
    lv_obj_set_style_radius(s_pin_textarea, 5, 0);
    lv_obj_set_style_text_color(s_pin_textarea, lv_color_white(), 0);
    lv_obj_set_style_text_font(s_pin_textarea, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(s_pin_textarea, lv_color_hex(0x8D9099), LV_PART_TEXTAREA_PLACEHOLDER);
    lv_obj_set_style_bg_color(s_pin_textarea, lv_color_white(), LV_PART_CURSOR | LV_STATE_FOCUSED);
    lv_obj_set_style_bg_opa(s_pin_textarea, LV_OPA_COVER, LV_PART_CURSOR | LV_STATE_FOCUSED);
    lv_obj_set_style_width(s_pin_textarea, 2, LV_PART_CURSOR | LV_STATE_FOCUSED);
    lv_obj_set_style_anim_duration(s_pin_textarea, 500, LV_PART_CURSOR | LV_STATE_FOCUSED);
    lv_obj_set_style_pad_left(s_pin_textarea, 18, 0);
    lv_obj_set_style_pad_right(s_pin_textarea, 18, 0);
    lv_obj_set_style_pad_top(s_pin_textarea, 18, 0);
    lv_obj_set_style_pad_bottom(s_pin_textarea, 18, 0);

    static const char *pin_map[] = {
        "1", "2", "3", "\n",
        "4", "5", "6", "\n",
        "7", "8", "9", "\n",
        LV_SYMBOL_BACKSPACE, "0", LV_SYMBOL_OK, ""
    };

    lv_obj_t *keypad = lv_buttonmatrix_create(screen);
    lv_buttonmatrix_set_map(keypad, pin_map);
    lv_obj_set_size(keypad, UI_SCREEN1_WIDTH - 12, 376);
    lv_obj_align(keypad, LV_ALIGN_BOTTOM_MID, 0, -4);
    lv_obj_set_style_bg_opa(keypad, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(keypad, 0, 0);
    lv_obj_set_style_pad_all(keypad, 0, 0);
    lv_obj_set_style_pad_row(keypad, 4, 0);
    lv_obj_set_style_pad_column(keypad, 4, 0);
    lv_obj_set_style_radius(keypad, 3, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(keypad, lv_color_hex(0x292A32), LV_PART_ITEMS);
    lv_obj_set_style_bg_color(keypad, lv_color_hex(0x3A3C46), LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_border_color(keypad, lv_color_hex(0x1A1B22), LV_PART_ITEMS);
    lv_obj_set_style_border_width(keypad, 1, LV_PART_ITEMS);
    lv_obj_set_style_text_color(keypad, lv_color_white(), LV_PART_ITEMS);
    lv_obj_set_style_text_font(keypad, &lv_font_montserrat_32, LV_PART_ITEMS);
    lv_obj_add_event_cb(keypad, pin_keypad_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
}

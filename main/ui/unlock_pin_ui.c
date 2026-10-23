#include "unlock_pin_ui.h"
#include "UI_SCREEN2.h"

#include <stdint.h>
#include <string.h>

#include "app_fido.h"
#include "esp_log.h"
#include "esp_random.h"
#include "hide_menu_item_ui.h"
#include "lvgl.h"
#include "setup_ui.h"
#include "ui/i18n/ui_i18n.h"
#include "ui_nav.h"
#include "ui_password_manager.h"
#include "ui_status_bar.h"

#define UNLOCK_PIN_UI_WIDTH 480

LV_FONT_DECLARE(font_unlock_title_32);

static lv_obj_t *s_pin_textarea;
static uint8_t s_failed_pin_attempts;
static const char *s_pin_map[17];

static const char *const s_pin_digits[] = {
    "0", "1", "2", "3", "4", "5", "6", "7", "8", "9"
};

static void build_pin_map(bool scramble)
{
    uint8_t order[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 0};
    if (scramble) {
        for (int32_t i = 9; i > 0; i--) {
            uint32_t j = esp_random() % (uint32_t)(i + 1);
            uint8_t value = order[i];
            order[i] = order[j];
            order[j] = value;
        }
    }

    uint8_t map_index = 0;
    for (uint8_t i = 0; i < 9; i++) {
        s_pin_map[map_index++] = s_pin_digits[order[i]];
        if ((i + 1) % 3 == 0) {
            s_pin_map[map_index++] = "\n";
        }
    }
    s_pin_map[map_index++] = LV_SYMBOL_BACKSPACE;
    s_pin_map[map_index++] = s_pin_digits[order[9]];
    s_pin_map[map_index++] = LV_SYMBOL_OK;
    s_pin_map[map_index] = "";
}

static void open_password_manager(password_database_mode_t mode)
{
    s_failed_pin_attempts = 0;
    
    ui_password_manager_set_database_mode(mode);
    ui_nav_replace(ui_screen2_show);
}

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
        ESP_LOGI("PIN_PAGE", "PIN submitted");
        if (strcmp(pin, "123456") == 0) {
            ESP_LOGW("PIN_PAGE", "Emergency mode requested");
            open_password_manager(PASSWORD_DATABASE_MODE_EMERGENCY);
        } else if (strcmp(pin, "110110") == 0) {
            ESP_LOGW("PIN_PAGE", "Duress mode requested");
            open_password_manager(PASSWORD_DATABASE_MODE_DURESS);
        } else if (setup_ui_device_password_matches(pin)) {
            if (hide_menu_item_ui_is_visible(MAIN_MENU_ITEM_FIDO)) {
                esp_err_t ret = app_fido_start();
                if (ret != ESP_OK) {
                    ESP_LOGE("PIN_PAGE", "Failed to start FIDO: %s",
                             esp_err_to_name(ret));
                }
            } else {
                ESP_LOGI("PIN_PAGE", "FIDO menu item is disabled; skipping FIDO startup");
            }

            password_database_mode_t database_mode = PASSWORD_DATABASE_MODE_NORMAL;
            setup_security_mode_t security_mode = setup_ui_get_security_mode();
            if (security_mode == SETUP_SECURITY_MODE_EMERGENCY) {
                database_mode = PASSWORD_DATABASE_MODE_EMERGENCY;
            } else if (security_mode == SETUP_SECURITY_MODE_DURESS) {
                database_mode = PASSWORD_DATABASE_MODE_DURESS;
            }
            
            open_password_manager(database_mode);

        } else {
            ESP_LOGW("PIN_PAGE", "Wrong PIN");
            if (s_failed_pin_attempts < UINT8_MAX) {
                s_failed_pin_attempts++;
            }
            if (s_failed_pin_attempts >= setup_ui_get_pin_error_limit()) {
                ESP_LOGW("PIN_PAGE", "PIN error limit reached; entering emergency mode");
                open_password_manager(PASSWORD_DATABASE_MODE_EMERGENCY);
                return;
            }
            lv_textarea_set_text(s_pin_textarea, "");
            lv_obj_add_state(s_pin_textarea, LV_STATE_FOCUSED);
        }
    } else if (key[0] >= '0' && key[0] <= '9') {
        lv_obj_add_state(s_pin_textarea, LV_STATE_FOCUSED);
        lv_textarea_add_char(s_pin_textarea, key[0]);
    }
}

void unlock_pin_ui_show(void)
{
    esp_err_t i18n_ret = ui_i18n_init();
    if (i18n_ret != ESP_OK) {
        ESP_LOGW("PIN_PAGE", "Failed to initialize UI language: %s",
                 esp_err_to_name(i18n_ret));
    }

    lv_obj_t *screen = lv_screen_active();
    lv_obj_clean(screen);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x11141A), 0);
    lv_obj_set_style_pad_all(screen, 0, 0);

    ui_status_bar_add(screen, UNLOCK_PIN_UI_WIDTH);

    lv_obj_t *title = lv_label_create(screen);
    lv_label_set_text(title, ui_i18n_get(UI_STR_UNLOCK));
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_set_style_text_font(title, &font_unlock_title_32, 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 28, 71);

    s_pin_textarea = lv_textarea_create(screen);
    lv_obj_set_size(s_pin_textarea, 410, 120);
    lv_obj_align(s_pin_textarea, LV_ALIGN_TOP_MID, 0, 110);
    lv_textarea_set_one_line(s_pin_textarea, true);
    lv_textarea_set_password_mode(s_pin_textarea, setup_ui_get_pin_masking());
    lv_textarea_set_max_length(s_pin_textarea, 12);
    lv_textarea_set_placeholder_text(s_pin_textarea,
                                     ui_i18n_get(UI_STR_ENTER_PASSWORD));
    lv_textarea_set_cursor_click_pos(s_pin_textarea, true);
    lv_obj_add_event_cb(s_pin_textarea, pin_textarea_event_cb, LV_EVENT_ALL, NULL);
    lv_obj_set_style_bg_color(s_pin_textarea, lv_color_hex(0x2B2C34), 0);
    lv_obj_set_style_border_color(s_pin_textarea, lv_color_hex(0x31333C), 0);
    lv_obj_set_style_border_width(s_pin_textarea, 1, 0);
    lv_obj_set_style_radius(s_pin_textarea, 5, 0);
    lv_obj_set_style_text_color(s_pin_textarea, lv_color_white(), 0);
    lv_obj_set_style_text_font(s_pin_textarea, ui_i18n_font_35(), 0);
    lv_obj_set_style_text_color(s_pin_textarea, lv_color_hex(0x8D9099), LV_PART_TEXTAREA_PLACEHOLDER);
    lv_obj_set_style_bg_color(s_pin_textarea, lv_color_white(), LV_PART_CURSOR | LV_STATE_FOCUSED);
    lv_obj_set_style_bg_opa(s_pin_textarea, LV_OPA_COVER, LV_PART_CURSOR | LV_STATE_FOCUSED);
    lv_obj_set_style_width(s_pin_textarea, 2, LV_PART_CURSOR | LV_STATE_FOCUSED);
    lv_obj_set_style_anim_duration(s_pin_textarea, 500, LV_PART_CURSOR | LV_STATE_FOCUSED);
    lv_obj_set_style_pad_left(s_pin_textarea, 18, 0);
    lv_obj_set_style_pad_right(s_pin_textarea, 18, 0);
    lv_obj_set_style_pad_top(s_pin_textarea, 18, 0);
    lv_obj_set_style_pad_bottom(s_pin_textarea, 18, 0);

    build_pin_map(setup_ui_get_pin_scramble());
    lv_obj_t *keypad = lv_buttonmatrix_create(screen);
    lv_buttonmatrix_set_map(keypad, s_pin_map);
    lv_obj_set_size(keypad, UNLOCK_PIN_UI_WIDTH - 12, 376);
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

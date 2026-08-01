#include "setup_ui.h"

#include <stdint.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "lvgl.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "ui_nav.h"
#include "usb_msc.h"

#define SETUP_UI_WIDTH  480
#define SETUP_UI_HEIGHT 640
#define DEVICE_PASSWORD_MAX 12
#define DEVICE_PASSWORD_DEFAULT "1234"
#define DEVICE_PASSWORD_NVS_NAMESPACE "device_auth"
#define DEVICE_PASSWORD_NVS_KEY "pin"

static const char *TAG = "setup_ui";
static lv_obj_t *s_password_status_label;
static lv_obj_t *s_password_fields[3];
static uint8_t s_password_active_field;

static esp_err_t setup_nvs_ready(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    return ret;
}

static esp_err_t read_device_password(char *out, size_t out_len)
{
    if (!out || out_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    strlcpy(out, DEVICE_PASSWORD_DEFAULT, out_len);

    esp_err_t ret = setup_nvs_ready();
    if (ret != ESP_OK) {
        return ret;
    }

    nvs_handle_t nvs;
    ret = nvs_open(DEVICE_PASSWORD_NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (ret != ESP_OK) {
        return ret;
    }

    size_t len = out_len;
    ret = nvs_get_str(nvs, DEVICE_PASSWORD_NVS_KEY, out, &len);
    nvs_close(nvs);
    return ret == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : ret;
}

static esp_err_t write_device_password(const char *pin)
{
    if (!pin || pin[0] == '\0' || strlen(pin) > DEVICE_PASSWORD_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = setup_nvs_ready();
    if (ret != ESP_OK) {
        return ret;
    }

    nvs_handle_t nvs;
    ret = nvs_open(DEVICE_PASSWORD_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = nvs_set_str(nvs, DEVICE_PASSWORD_NVS_KEY, pin);
    if (ret == ESP_OK) {
        ret = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return ret;
}

bool setup_ui_device_password_matches(const char *pin)
{
    char stored[DEVICE_PASSWORD_MAX + 1];
    esp_err_t ret = read_device_password(stored, sizeof(stored));
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to read device password: %s", esp_err_to_name(ret));
        strlcpy(stored, DEVICE_PASSWORD_DEFAULT, sizeof(stored));
    }
    return pin && strcmp(pin, stored) == 0;
}

static void add_status_bar(lv_obj_t *screen)
{
    lv_obj_t *status = lv_obj_create(screen);
    lv_obj_remove_style_all(status);
    lv_obj_set_size(status, SETUP_UI_WIDTH, 100);
    lv_obj_align(status, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(status, lv_color_hex(0x0F1117), 0);
    lv_obj_set_style_bg_opa(status, LV_OPA_COVER, 0);

    lv_obj_t *time_label = lv_label_create(status);
    lv_label_set_text(time_label, "07-30 16:47");
    lv_obj_set_style_text_color(time_label, lv_color_hex(0x9EA3AD), 0);
    lv_obj_set_style_text_font(time_label, &lv_font_montserrat_28, 0);
    lv_obj_align(time_label, LV_ALIGN_TOP_RIGHT, -60, 3);

    lv_obj_t *battery = lv_label_create(status);
    lv_label_set_text(battery, LV_SYMBOL_BATTERY_FULL);
    lv_obj_set_style_text_color(battery, lv_color_hex(0x9EA3AD), 0);
    lv_obj_set_style_text_font(battery, &lv_font_montserrat_28, 0);
    lv_obj_align(battery, LV_ALIGN_TOP_RIGHT, -8, 3);
}

static void usb_disk_switch_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_VALUE_CHANGED) {
        return;
    }

    lv_obj_t *sw = lv_event_get_target(event);
    esp_err_t ret = lv_obj_has_state(sw, LV_STATE_CHECKED)
                       ? usb_hid_enable_usbstore()
                       : usb_hid_disable_usbstore();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to switch usbstore: %s", esp_err_to_name(ret));
    }
}

static void add_usb_disk_switch(lv_obj_t *parent)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, SETUP_UI_WIDTH - 50, 96);
    lv_obj_set_style_bg_color(row, lv_color_hex(0x292A32), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(row, 4, 0);
    lv_obj_set_style_pad_all(row, 0, 0);

    lv_obj_t *label = lv_label_create(row);
    lv_label_set_text(label, "u-disk switch");
    lv_obj_set_width(label, SETUP_UI_WIDTH - 190);
    lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_28, 0);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, 20, 0);

    lv_obj_t *sw = lv_switch_create(row);
    lv_obj_set_size(sw, 82, 44);
    lv_obj_align(sw, LV_ALIGN_RIGHT_MID, -18, 0);
    lv_obj_set_style_bg_color(sw, lv_color_hex(0x3A3C46), LV_PART_MAIN);
    lv_obj_set_style_bg_color(sw, lv_color_hex(0xA2ED8B), LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(sw, lv_color_white(), LV_PART_KNOB);
    if (usb_hid_usbstore_is_exposed_to_usb()) {
        lv_obj_add_state(sw, LV_STATE_CHECKED);
    }
    lv_obj_add_event_cb(sw, usb_disk_switch_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
}

static void password_field_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }

    s_password_active_field = (uint8_t)(uintptr_t)lv_event_get_user_data(event);
    for (uint8_t i = 0; i < 3; i++) {
        if (s_password_fields[i]) {
            if (i == s_password_active_field) {
                lv_obj_add_state(s_password_fields[i], LV_STATE_FOCUSED);
            } else {
                lv_obj_clear_state(s_password_fields[i], LV_STATE_FOCUSED);
            }
        }
    }
}

static void update_password_field_focus(void)
{
    for (uint8_t i = 0; i < 3; i++) {
        if (s_password_fields[i]) {
            if (i == s_password_active_field) {
                lv_obj_add_state(s_password_fields[i], LV_STATE_FOCUSED);
            } else {
                lv_obj_clear_state(s_password_fields[i], LV_STATE_FOCUSED);
            }
        }
    }
}

static void set_password_status(const char *text, lv_color_t color)
{
    if (s_password_status_label) {
        lv_label_set_text(s_password_status_label, text);
        lv_obj_set_style_text_color(s_password_status_label, color, 0);
    }
}

static void save_password_from_fields(void)
{
    const char *current = lv_textarea_get_text(s_password_fields[0]);
    const char *new_pin = lv_textarea_get_text(s_password_fields[1]);
    const char *confirm = lv_textarea_get_text(s_password_fields[2]);

    if (!setup_ui_device_password_matches(current)) {
        set_password_status("Current password wrong", lv_color_hex(0xF27A7A));
        lv_textarea_set_text(s_password_fields[0], "");
        s_password_active_field = 0;
        lv_obj_add_state(s_password_fields[0], LV_STATE_FOCUSED);
        return;
    }

    if (!new_pin || new_pin[0] == '\0') {
        set_password_status("New password is empty", lv_color_hex(0xF27A7A));
        return;
    }

    if (strcmp(new_pin, confirm) != 0) {
        set_password_status("Confirm password mismatch", lv_color_hex(0xF27A7A));
        lv_textarea_set_text(s_password_fields[2], "");
        s_password_active_field = 2;
        lv_obj_add_state(s_password_fields[2], LV_STATE_FOCUSED);
        return;
    }

    esp_err_t ret = write_device_password(new_pin);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to save device password: %s", esp_err_to_name(ret));
        set_password_status("Save failed", lv_color_hex(0xF27A7A));
        return;
    }

    for (uint8_t i = 0; i < 3; i++) {
        lv_textarea_set_text(s_password_fields[i], "");
    }
    s_password_active_field = 0;
    lv_obj_add_state(s_password_fields[0], LV_STATE_FOCUSED);
    set_password_status("Password updated", lv_color_hex(0xA2ED8B));
}

static void password_keypad_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_VALUE_CHANGED) {
        return;
    }

    lv_obj_t *button_matrix = lv_event_get_target_obj(event);
    uint32_t button_id = lv_buttonmatrix_get_selected_button(button_matrix);
    const char *key = lv_buttonmatrix_get_button_text(button_matrix, button_id);
    lv_obj_t *field = s_password_fields[s_password_active_field];
    if (!key || !field) {
        return;
    }

    if (strcmp(key, LV_SYMBOL_BACKSPACE) == 0) {
        lv_textarea_delete_char(field);
    } else if (strcmp(key, LV_SYMBOL_OK) == 0) {
        save_password_from_fields();
    } else if (key[0] >= '0' && key[0] <= '9') {
        lv_textarea_add_char(field, key[0]);
        if (strlen(lv_textarea_get_text(field)) >= 4 && s_password_active_field < 2) {
            s_password_active_field++;
            update_password_field_focus();
        }
    }
}

static lv_obj_t *add_password_textarea(lv_obj_t *screen, const char *placeholder, uint8_t index, int32_t y)
{
    lv_obj_t *textarea = lv_textarea_create(screen);
    lv_obj_set_size(textarea, SETUP_UI_WIDTH - 50, 58);
    lv_obj_align(textarea, LV_ALIGN_TOP_MID, 0, y);
    lv_textarea_set_one_line(textarea, true);
    lv_textarea_set_password_mode(textarea, true);
    lv_textarea_set_max_length(textarea, DEVICE_PASSWORD_MAX);
    lv_textarea_set_placeholder_text(textarea, placeholder);
    lv_obj_add_event_cb(textarea, password_field_event_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)index);
    lv_obj_set_style_bg_color(textarea, lv_color_hex(0x2B2C34), 0);
    lv_obj_set_style_bg_color(textarea, lv_color_hex(0x343744), LV_STATE_FOCUSED);
    lv_obj_set_style_border_color(textarea, lv_color_hex(0x31333C), 0);
    lv_obj_set_style_border_color(textarea, lv_color_hex(0x6DD6F2), LV_STATE_FOCUSED);
    lv_obj_set_style_border_width(textarea, 1, 0);
    lv_obj_set_style_radius(textarea, 5, 0);
    lv_obj_set_style_text_color(textarea, lv_color_white(), 0);
    lv_obj_set_style_text_color(textarea, lv_color_hex(0x8D9099), LV_PART_TEXTAREA_PLACEHOLDER);
    lv_obj_set_style_text_font(textarea, &lv_font_montserrat_24, 0);
    lv_obj_set_style_pad_left(textarea, 16, 0);
    lv_obj_set_style_pad_right(textarea, 16, 0);
    return textarea;
}

static void setup_password_page_show(void)
{
    lv_obj_t *screen = lv_screen_active();
    lv_obj_clean(screen);
    ui_nav_enable_right_swipe(screen);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x11141A), 0);
    lv_obj_set_style_pad_all(screen, 0, 0);

    lv_obj_t *title = lv_label_create(screen);
    lv_label_set_text(title, "Device Password");
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 25, 30);

    s_password_status_label = lv_label_create(screen);
    lv_label_set_text(s_password_status_label, "Enter current and new PIN");
    lv_obj_set_width(s_password_status_label, SETUP_UI_WIDTH - 50);
    lv_label_set_long_mode(s_password_status_label, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_color(s_password_status_label, lv_color_hex(0x9EA3AD), 0);
    lv_obj_set_style_text_font(s_password_status_label, &lv_font_montserrat_16, 0);
    lv_obj_align(s_password_status_label, LV_ALIGN_TOP_LEFT, 25, 64);

    s_password_fields[0] = add_password_textarea(screen, "Current password", 0, 96);
    s_password_fields[1] = add_password_textarea(screen, "New password", 1, 166);
    s_password_fields[2] = add_password_textarea(screen, "Confirm password", 2, 236);
    s_password_active_field = 0;
    lv_obj_add_state(s_password_fields[0], LV_STATE_FOCUSED);

    static const char *pin_map[] = {
        "1", "2", "3", "\n",
        "4", "5", "6", "\n",
        "7", "8", "9", "\n",
        LV_SYMBOL_BACKSPACE, "0", LV_SYMBOL_OK, ""
    };

    lv_obj_t *keypad = lv_buttonmatrix_create(screen);
    lv_buttonmatrix_set_map(keypad, pin_map);
    lv_obj_set_size(keypad, SETUP_UI_WIDTH - 12, 300);
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
    lv_obj_set_style_text_font(keypad, &lv_font_montserrat_28, LV_PART_ITEMS);
    lv_obj_add_event_cb(keypad, password_keypad_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
}

static void password_menu_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_CLICKED) {
        ui_nav_push(setup_ui_show, setup_password_page_show);
    }
}

static void add_password_menu_item(lv_obj_t *parent)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, SETUP_UI_WIDTH - 50, 96);
    lv_obj_set_style_bg_color(row, lv_color_hex(0x292A32), 0);
    lv_obj_set_style_bg_color(row, lv_color_hex(0x3A3C46), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(row, 4, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row, password_menu_event_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *label = lv_label_create(row);
    lv_label_set_text(label, "device password");
    lv_obj_set_width(label, SETUP_UI_WIDTH - 120);
    lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_28, 0);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, 20, 0);

    lv_obj_t *arrow = lv_label_create(row);
    lv_label_set_text(arrow, LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_color(arrow, lv_color_hex(0x9EA3AD), 0);
    lv_obj_set_style_text_font(arrow, &lv_font_montserrat_28, 0);
    lv_obj_align(arrow, LV_ALIGN_RIGHT_MID, -18, 0);
}

static void sc_setting_page_show(void)
{
    lv_obj_t *screen = lv_screen_active();
    lv_obj_clean(screen);
    ui_nav_enable_right_swipe(screen);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x11141A), 0);
    lv_obj_set_style_pad_all(screen, 0, 0);

    lv_obj_t *title = lv_label_create(screen);
    lv_label_set_text(title, "SC setting");
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_32, 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 25, 42);

    lv_obj_t *list = lv_obj_create(screen);
    lv_obj_remove_style_all(list);
    lv_obj_set_size(list, SETUP_UI_WIDTH, 430);
    lv_obj_align(list, LV_ALIGN_TOP_MID, 0, 118);
    lv_obj_set_style_pad_left(list, 25, 0);
    lv_obj_set_style_pad_right(list, 25, 0);
    lv_obj_set_style_pad_top(list, 0, 0);
    lv_obj_set_style_pad_bottom(list, 0, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list, 20, 0);
    lv_obj_clear_flag(list, LV_OBJ_FLAG_SCROLLABLE);

    const char *labels[] = {"OpenPGP", "PIV"};
    for (size_t i = 0; i < sizeof(labels) / sizeof(labels[0]); i++) {
        lv_obj_t *row = lv_obj_create(list);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, SETUP_UI_WIDTH - 50, 96);
        lv_obj_set_style_bg_color(row, lv_color_hex(0x292A32), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(row, 4, 0);
        lv_obj_set_style_pad_all(row, 0, 0);

        lv_obj_t *label = lv_label_create(row);
        lv_label_set_text(label, labels[i]);
        lv_obj_set_width(label, SETUP_UI_WIDTH - 190);
        lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
        lv_obj_set_style_text_color(label, lv_color_white(), 0);
        lv_obj_set_style_text_font(label, &lv_font_montserrat_28, 0);
        lv_obj_align(label, LV_ALIGN_LEFT_MID, 20, 0);

        lv_obj_t *sw = lv_switch_create(row);
        lv_obj_set_size(sw, 82, 44);
        lv_obj_align(sw, LV_ALIGN_RIGHT_MID, -18, 0);
        lv_obj_set_style_bg_color(sw, lv_color_hex(0x3A3C46), LV_PART_MAIN);
        lv_obj_set_style_bg_color(sw, lv_color_hex(0xA2ED8B), LV_PART_INDICATOR | LV_STATE_CHECKED);
        lv_obj_set_style_bg_color(sw, lv_color_white(), LV_PART_KNOB);
    }
}

static void sc_setting_menu_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_CLICKED) {
        ui_nav_push(setup_ui_show, sc_setting_page_show);
    }
}

static void add_sc_setting_menu_item(lv_obj_t *parent)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, SETUP_UI_WIDTH - 50, 96);
    lv_obj_set_style_bg_color(row, lv_color_hex(0x292A32), 0);
    lv_obj_set_style_bg_color(row, lv_color_hex(0x3A3C46), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(row, 4, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row, sc_setting_menu_event_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *label = lv_label_create(row);
    lv_label_set_text(label, "SC setting");
    lv_obj_set_width(label, SETUP_UI_WIDTH - 120);
    lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_28, 0);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, 20, 0);

    lv_obj_t *arrow = lv_label_create(row);
    lv_label_set_text(arrow, LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_color(arrow, lv_color_hex(0x9EA3AD), 0);
    lv_obj_set_style_text_font(arrow, &lv_font_montserrat_28, 0);
    lv_obj_align(arrow, LV_ALIGN_RIGHT_MID, -18, 0);
}

void setup_ui_show(void)
{
    lv_obj_t *screen = lv_screen_active();
    lv_obj_clean(screen);
    ui_nav_enable_right_swipe(screen);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x11141A), 0);
    lv_obj_set_style_pad_all(screen, 0, 0);

    add_status_bar(screen);

    lv_obj_t *title = lv_label_create(screen);
    lv_label_set_text(title, "system setup");
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_32, 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 25, 82);

    lv_obj_t *list = lv_obj_create(screen);
    lv_obj_remove_style_all(list);
    lv_obj_set_size(list, SETUP_UI_WIDTH, SETUP_UI_HEIGHT - 140);
    lv_obj_align(list, LV_ALIGN_TOP_MID, 0, 140);
    lv_obj_set_style_pad_left(list, 25, 0);
    lv_obj_set_style_pad_right(list, 25, 0);
    lv_obj_set_style_pad_top(list, 0, 0);
    lv_obj_set_style_pad_bottom(list, 0, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list, 20, 0);
    lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, LV_PART_SCROLLBAR);

    add_usb_disk_switch(list);
    add_password_menu_item(list);
    add_sc_setting_menu_item(list);
}

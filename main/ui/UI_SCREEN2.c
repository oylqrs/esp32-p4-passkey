#include "UI_SCREEN2.h"

#include <stdint.h>

#include "esp_log.h"
#include "esp_system.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "FIDO_Management.h"
#include "Serach_UI.h"
#include "setup_ui.h"
#include "smartcard_ui.h"
#include "ui_password_manager.h"
#include "ui_new_entry.h"
#include "ui_script_detail.h"
#include "lvgl.h"
#include "ui_nav.h"
#include "usb_hid_keyboard.h"

#define UI_SCREEN2_WIDTH  480
#define LCD_BACKLIGHT_GPIO GPIO_NUM_19

static const char *TAG = "ui_screen2";

static void powerdown_reset(void)
{
    ESP_LOGW(TAG, "Powerdown selected: turn off backlight before restart");
    gpio_set_direction(LCD_BACKLIGHT_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(LCD_BACKLIGHT_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(150));
    esp_restart();
}

typedef struct {
    const char *symbol;
    const char *text;
    lv_color_t icon_bg;
} menu_item_t;

static const lv_font_t *ui_screen2_text_font(void)
{

    return &lv_font_montserrat_28;

}

static void add_status_bar(lv_obj_t *screen)
{
    lv_obj_t *status = lv_obj_create(screen);
    lv_obj_remove_style_all(status);
    lv_obj_set_size(status, UI_SCREEN2_WIDTH, 100);//28
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

static void menu_item_event_cb(lv_event_t *event)
{
    uintptr_t index = (uintptr_t)lv_event_get_user_data(event);
    if (index == 0) {
        ui_nav_push(ui_screen2_show, ui_password_manager_show);
    } else if (index == 1) {
        ui_nav_push(ui_screen2_show, serach_ui_show);
    } else if (index == 2) {
        ui_nav_push(ui_screen2_show, ui_new_entry_show);
    } else if (index == 3) {
        ui_nav_push(ui_screen2_show, FIDO_Management_show);
    } else if (index == 4) {
        ui_nav_push(ui_screen2_show, smartcard_ui_show);
    } else if (index == 6) {
        ui_nav_push(ui_screen2_show, ui_script_detail_show);
    } else if (index == 7) {
        ui_nav_push(ui_screen2_show, setup_ui_show);
    } else if (index == 8) {
        powerdown_reset();
    }
}

static void add_menu_item(lv_obj_t *parent, const menu_item_t *item, size_t index)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, UI_SCREEN2_WIDTH - 50, 139);//list height
    lv_obj_set_style_bg_color(row, lv_color_hex(0x292A32), 0);
    lv_obj_set_style_bg_color(row, lv_color_hex(0x3A3C46), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(row, 4, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row, menu_item_event_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)index);

    lv_obj_t *icon_box = lv_obj_create(row);
    lv_obj_remove_style_all(icon_box);
    lv_obj_set_size(icon_box, 88, 88);
    lv_obj_align(icon_box, LV_ALIGN_LEFT_MID, 18, 0);
    lv_obj_set_style_bg_color(icon_box, item->icon_bg, 0);
    lv_obj_set_style_bg_opa(icon_box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(icon_box, lv_color_hex(0x11141A), 0);
    lv_obj_set_style_border_width(icon_box, 2, 0);
    lv_obj_set_style_radius(icon_box, 8, 0);

    lv_obj_t *icon = lv_label_create(icon_box);
    lv_label_set_text(icon, item->symbol);
    lv_obj_set_style_text_color(icon, lv_color_hex(0x11141A), 0);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_28, 0);
    lv_obj_center(icon);

    lv_obj_t *label = lv_label_create(row);
    lv_label_set_text(label, item->text);
    lv_obj_set_width(label, UI_SCREEN2_WIDTH - 164);
    lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_set_style_text_font(label, ui_screen2_text_font(), 0);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, 128, 0);
}

void ui_screen2_show(void)
{
    ui_nav_set_root(ui_screen2_show);

    esp_err_t ret = usb_hid_disable_smartcard_reader();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "SmartCard reader disable failed: %s", esp_err_to_name(ret));
    }

    lv_obj_t *screen = lv_screen_active();
    lv_obj_clean(screen);
    ui_nav_enable_right_swipe(screen);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x11141A), 0);
    lv_obj_set_style_pad_all(screen, 0, 0);

    add_status_bar(screen);

    lv_obj_t *list = lv_obj_create(screen);
    lv_obj_remove_style_all(list);
    lv_obj_set_size(list, UI_SCREEN2_WIDTH, 580);

    lv_obj_align(list, LV_ALIGN_TOP_MID, 0, 50);
    lv_obj_set_style_pad_left(list, 25, 0);
    lv_obj_set_style_pad_right(list, 25, 0);
    lv_obj_set_style_pad_top(list, 0, 0);
    lv_obj_set_style_pad_bottom(list, 0, 0);
    
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);

    lv_obj_set_style_pad_row(list, 30, 0);//10
    lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, LV_PART_SCROLLBAR);

    static const menu_item_t items[] = {
        {LV_SYMBOL_KEYBOARD, "password manager", LV_COLOR_MAKE(0x6D, 0xD6, 0xF2)},
        {LV_SYMBOL_EYE_OPEN, "Search",     LV_COLOR_MAKE(0xBA, 0xEE, 0xFF)},
        {LV_SYMBOL_PLUS,     "new",     LV_COLOR_MAKE(0x9E, 0xE6, 0xEA)},
        {LV_SYMBOL_KEYBOARD, "FIDO",     LV_COLOR_MAKE(0xB8, 0xA8, 0xFF)},
        {LV_SYMBOL_SD_CARD,  "SmartCard",   LV_COLOR_MAKE(0xA2, 0xED, 0x8B)},
        {LV_SYMBOL_REFRESH,  "Chameleon",   LV_COLOR_MAKE(0x62, 0xD6, 0x73)},
        {LV_SYMBOL_FILE,     "Script",     LV_COLOR_MAKE(0xF2, 0x7A, 0x7A)},
        {LV_SYMBOL_SETTINGS, "Setup",     LV_COLOR_MAKE(0xFF, 0xC9, 0x58)},
        {LV_SYMBOL_POWER,    "Powerdown",     LV_COLOR_MAKE(0xFF, 0x7C, 0x88)},
    };

    for (size_t i = 0; i < sizeof(items) / sizeof(items[0]); i++) {
        add_menu_item(list, &items[i], i);
    }
}

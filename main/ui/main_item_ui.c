#include "UI_SCREEN2.h"

#include <stdint.h>

#include "esp_log.h"
#include "esp_system.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "powerdown_state.h"
#include "FIDO_Management.h"
#include "Serach_UI.h"
#include "hide_menu_item_ui.h"
#include "setup_ui.h"
#include "UI_SmartCard.h"
#include "chameleon_manager_ui.h"
#include "ui_password_manager.h"
#include "ui_new_entry.h"
#include "ui_script_detail.h"
#include "wallet/jade_wallet_ui.h"
#include "lvgl.h"
#include "ui_nav.h"
#include "ui/i18n/ui_i18n.h"
#include "ui_status_bar.h"
#include "usb_hid_keyboard.h"

#define UI_SCREEN2_WIDTH  480
#define UI_SCREEN2_ITEM_HEIGHT 145
#define UI_SCREEN2_TEXT_WIDTH (UI_SCREEN2_WIDTH - 172)
#define LCD_BACKLIGHT_GPIO GPIO_NUM_19

static const char *TAG = "ui_screen2";

LV_IMAGE_DECLARE(padlock_opaque);
LV_IMAGE_DECLARE(transparency_opaque);
LV_IMAGE_DECLARE(add_user_opaque);
LV_IMAGE_DECLARE(fido_password_opaque);
LV_IMAGE_DECLARE(chip_opaque);
LV_IMAGE_DECLARE(chameleon_opaque);
LV_IMAGE_DECLARE(bitcoin_opaque);
LV_IMAGE_DECLARE(flash_drive_opaque);
LV_IMAGE_DECLARE(gear_opaque);
LV_IMAGE_DECLARE(power_button_opaque);

static void powerdown_reset(void)
{
    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(POWERDOWN_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (ret == ESP_OK) {
        ret = nvs_set_u8(nvs, POWERDOWN_NVS_KEY, 1);
        if (ret == ESP_OK) {
            ret = nvs_commit(nvs);
        }
        nvs_close(nvs);
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save powerdown request: %s", esp_err_to_name(ret));
        return;
    }

    ESP_LOGW(TAG, "Powerdown selected: turn off backlight before restart");
    gpio_set_direction(LCD_BACKLIGHT_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(LCD_BACKLIGHT_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(150));
    esp_restart();
}

typedef struct {
    const lv_image_dsc_t *image;
    ui_string_id_t text_id;
} menu_item_t;

static const lv_font_t *ui_screen2_text_font(void)
{
    return ui_i18n_font_42();
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
        ui_nav_push(ui_screen2_show, ui_smartcard_show);
    } else if (index == 5) {
        ui_nav_push(ui_screen2_show, chameleon_manager_ui_show);
    } else if (index == 6) {
        ui_nav_push(ui_screen2_show, jade_wallet_ui_show);
    } else if (index == 7) {
        ui_nav_push(ui_screen2_show, ui_script_detail_show);
    } else if (index == 8) {
        ui_nav_push(ui_screen2_show, setup_ui_show);
    } else if (index == 9) {
        powerdown_reset();
    }
}

static void add_menu_item(lv_obj_t *parent, const menu_item_t *item, size_t index)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, UI_SCREEN2_WIDTH - 32, UI_SCREEN2_ITEM_HEIGHT);
    lv_obj_set_style_bg_color(row, lv_color_hex(0x292A32), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row, menu_item_event_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)index);

    lv_obj_t *icon_box = lv_obj_create(row);
    lv_obj_remove_style_all(icon_box);
    lv_obj_set_size(icon_box, 100, 100);
    lv_obj_align(icon_box, LV_ALIGN_LEFT_MID, 12, 0);
    lv_obj_clear_flag(icon_box, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *icon = lv_image_create(icon_box);
    lv_image_set_src(icon, item->image);
    lv_obj_center(icon);

    lv_obj_t *label = lv_label_create(row);
    lv_label_set_text(label, ui_i18n_get(item->text_id));
    lv_obj_set_width(label, UI_SCREEN2_TEXT_WIDTH);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_line_space(label, 0, 0);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_set_style_text_font(label, ui_screen2_text_font(), 0);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, 124, 0);
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
    ui_nav_disable_right_swipe(screen);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x11141A), 0);
    lv_obj_set_style_pad_all(screen, 0, 0);

    ui_status_bar_add(screen, UI_SCREEN2_WIDTH);

    lv_obj_t *list = lv_obj_create(screen);
    lv_obj_remove_style_all(list);
    lv_obj_set_size(list, UI_SCREEN2_WIDTH, 580);

    lv_obj_align(list, LV_ALIGN_TOP_MID, 0, 50);
    lv_obj_set_style_pad_left(list, 16, 0);
    lv_obj_set_style_pad_right(list, 16, 0);
    lv_obj_set_style_pad_top(list, 0, 0);
    lv_obj_set_style_pad_bottom(list, 0, 0);

    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);

    lv_obj_set_style_pad_row(list, 25, 0);
    lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(list, LV_OBJ_FLAG_SCROLL_CHAIN_HOR);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, LV_PART_SCROLLBAR);

    static const menu_item_t items[] = {
        {&padlock_opaque,       UI_STR_PASSWORD_MANAGER},
        {&transparency_opaque,  UI_STR_SEARCH},
        {&add_user_opaque,      UI_STR_NEW_ENTRY},
        {&fido_password_opaque, UI_STR_FIDO},
        {&chip_opaque,          UI_STR_SMARTCARD},
        {&chameleon_opaque,     UI_STR_CHAMELEON},
        {&bitcoin_opaque,       UI_STR_WALLET},
        {&flash_drive_opaque,   UI_STR_SCRIPT},
        {&gear_opaque,          UI_STR_SETUP},
        {&power_button_opaque,  UI_STR_POWERDOWN},
    };

    for (size_t i = 0; i < sizeof(items) / sizeof(items[0]); i++) {
        main_menu_item_id_t item_id = i == 6 ? MAIN_MENU_ITEM_WALLET
                                            : i == 7 ? MAIN_MENU_ITEM_SCRIPT
                                                     : (main_menu_item_id_t)i;
        if (i < 8 && !hide_menu_item_ui_is_visible(item_id)) {
            continue;
        }
        add_menu_item(list, &items[i], i);
    }
}

#include "smartcard_ui.h"

#include <stddef.h>
#include <stdint.h>

#include "canokey_esp32p4.h"
#include "esp_log.h"
#include "lvgl.h"
#include "openpgp_ui.h"
#include "ui_piv_detail.h"
#include "ui_nav.h"
#include "usb_ccid.h"
#include "usb_hid_keyboard.h"

#define SMARTCARD_UI_WIDTH 480

static const char *TAG = "smartcard_ui";
static void smartcard_ui_show_after_child(void);

typedef struct {
    const char *symbol;
    const char *text;
    lv_color_t icon_bg;
} smartcard_item_t;

static const lv_font_t *smartcard_text_font(void)
{
    return &lv_font_montserrat_28;
}

static void add_status_bar(lv_obj_t *screen)
{
    lv_obj_t *status = lv_obj_create(screen);
    lv_obj_remove_style_all(status);
    lv_obj_set_size(status, SMARTCARD_UI_WIDTH, 100);
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

static void smartcard_item_event_cb(lv_event_t *event)
{
    uintptr_t index = (uintptr_t)lv_event_get_user_data(event);
    if (index == 0) {
        ui_nav_push(smartcard_ui_show_after_child, openpgp_ui_show);
    } else if (index == 1) {
        ui_nav_push(smartcard_ui_show_after_child, ui_piv_detail_show);
    }
}

static void add_smartcard_item(lv_obj_t *parent, const smartcard_item_t *item, size_t index)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, SMARTCARD_UI_WIDTH - 50, 139);
    lv_obj_set_style_bg_color(row, lv_color_hex(0x292A32), 0);
    lv_obj_set_style_bg_color(row, lv_color_hex(0x3A3C46), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(row, 4, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row, smartcard_item_event_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)index);

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
    lv_obj_set_width(label, SMARTCARD_UI_WIDTH - 164);
    lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_set_style_text_font(label, smartcard_text_font(), 0);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, 128, 0);
}

void smartcard_ui_show(void)
{
    esp_err_t ret = canokey_esp32p4_start();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "CanoKey start failed: %s", esp_err_to_name(ret));
    }
    ret = usb_hid_enable_smartcard_reader();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "SmartCard reader enable failed: %s", esp_err_to_name(ret));
    }

    lv_obj_t *screen = lv_screen_active();
    lv_obj_clean(screen);
    ui_nav_enable_right_swipe(screen);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x11141A), 0);
    lv_obj_set_style_pad_all(screen, 0, 0);

    add_status_bar(screen);

    lv_obj_t *list = lv_obj_create(screen);
    lv_obj_remove_style_all(list);
    lv_obj_set_size(list, SMARTCARD_UI_WIDTH, 580);
    lv_obj_align(list, LV_ALIGN_TOP_MID, 0, 50);
    lv_obj_set_style_pad_left(list, 25, 0);
    lv_obj_set_style_pad_right(list, 25, 0);
    lv_obj_set_style_pad_top(list, 0, 0);
    lv_obj_set_style_pad_bottom(list, 0, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list, 30, 0);
    lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, LV_PART_SCROLLBAR);

    static const smartcard_item_t items[] = {
        {LV_SYMBOL_SD_CARD, "OpenPGP", LV_COLOR_MAKE(0xA2, 0xED, 0x8B)},
        {LV_SYMBOL_SD_CARD, "PIV",     LV_COLOR_MAKE(0xFF, 0xC9, 0x58)},
    };

    for (size_t i = 0; i < sizeof(items) / sizeof(items[0]); i++) {
        add_smartcard_item(list, &items[i], i);
    }
}

static void smartcard_ui_show_after_child(void)
{
    usb_ccid_deactivate_card();
    smartcard_ui_show();
}

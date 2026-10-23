/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * LVGL adaptation of the Blockstream Jade dashboard UI for ESP32-P4.
 */
#include "jade_wallet_ui.h"

#include <stdint.h>

#include "esp_app_desc.h"
#include "esp_err.h"
#include "jade_ui_model.h"
#include "jade_usb_protocol.h"
#include "lvgl.h"
#include "ui/i18n/ui_i18n.h"
#include "ui/ui_nav.h"
#include "ui/ui_status_bar.h"
#include "wallet_service.h"
#include "wallet_flow_ui.h"

#define JADE_UI_WIDTH 480
#define JADE_PANEL_WIDTH 372
#define JADE_PANEL_HEIGHT 360
#define JADE_HEADER_TOP 56
#define JADE_HEADER_HEIGHT 72

static bool s_internal_navigation;

static void show_setup_async(void *unused)
{
    (void)unused;
    wallet_flow_show_setup();
}

static void show_unlock_async(void *unused)
{
    (void)unused;
    wallet_flow_show_unlock();
}

static void show_receive_async(void *unused)
{
    (void)unused;
    wallet_flow_show_receive();
}

static void show_options_async(void *unused)
{
    (void)unused;
    wallet_flow_show_options();
}

static ui_string_id_t text_id(jade_ui_text_t text)
{
    static const ui_string_id_t ids[] = {
        [JADE_UI_TEXT_SET_UP] = UI_STR_WALLET_SET_UP,
        [JADE_UI_TEXT_SCAN_SEED_QR] = UI_STR_WALLET_SCAN_SEED_QR,
        [JADE_UI_TEXT_READY] = UI_STR_WALLET_READY,
        [JADE_UI_TEXT_QR_MODE] = UI_STR_WALLET_QR_MODE,
        [JADE_UI_TEXT_SESSION] = UI_STR_WALLET_SESSION,
        [JADE_UI_TEXT_SCAN_QR] = UI_STR_WALLET_SCAN_QR,
        [JADE_UI_TEXT_OPTIONS] = UI_STR_WALLET_OPTIONS,
    };
    return text < sizeof(ids) / sizeof(ids[0]) ? ids[text] : UI_STR_WALLET;
}

static ui_string_id_t state_text_id(jade_ui_state_t state)
{
    switch (state) {
    case JADE_UI_STATE_UNINITIALIZED:
        return UI_STR_WALLET_UNINITIALIZED;
    case JADE_UI_STATE_INITIALIZED:
        return UI_STR_WALLET_INITIALIZED;
    case JADE_UI_STATE_ACTIVE:
        return UI_STR_WALLET_ACTIVE;
    case JADE_UI_STATE_UNAVAILABLE:
    default:
        return UI_STR_WALLET_UNAVAILABLE;
    }
}

static void leave_wallet(void)
{
    (void)jade_usb_protocol_stop();
    wallet_service_leave();
    ui_nav_go_back();
}

static void wallet_content_delete_cb(lv_event_t *event)
{
    (void)event;
    if (!s_internal_navigation) {
        (void)jade_usb_protocol_stop();
        wallet_service_leave();
    }
}

static void back_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_CLICKED) {
        leave_wallet();
    }
}

static void message_dialog(ui_string_id_t message)
{
    lv_obj_t *dialog = lv_msgbox_create(NULL);
    lv_msgbox_add_title(dialog, ui_i18n_get(UI_STR_WALLET));
    lv_msgbox_add_text(dialog, ui_i18n_get(message));
    lv_msgbox_add_close_button(dialog);
    lv_obj_set_width(dialog, 390);
    lv_obj_set_style_bg_color(dialog, lv_color_hex(0x292A32), 0);
    lv_obj_set_style_text_color(dialog, lv_color_white(), 0);
    lv_obj_set_style_text_font(dialog, ui_i18n_font_24(), 0);
}

static void menu_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }

    wallet_action_t action = (wallet_action_t)(uintptr_t)lv_event_get_user_data(event);
    switch (action) {
    case WALLET_ACTION_INITIALIZE:
        s_internal_navigation = true;
        lv_async_call(show_setup_async, NULL);
        return;
    case WALLET_ACTION_CONNECT:
        s_internal_navigation = true;
        lv_async_call(show_unlock_async, NULL);
        return;
    case WALLET_ACTION_SESSION:
        s_internal_navigation = true;
        lv_async_call(show_receive_async, NULL);
        return;
    case WALLET_ACTION_SETTINGS:
        s_internal_navigation = true;
        lv_async_call(show_options_async, NULL);
        return;
    case WALLET_ACTION_SCAN_QR:
    case WALLET_ACTION_QR_MODE:
        message_dialog(UI_STR_WALLET_QR_REQUIRED);
        return;
    default:
        message_dialog(UI_STR_WALLET_UNAVAILABLE);
        return;
    }
}

static void add_menu_panel(lv_obj_t *parent, const jade_ui_menu_item_t *item)
{
    lv_obj_t *panel = lv_button_create(parent);
    lv_obj_set_size(panel, JADE_PANEL_WIDTH, JADE_PANEL_HEIGHT);
    lv_obj_add_flag(panel, LV_OBJ_FLAG_SNAPPABLE);
    lv_obj_set_style_bg_color(panel, lv_color_hex(item->color), 0);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x34363F), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_style_radius(panel, 6, 0);
    lv_obj_set_style_shadow_width(panel, 0, 0);
    lv_obj_add_event_cb(panel, menu_event_cb, LV_EVENT_CLICKED,
                        (void *)(uintptr_t)item->action);

    lv_obj_t *symbol = lv_label_create(panel);
    lv_label_set_text(symbol, item->symbol);
    lv_obj_set_style_text_color(symbol, lv_color_white(), 0);
    lv_obj_set_style_text_font(symbol, &lv_font_montserrat_48, 0);
    lv_obj_align(symbol, LV_ALIGN_TOP_LEFT, 12, 18);

    lv_obj_t *label = lv_label_create(panel);
    lv_label_set_text(label, ui_i18n_get(text_id(item->text)));
    lv_obj_set_width(label, JADE_PANEL_WIDTH - 40);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_set_style_text_font(label, ui_i18n_font_35(), 0);
    lv_obj_align(label, LV_ALIGN_BOTTOM_LEFT, 12, -22);
}

static void add_footer(lv_obj_t *screen, const jade_ui_home_model_t *model)
{
    lv_obj_t *dot = lv_obj_create(screen);
    lv_obj_remove_style_all(dot);
    lv_obj_set_size(dot, 14, 14);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(dot,
                              model->operational ? lv_color_hex(0x00A688)
                                                 : lv_color_hex(0x6B6E78),
                              0);
    lv_obj_align(dot, LV_ALIGN_BOTTOM_LEFT, 24, -36);

    lv_obj_t *state = lv_label_create(screen);
    lv_label_set_text(state, ui_i18n_get(state_text_id(model->state)));
    lv_obj_set_width(state, 270);
    lv_label_set_long_mode(state, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(state, lv_color_hex(0xD5D8DE), 0);
    lv_obj_set_style_text_font(state, ui_i18n_font_24(), 0);
    lv_obj_align(state, LV_ALIGN_BOTTOM_LEFT, 48, -29);

    lv_obj_t *version = lv_label_create(screen);
    lv_label_set_text(version, esp_app_get_description()->version);
    lv_obj_set_style_text_color(version, lv_color_hex(0x9EA3AD), 0);
    lv_obj_set_style_text_font(version, &lv_font_montserrat_24, 0);
    lv_obj_align(version, LV_ALIGN_BOTTOM_RIGHT, -20, -31);
}

void jade_wallet_ui_show(void)
{
    lv_obj_t *screen = lv_screen_active();
    lv_obj_clean(screen);
    s_internal_navigation = false;
    ui_nav_enable_right_swipe(screen);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x050607), 0);
    lv_obj_set_style_pad_all(screen, 0, 0);
    ui_status_bar_add(screen, JADE_UI_WIDTH);

    if (wallet_service_enter() != ESP_OK) {
        leave_wallet();
        return;
    }
    jade_usb_protocol_ui_init();

    wallet_status_t status = wallet_service_get_status();
    wallet_capabilities_t capabilities = wallet_service_get_capabilities();
    jade_ui_home_model_t model = jade_ui_build_home_model(status, capabilities);

    lv_obj_t *back = lv_button_create(screen);
    lv_obj_set_size(back, 108, JADE_HEADER_HEIGHT);
    lv_obj_align(back, LV_ALIGN_TOP_LEFT, 4, JADE_HEADER_TOP);
    lv_obj_set_style_bg_color(back, lv_color_hex(0x252830), 0);
    lv_obj_set_style_bg_opa(back, LV_OPA_50, 0);
    lv_obj_set_style_bg_opa(back, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(back, lv_color_hex(0x4B4E57), LV_STATE_PRESSED);
    lv_obj_set_style_radius(back, 8, 0);
    lv_obj_set_style_shadow_width(back, 0, 0);
    lv_obj_add_event_cb(back, back_event_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *back_icon = lv_label_create(back);
    lv_label_set_text(back_icon, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(back_icon, lv_color_white(), 0);
    lv_obj_set_style_text_font(back_icon, &lv_font_montserrat_48, 0);
    lv_obj_center(back_icon);

    lv_obj_t *title = lv_label_create(screen);
    lv_label_set_text(title, ui_i18n_get(UI_STR_WALLET));
    lv_obj_set_width(title, 336);
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_set_style_text_font(title, ui_i18n_font_28(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 124, 76);

    lv_obj_t *carousel = lv_obj_create(screen);
    lv_obj_add_event_cb(carousel, wallet_content_delete_cb, LV_EVENT_DELETE, NULL);
    lv_obj_remove_style_all(carousel);
    lv_obj_set_size(carousel, JADE_UI_WIDTH, JADE_PANEL_HEIGHT + 24);
    lv_obj_align(carousel, LV_ALIGN_TOP_LEFT, 0, 138);
    lv_obj_set_flex_flow(carousel, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(carousel, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_left(carousel, 24, 0);
    lv_obj_set_style_pad_right(carousel, JADE_UI_WIDTH - JADE_PANEL_WIDTH - 24, 0);
    lv_obj_set_style_pad_column(carousel, 14, 0);
    lv_obj_set_scroll_dir(carousel, LV_DIR_HOR);
    lv_obj_set_scroll_snap_x(carousel, LV_SCROLL_SNAP_START);
    lv_obj_set_scrollbar_mode(carousel, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(carousel, LV_OBJ_FLAG_GESTURE_BUBBLE);

    for (size_t i = 0; i < model.item_count; ++i) {
        add_menu_panel(carousel, &model.items[i]);
    }

    add_footer(screen, &model);
}

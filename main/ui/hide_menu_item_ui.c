#include "hide_menu_item_ui.h"

#include <stdint.h>

#include "esp_err.h"
#include "esp_log.h"
#include "lvgl.h"
#include "nvs.h"
#include "ui/i18n/ui_i18n.h"
#include "ui_nav.h"
#include "ui_status_bar.h"

#define HIDE_MENU_UI_WIDTH 480
#define HIDE_MENU_UI_HEIGHT 640
#define MENU_VISIBILITY_NVS_NAMESPACE "menu_ui"
#define MENU_VISIBILITY_NVS_KEY "visible"
#define MENU_VISIBILITY_NVS_KEY_V2 "visible_v2"
#define MENU_VISIBILITY_ALL_MASK ((1U << MAIN_MENU_ITEM_COUNT) - 1U)

static const char *TAG = "hide_menu_ui";
static uint32_t s_visible_mask = MENU_VISIBILITY_ALL_MASK;
static bool s_visible_mask_loaded;

static uint32_t load_visible_mask(void)
{
    if (s_visible_mask_loaded) {
        return s_visible_mask;
    }

    nvs_handle_t nvs;
    uint32_t mask = MENU_VISIBILITY_ALL_MASK;
    esp_err_t ret = nvs_open(MENU_VISIBILITY_NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (ret == ESP_OK) {
        ret = nvs_get_u32(nvs, MENU_VISIBILITY_NVS_KEY_V2, &mask);
        if (ret == ESP_ERR_NVS_NOT_FOUND) {
            ret = nvs_get_u32(nvs, MENU_VISIBILITY_NVS_KEY, &mask);
            if (ret == ESP_OK) {
                mask |= 1U << MAIN_MENU_ITEM_WALLET;
            }
        }
        nvs_close(nvs);
    }

    if (ret != ESP_OK && ret != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "Failed to load main menu visibility: %s", esp_err_to_name(ret));
        mask = MENU_VISIBILITY_ALL_MASK;
    }

    s_visible_mask = mask & MENU_VISIBILITY_ALL_MASK;
    s_visible_mask_loaded = true;
    return s_visible_mask;
}

static esp_err_t save_visible_mask(uint32_t mask)
{
    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(MENU_VISIBILITY_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = nvs_set_u32(nvs, MENU_VISIBILITY_NVS_KEY_V2,
                      mask & MENU_VISIBILITY_ALL_MASK);
    if (ret == ESP_OK) {
        ret = nvs_commit(nvs);
    }
    nvs_close(nvs);

    if (ret == ESP_OK) {
        s_visible_mask = mask & MENU_VISIBILITY_ALL_MASK;
        s_visible_mask_loaded = true;
    }
    return ret;
}

bool hide_menu_item_ui_is_visible(main_menu_item_id_t item_id)
{
    if (item_id < 0 || item_id >= MAIN_MENU_ITEM_COUNT) {
        return true;
    }
    return (load_visible_mask() & (1U << item_id)) != 0;
}

static void visibility_switch_changed(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_VALUE_CHANGED) {
        return;
    }

    lv_obj_t *sw = lv_event_get_target_obj(event);
    main_menu_item_id_t item_id =
        (main_menu_item_id_t)(uintptr_t)lv_event_get_user_data(event);
    uint32_t old_mask = load_visible_mask();
    uint32_t new_mask = old_mask;
    if (lv_obj_has_state(sw, LV_STATE_CHECKED)) {
        new_mask |= 1U << item_id;
    } else {
        new_mask &= ~(1U << item_id);
    }

    esp_err_t ret = save_visible_mask(new_mask);
    if (ret == ESP_OK) {
        return;
    }

    ESP_LOGW(TAG, "Failed to save menu item %d visibility: %s",
             (int)item_id, esp_err_to_name(ret));
    if ((old_mask & (1U << item_id)) != 0) {
        lv_obj_add_state(sw, LV_STATE_CHECKED);
    } else {
        lv_obj_remove_state(sw, LV_STATE_CHECKED);
    }
}

static void add_visibility_row(lv_obj_t *parent, ui_string_id_t text_id,
                               main_menu_item_id_t item_id)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, HIDE_MENU_UI_WIDTH - 36, 80);
    lv_obj_set_style_bg_color(row, lv_color_hex(0x292A32), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(row, 6, 0);
    lv_obj_set_style_pad_left(row, 18, 0);
    lv_obj_set_style_pad_right(row, 18, 0);
    lv_obj_set_style_pad_column(row, 14, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *label = lv_label_create(row);
    lv_label_set_text(label, ui_i18n_get(text_id));
    lv_obj_set_width(label, 0);
    lv_obj_set_flex_grow(label, 1);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_set_style_text_font(label, ui_i18n_font_32(), 0);
    lv_obj_set_style_text_letter_space(label, 0, 0);

    lv_obj_t *sw = lv_switch_create(row);
    lv_obj_set_size(sw, 82, 44);
    lv_obj_set_style_bg_color(sw, lv_color_hex(0x3A3C46), LV_PART_MAIN);
    lv_obj_set_style_bg_color(sw, lv_color_hex(0x2997E8),
                              LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(sw, lv_color_white(), LV_PART_KNOB);
    if (hide_menu_item_ui_is_visible(item_id)) {
        lv_obj_add_state(sw, LV_STATE_CHECKED);
    }
    lv_obj_add_event_cb(sw, visibility_switch_changed, LV_EVENT_VALUE_CHANGED,
                        (void *)(uintptr_t)item_id);
}

void hide_menu_item_ui_show(void)
{
    lv_obj_t *screen = lv_screen_active();
    lv_obj_clean(screen);
    ui_nav_enable_right_swipe(screen);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x11141A), 0);
    lv_obj_set_style_pad_all(screen, 0, 0);

    ui_status_bar_add(screen, HIDE_MENU_UI_WIDTH);

    lv_obj_t *content = lv_obj_create(screen);
    lv_obj_remove_style_all(content);
    lv_obj_set_size(content, HIDE_MENU_UI_WIDTH,
                    HIDE_MENU_UI_HEIGHT - UI_STATUS_BAR_HEIGHT);
    lv_obj_align(content, LV_ALIGN_TOP_MID, 0, UI_STATUS_BAR_HEIGHT);
    lv_obj_clear_flag(content, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(content);
    lv_label_set_text(title, ui_i18n_get(UI_STR_HIDE_MENU_ITEM));
    lv_obj_set_width(title, HIDE_MENU_UI_WIDTH - 36);
    lv_label_set_long_mode(title, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_set_style_text_font(title, ui_i18n_font_35(), 0);
    lv_obj_set_style_text_letter_space(title, 0, 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 18, 10);

    lv_obj_t *list = lv_obj_create(content);
    lv_obj_remove_style_all(list);
    lv_obj_set_size(list, HIDE_MENU_UI_WIDTH, 518);
    lv_obj_align(list, LV_ALIGN_TOP_MID, 0, 66);
    lv_obj_set_style_pad_left(list, 18, 0);
    lv_obj_set_style_pad_right(list, 18, 0);
    lv_obj_set_style_pad_top(list, 0, 0);
    lv_obj_set_style_pad_bottom(list, 12, 0);
    lv_obj_set_style_pad_row(list, 8, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(list, LV_OBJ_FLAG_SCROLL_CHAIN_HOR);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_width(list, 4, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_color(list, lv_color_hex(0x6B6E78), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(list, LV_OPA_50, LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(list, 2, LV_PART_SCROLLBAR);

    static const struct {
        ui_string_id_t label;
        main_menu_item_id_t id;
    } items[] = {
        {UI_STR_PASSWORD_MANAGER, MAIN_MENU_ITEM_PASSWORD_MANAGER},
        {UI_STR_SEARCH, MAIN_MENU_ITEM_SEARCH},
        {UI_STR_NEW_ENTRY, MAIN_MENU_ITEM_NEW_ENTRY},
        {UI_STR_FIDO, MAIN_MENU_ITEM_FIDO},
        {UI_STR_SMARTCARD, MAIN_MENU_ITEM_SMARTCARD},
        {UI_STR_CHAMELEON, MAIN_MENU_ITEM_CHAMELEON},
        {UI_STR_WALLET, MAIN_MENU_ITEM_WALLET},
        {UI_STR_SCRIPT, MAIN_MENU_ITEM_SCRIPT},
    };
    for (size_t i = 0; i < sizeof(items) / sizeof(items[0]); ++i) {
        add_visibility_row(list, items[i].label, items[i].id);
    }
}

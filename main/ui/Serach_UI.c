#include "Serach_UI.h"

#include <ctype.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "kdbx/kdbx_loader.h"
#include "lvgl.h"
#include "ui_nav.h"
#include "ui/i18n/ui_i18n.h"
#include "ui_password_detail.h"
#include "ui_password_manager.h"
#include "ui_status_bar.h"

#define SEARCH_UI_WIDTH            480
#define SEARCH_UI_HEIGHT           640
#define SEARCH_CONTENT_HEIGHT      (SEARCH_UI_HEIGHT - UI_STATUS_BAR_HEIGHT)
#define SEARCH_CONTENT_WIDTH       (SEARCH_UI_WIDTH - 32)
#define SEARCH_MAX_RESULTS         24
#define SEARCH_QUERY_Y             62
#define SEARCH_QUERY_HEIGHT        72
#define SEARCH_RESULTS_PANEL_Y     142
#define SEARCH_RESULTS_HIDDEN      (SEARCH_CONTENT_HEIGHT - SEARCH_RESULTS_PANEL_Y)
#define SEARCH_RESULTS_VISIBLE     162
#define SEARCH_KEYBOARD_HEIGHT     264
#define SEARCH_RESULT_ROW_HEIGHT   88
#define SEARCH_ENTRY_ICON_SCALE    154

LV_IMAGE_DECLARE(C00_Password);

typedef struct {
    size_t entry_index;
} search_result_item_t;

static lv_obj_t *s_query_textarea;
static lv_obj_t *s_results_panel;
static lv_obj_t *s_keyboard;
static lv_obj_t *s_result_count_label;
static search_result_item_t s_result_items[SEARCH_MAX_RESULTS];
static char s_pending_t9_key;
static uint8_t s_pending_t9_press_count;

static const char *s_t9_map[] = {
    "1",     "2 abc", "3 def", "\n",
    "4 ghi", "5 jkl", "6 mno", "\n",
    "7 pqrs", "8 tuv", "9 wxyz", "\n",
    LV_SYMBOL_BACKSPACE, "0", LV_SYMBOL_CLOSE, ""
};

static char lower_ascii(char ch)
{
    return (char)tolower((unsigned char)ch);
}

static bool title_matches_query(const char *title, const char *query)
{
    if (!query || query[0] == '\0') {
        return true;
    }

    const char *title_cursor = title;
    const char *query_cursor = query;
    while (*title_cursor && *query_cursor) {
        if (lower_ascii(*title_cursor) == lower_ascii(*query_cursor)) {
            query_cursor++;
        }
        title_cursor++;
    }

    if (*query_cursor == '\0') {
        return true;
    }

    size_t query_len = strlen(query);
    if (query_len == 0) {
        return true;
    }

    for (const char *start = title; *start; start++) {
        size_t i = 0;
        while (i < query_len && start[i] &&
               lower_ascii(start[i]) == lower_ascii(query[i])) {
            i++;
        }
        if (i == query_len) {
            return true;
        }
    }

    return false;
}

static const char *t9_chars_for_key(char key)
{
    switch (key) {
    case '1': return "1";
    case '2': return "2abc";
    case '3': return "3def";
    case '4': return "4ghi";
    case '5': return "5jkl";
    case '6': return "6mno";
    case '7': return "7pqrs";
    case '8': return "8tuv";
    case '9': return "9wxyz";
    case '0': return "0";
    default: return NULL;
    }
}

static char t9_char_for_press(char key, uint8_t press_count)
{
    const char *chars = t9_chars_for_key(key);
    if (!chars || press_count == 0) {
        return '\0';
    }

    size_t char_count = strlen(chars);
    return chars[(press_count - 1) % char_count];
}

static void reset_pending_t9_key(void)
{
    s_pending_t9_key = '\0';
    s_pending_t9_press_count = 0;
}

static void update_search_layout(bool keyboard_visible)
{
    if (s_results_panel && !lv_obj_is_valid(s_results_panel)) {
        s_results_panel = NULL;
    }
    if (s_keyboard && !lv_obj_is_valid(s_keyboard)) {
        s_keyboard = NULL;
    }
    if (s_query_textarea && !lv_obj_is_valid(s_query_textarea)) {
        s_query_textarea = NULL;
    }
    if (s_result_count_label && !lv_obj_is_valid(s_result_count_label)) {
        s_result_count_label = NULL;
    }

    if (s_results_panel) {
        lv_obj_set_height(s_results_panel,
                          keyboard_visible ? SEARCH_RESULTS_VISIBLE : SEARCH_RESULTS_HIDDEN);
    }

    if (s_keyboard) {
        if (keyboard_visible) {
            lv_obj_remove_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void show_t9_keyboard(void)
{
    update_search_layout(true);
}

static void hide_t9_keyboard(void)
{
    update_search_layout(false);
    reset_pending_t9_key();
}

static void query_textarea_event_cb(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_CLICKED || code == LV_EVENT_FOCUSED) {
        lv_obj_t *textarea = lv_event_get_target_obj(event);
        lv_obj_add_state(textarea, LV_STATE_FOCUSED);
        lv_textarea_set_cursor_pos(textarea, LV_TEXTAREA_CURSOR_LAST);
        show_t9_keyboard();
    }
}

static void hide_keyboard_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_CLICKED) {
        hide_t9_keyboard();
    }
}

static void add_t9_key_to_query(char key)
{
    if (s_pending_t9_key == key) {
        s_pending_t9_press_count++;
        lv_textarea_delete_char(s_query_textarea);
    } else {
        s_pending_t9_key = key;
        s_pending_t9_press_count = 1;
    }

    char text[2] = {t9_char_for_press(key, s_pending_t9_press_count), '\0'};
    if (text[0] != '\0') {
        lv_textarea_add_text(s_query_textarea, text);
    }
}

static void result_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }

    search_result_item_t *item = lv_event_get_user_data(event);
    if (item) {
        lv_obj_t *screen = lv_screen_active();
        if (screen) {
            lv_obj_remove_event_cb(screen, hide_keyboard_event_cb);
        }
        s_query_textarea = NULL;
        s_results_panel = NULL;
        s_keyboard = NULL;
        s_result_count_label = NULL;
        ui_password_detail_set_entry(item->entry_index);
        ui_nav_push(serach_ui_show, ui_password_detail_show);
    }
}

static void add_result_row(size_t entry_index, size_t result_index)
{
    const kdbx_entry_t *entry = kdbx_get_entry(entry_index);
    if (!entry || result_index >= SEARCH_MAX_RESULTS) {
        return;
    }

    s_result_items[result_index].entry_index = entry_index;

    lv_obj_t *row = lv_obj_create(s_results_panel);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, SEARCH_CONTENT_WIDTH, SEARCH_RESULT_ROW_HEIGHT);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(row, lv_color_hex(0x303139), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_radius(row, 4, LV_STATE_PRESSED);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_add_flag(row, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row, result_event_cb, LV_EVENT_CLICKED, &s_result_items[result_index]);

    lv_obj_t *icon_box = lv_obj_create(row);
    lv_obj_remove_style_all(icon_box);
    lv_obj_set_size(icon_box, 64, 64);
    lv_obj_align(icon_box, LV_ALIGN_LEFT_MID, 8, 0);
    lv_obj_set_style_bg_opa(icon_box, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(icon_box, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(icon_box, LV_OBJ_FLAG_GESTURE_BUBBLE);

    lv_obj_t *icon = lv_image_create(icon_box);
    lv_image_set_src(icon, &C00_Password);
    lv_image_set_scale(icon, SEARCH_ENTRY_ICON_SCALE);
    lv_obj_center(icon);

    lv_obj_t *title = lv_label_create(row);
    lv_label_set_text(title, entry->title);
    lv_obj_set_width(title, SEARCH_CONTENT_WIDTH - 96);
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_set_style_text_font(title, ui_i18n_font_28(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 84, 8);
    lv_obj_add_flag(title, LV_OBJ_FLAG_GESTURE_BUBBLE);

    const char *detail_text = entry->username[0] ? entry->username : entry->url;
    lv_obj_t *detail = lv_label_create(row);
    lv_label_set_text(detail, detail_text && detail_text[0] ? detail_text : " ");
    lv_obj_set_width(detail, SEARCH_CONTENT_WIDTH - 96);
    lv_label_set_long_mode(detail, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(detail, lv_color_hex(0xAEB1B9), 0);
    lv_obj_set_style_text_font(detail, ui_i18n_font_24(), 0);
    lv_obj_align(detail, LV_ALIGN_TOP_LEFT, 84, 48);
    lv_obj_add_flag(detail, LV_OBJ_FLAG_GESTURE_BUBBLE);
}

static void add_results_message(const char *text)
{
    lv_obj_t *message_row = lv_obj_create(s_results_panel);
    lv_obj_remove_style_all(message_row);
    lv_obj_set_size(message_row, SEARCH_CONTENT_WIDTH, 96);
    lv_obj_clear_flag(message_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(message_row, LV_OBJ_FLAG_GESTURE_BUBBLE);

    lv_obj_t *label = lv_label_create(message_row);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, lv_color_hex(0xAEB1B9), 0);
    lv_obj_set_style_text_font(label, ui_i18n_font_24(), 0);
    lv_obj_center(label);
    lv_obj_add_flag(label, LV_OBJ_FLAG_GESTURE_BUBBLE);
}

static void refresh_results(void)
{
    if (!s_results_panel || !s_query_textarea) {
        return;
    }

    lv_obj_clean(s_results_panel);

    if (ui_password_manager_is_loading()) {
        if (s_result_count_label) {
            lv_label_set_text(s_result_count_label, ui_i18n_get(UI_STR_LOADING_DATABASE));
        }
        add_results_message(ui_i18n_get(UI_STR_LOADING_ENTRIES));
        return;
    }

    const char *query = lv_textarea_get_text(s_query_textarea);
    size_t result_count = 0;
    size_t visible_count = 0;
    size_t entry_count = kdbx_get_entry_count();
    for (size_t i = 0; i < entry_count; i++) {
        const kdbx_entry_t *entry = kdbx_get_entry(i);
        if (entry && title_matches_query(entry->title, query)) {
            if (visible_count < SEARCH_MAX_RESULTS) {
                add_result_row(i, visible_count++);
            }
            result_count++;
        }
    }

    if (s_result_count_label) {
        if (result_count > SEARCH_MAX_RESULTS) {
            lv_label_set_text_fmt(s_result_count_label, ui_i18n_get(UI_STR_RESULTS_SHOWING),
                                  (unsigned int)result_count, SEARCH_MAX_RESULTS);
        } else {
            lv_label_set_text_fmt(s_result_count_label,
                                  ui_i18n_get(result_count == 1 ? UI_STR_RESULT : UI_STR_RESULTS),
                                  (unsigned int)result_count);
        }
    }

    if (result_count == 0) {
        add_results_message(ui_i18n_get(UI_STR_NO_RESULTS));
    }
}

static void keyboard_event_cb(lv_event_t *event)
{
    if (ui_password_manager_is_loading()) {
        return;
    }

    lv_event_code_t code = lv_event_get_code(event);
    lv_obj_t *keyboard = lv_event_get_target_obj(event);
    uint32_t button_id = lv_buttonmatrix_get_selected_button(keyboard);
    const char *key = lv_buttonmatrix_get_button_text(keyboard, button_id);
    if (!key || !s_query_textarea) {
        return;
    }

    if (code != LV_EVENT_VALUE_CHANGED) {
        return;
    }

    if (strcmp(key, LV_SYMBOL_BACKSPACE) == 0) {
        reset_pending_t9_key();
        lv_textarea_delete_char(s_query_textarea);
    } else if (strcmp(key, LV_SYMBOL_CLOSE) == 0) {
        reset_pending_t9_key();
        lv_textarea_set_text(s_query_textarea, "");
    } else {
        char digit = key[0];
        if (t9_chars_for_key(digit)) {
            add_t9_key_to_query(digit);
        }
    }

    refresh_results();
}

void serach_ui_show(void)
{
    reset_pending_t9_key();

    lv_obj_t *screen = lv_screen_active();
    if (screen) {
        lv_obj_remove_event_cb(screen, hide_keyboard_event_cb);
    }
    s_query_textarea = NULL;
    s_results_panel = NULL;
    s_keyboard = NULL;
    s_result_count_label = NULL;
    lv_obj_clean(screen);
    ui_nav_enable_right_swipe(screen);
    lv_obj_add_flag(screen, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(screen, hide_keyboard_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x0D1016), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(screen, 0, 0);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    ui_status_bar_add(screen, SEARCH_UI_WIDTH);

    lv_obj_t *content = lv_obj_create(screen);
    lv_obj_remove_style_all(content);
    lv_obj_set_size(content, SEARCH_UI_WIDTH, SEARCH_CONTENT_HEIGHT);
    lv_obj_align(content, LV_ALIGN_TOP_MID, 0, UI_STATUS_BAR_HEIGHT);
    lv_obj_set_style_bg_color(content, lv_color_hex(0x25262E), 0);
    lv_obj_set_style_bg_opa(content, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(content, 0, 0);
    lv_obj_clear_flag(content, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(content, LV_OBJ_FLAG_GESTURE_BUBBLE);

    lv_obj_t *title = lv_label_create(content);
    lv_label_set_text(title, ui_i18n_get(UI_STR_SEARCH));
    lv_obj_set_width(title, 180);
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_set_style_text_font(title, ui_i18n_font_35(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 16, 8);
    lv_obj_add_flag(title, LV_OBJ_FLAG_GESTURE_BUBBLE);

    s_result_count_label = lv_label_create(content);
    lv_label_set_text_fmt(s_result_count_label, ui_i18n_get(UI_STR_RESULTS), 0U);
    lv_obj_set_width(s_result_count_label, 252);
    lv_label_set_long_mode(s_result_count_label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(s_result_count_label, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_color(s_result_count_label, lv_color_hex(0xAEB1B9), 0);
    lv_obj_set_style_text_font(s_result_count_label, ui_i18n_font_24(), 0);
    lv_obj_align(s_result_count_label, LV_ALIGN_TOP_RIGHT, -16, 14);
    lv_obj_add_flag(s_result_count_label, LV_OBJ_FLAG_GESTURE_BUBBLE);

    if (ui_password_manager_is_loading()) {
        s_query_textarea = NULL;
        s_keyboard = NULL;

        s_results_panel = lv_obj_create(content);
        lv_obj_remove_style_all(s_results_panel);
        lv_obj_set_size(s_results_panel, SEARCH_UI_WIDTH, SEARCH_RESULTS_HIDDEN);
        lv_obj_align(s_results_panel, LV_ALIGN_TOP_MID, 0, SEARCH_RESULTS_PANEL_Y);
        lv_obj_set_style_bg_opa(s_results_panel, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(s_results_panel, 0, 0);
        lv_obj_set_style_pad_left(s_results_panel, 16, 0);
        lv_obj_set_style_pad_right(s_results_panel, 16, 0);
        lv_obj_clear_flag(s_results_panel, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(s_results_panel, LV_OBJ_FLAG_GESTURE_BUBBLE);

        lv_obj_t *loading = lv_label_create(s_results_panel);
        lv_label_set_text(loading, ui_i18n_get(UI_STR_LOADING_ENTRIES));
        lv_obj_set_style_text_color(loading, lv_color_hex(0xAEB1B9), 0);
        lv_obj_set_style_text_font(loading, ui_i18n_font_24(), 0);
        lv_obj_center(loading);
        lv_obj_add_flag(loading, LV_OBJ_FLAG_GESTURE_BUBBLE);
        lv_label_set_text(s_result_count_label, ui_i18n_get(UI_STR_LOADING_DATABASE));
        return;
    }

    s_query_textarea = lv_textarea_create(content);
    lv_obj_set_size(s_query_textarea, SEARCH_CONTENT_WIDTH, SEARCH_QUERY_HEIGHT);
    lv_obj_align(s_query_textarea, LV_ALIGN_TOP_MID, 0, SEARCH_QUERY_Y);
    lv_textarea_set_one_line(s_query_textarea, true);
    lv_textarea_set_max_length(s_query_textarea, 48);
    lv_textarea_set_placeholder_text(s_query_textarea, ui_i18n_get(UI_STR_T9_SEARCH));
    lv_textarea_set_cursor_click_pos(s_query_textarea, true);
    lv_obj_add_event_cb(s_query_textarea, query_textarea_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(s_query_textarea, query_textarea_event_cb, LV_EVENT_FOCUSED, NULL);
    lv_obj_set_style_bg_color(s_query_textarea, lv_color_hex(0x303139), 0);
    lv_obj_set_style_border_color(s_query_textarea, lv_color_hex(0x303139), 0);
    lv_obj_set_style_border_color(s_query_textarea, lv_color_hex(0x2997E8), LV_STATE_FOCUSED);
    lv_obj_set_style_border_width(s_query_textarea, 1, 0);
    lv_obj_set_style_border_width(s_query_textarea, 2, LV_STATE_FOCUSED);
    lv_obj_set_style_radius(s_query_textarea, 4, 0);
    lv_obj_set_style_text_color(s_query_textarea, lv_color_white(), 0);
    lv_obj_set_style_text_font(s_query_textarea, ui_i18n_font_28(), 0);
    lv_obj_set_style_text_color(s_query_textarea, lv_color_hex(0x676C78),
                                LV_PART_TEXTAREA_PLACEHOLDER);
    lv_obj_set_style_text_font(s_query_textarea, ui_i18n_font_24(),
                               LV_PART_TEXTAREA_PLACEHOLDER);
    lv_obj_set_style_bg_color(s_query_textarea, lv_color_white(),
                              LV_PART_CURSOR | LV_STATE_FOCUSED);
    lv_obj_set_style_bg_opa(s_query_textarea, LV_OPA_COVER,
                            LV_PART_CURSOR | LV_STATE_FOCUSED);
    lv_obj_set_style_width(s_query_textarea, 2, LV_PART_CURSOR | LV_STATE_FOCUSED);
    lv_obj_set_style_anim_duration(s_query_textarea, 500,
                                   LV_PART_CURSOR | LV_STATE_FOCUSED);
    lv_obj_set_style_pad_left(s_query_textarea, 16, 0);
    lv_obj_set_style_pad_right(s_query_textarea, 14, 0);
    lv_obj_set_style_pad_top(s_query_textarea, 13, 0);
    lv_obj_set_style_pad_bottom(s_query_textarea, 13, 0);
    lv_obj_add_flag(s_query_textarea, LV_OBJ_FLAG_GESTURE_BUBBLE);

    s_results_panel = lv_obj_create(content);
    lv_obj_remove_style_all(s_results_panel);
    lv_obj_set_size(s_results_panel, SEARCH_UI_WIDTH, SEARCH_RESULTS_HIDDEN);
    lv_obj_align(s_results_panel, LV_ALIGN_TOP_MID, 0, SEARCH_RESULTS_PANEL_Y);
    lv_obj_set_style_bg_opa(s_results_panel, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_results_panel, 0, 0);
    lv_obj_set_style_pad_left(s_results_panel, 16, 0);
    lv_obj_set_style_pad_right(s_results_panel, 16, 0);
    lv_obj_set_style_pad_top(s_results_panel, 4, 0);
    lv_obj_set_style_pad_bottom(s_results_panel, 12, 0);
    lv_obj_set_flex_flow(s_results_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_results_panel, 0, 0);
    lv_obj_add_flag(s_results_panel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_results_panel, hide_keyboard_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(s_results_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_results_panel, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_results_panel, LV_SCROLLBAR_MODE_OFF);

    s_keyboard = lv_buttonmatrix_create(screen);
    lv_buttonmatrix_set_map(s_keyboard, s_t9_map);
    lv_obj_set_size(s_keyboard, SEARCH_UI_WIDTH - 16, SEARCH_KEYBOARD_HEIGHT);
    lv_obj_align(s_keyboard, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_obj_add_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_bg_opa(s_keyboard, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_keyboard, 0, 0);
    lv_obj_set_style_pad_all(s_keyboard, 2, 0);
    lv_obj_set_style_pad_row(s_keyboard, 6, 0);
    lv_obj_set_style_pad_column(s_keyboard, 6, 0);
    lv_obj_set_style_radius(s_keyboard, 4, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(s_keyboard, lv_color_hex(0x303139), LV_PART_ITEMS);
    lv_obj_set_style_bg_color(s_keyboard, lv_color_hex(0x41424A),
                              LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_border_width(s_keyboard, 0, LV_PART_ITEMS);
    lv_obj_set_style_text_color(s_keyboard, lv_color_white(), LV_PART_ITEMS);
    lv_obj_set_style_text_font(s_keyboard, &lv_font_montserrat_24, LV_PART_ITEMS);
    lv_obj_add_event_cb(s_keyboard, keyboard_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

    update_search_layout(false);
    refresh_results();
}

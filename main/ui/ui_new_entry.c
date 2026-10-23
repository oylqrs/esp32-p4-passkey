#include "ui_new_entry.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_random.h"
#include "lvgl.h"
#include "usb_msc.h"
#include "ui_nav.h"
#include "ui/i18n/ui_i18n.h"
#include "ui_status_bar.h"

#define NEW_ENTRY_WIDTH            480
#define NEW_ENTRY_PANEL_WIDTH      (NEW_ENTRY_WIDTH - 32)
#define NEW_ENTRY_FIELD_COUNT      5
#define NEW_ENTRY_FIELD_ROW_HEIGHT 76
#define NEW_ENTRY_FIELD_LABEL_X    16
#define NEW_ENTRY_FIELD_GAP        8
#define NEW_ENTRY_FIELD_MIN_WIDTH  220
#define NEW_ENTRY_KEYBOARD_HEIGHT  260
#define NEW_ENTRY_EDITOR_HEIGHT    478
#define NEW_ENTRY_SETTINGS_HEIGHT  456
#define NEW_ENTRY_TEMPORARY_HEIGHT 304
#define NEW_ENTRY_VIEW_HEIGHT      (640 - UI_STATUS_BAR_HEIGHT)
#define PASSWORD_MIN_LENGTH        8
#define PASSWORD_MAX_LENGTH        32
#define TEMPORARY_ENTRY_MAX_COUNT  20
#define TEMPORARY_CSV_PATH         "/data/temp.csv"
#define TEMPORARY_CSV_LINE_SIZE    1024

typedef struct {
    char title[129];
    char account[129];
    char password[129];
    char website[129];
    char notes[257];
} temporary_entry_t;

static const char *TAG = "ui_new_entry";
static lv_obj_t *s_fields[NEW_ENTRY_FIELD_COUNT];
static lv_obj_t *s_option_switches[4];
static lv_obj_t *s_length_slider;
static lv_obj_t *s_length_label;
static lv_obj_t *s_temporary_list;
static lv_obj_t *s_content;
static lv_obj_t *s_keyboard;
static lv_obj_t *s_active_field;
static char s_pending_t9_key;
static uint8_t s_pending_t9_press_count;
static temporary_entry_t s_temporary_entries[TEMPORARY_ENTRY_MAX_COUNT];
static size_t s_temporary_entry_count;
static size_t s_selected_temporary_entry;
static bool s_temporary_entries_loaded;

static const ui_string_id_t s_field_names[NEW_ENTRY_FIELD_COUNT] = {
    UI_STR_FIELD_TITLE,
    UI_STR_ACCOUNT,
    UI_STR_PASSWORD,
    UI_STR_WEBSITE,
    UI_STR_NOTES,
};

static const char *s_t9_map[] = {
    "1 .,@",  "2 abc", "3 def",  "\n",
    "4 ghi",  "5 jkl", "6 mno",  "\n",
    "7 pqrs", "8 tuv", "9 wxyz", "\n",
    LV_SYMBOL_BACKSPACE, "0 space", LV_SYMBOL_CLOSE, ""
};

static const char *t9_chars_for_key(char key)
{
    switch (key) {
    case '1': return "1.,@-_:/?";
    case '2': return "2abc";
    case '3': return "3def";
    case '4': return "4ghi";
    case '5': return "5jkl";
    case '6': return "6mno";
    case '7': return "7pqrs";
    case '8': return "8tuv";
    case '9': return "9wxyz";
    case '0': return "0 ";
    default: return NULL;
    }
}

static void reset_pending_t9_key(void)
{
    s_pending_t9_key = '\0';
    s_pending_t9_press_count = 0;
}

static void hide_t9_keyboard(void)
{
    reset_pending_t9_key();
    s_active_field = NULL;
    lv_obj_add_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
}

static void style_panel(lv_obj_t *panel)
{
    lv_obj_remove_style_all(panel);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x27282F), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(panel, 4, 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_style_pad_all(panel, 0, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
}

static lv_obj_t *add_section_label(lv_obj_t *parent, const char *text, int32_t y)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, lv_color_hex(0x9B9DA5), 0);
    lv_obj_set_style_text_font(label, ui_i18n_font_35(), 0);
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, 24, y);
    return label;
}

static void field_event_cb(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);
    if (code != LV_EVENT_CLICKED && code != LV_EVENT_FOCUSED) {
        return;
    }

    lv_obj_t *field = lv_event_get_target_obj(event);
    lv_obj_add_state(field, LV_STATE_FOCUSED);
    lv_textarea_set_cursor_pos(field, LV_TEXTAREA_CURSOR_LAST);

    if (s_keyboard) {
        if (s_active_field != field) {
            reset_pending_t9_key();
        }
        s_active_field = field;
        lv_obj_remove_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_to_index(s_keyboard, -1);
    }

    for (size_t i = 0; i < NEW_ENTRY_FIELD_COUNT; i++) {
        if (s_fields[i] == field) {
            int32_t scroll_y = i > 2 ? (int32_t)(i - 2) * NEW_ENTRY_FIELD_ROW_HEIGHT : 0;
            lv_obj_scroll_to_y(s_content, scroll_y, LV_ANIM_ON);
            break;
        }
    }
}

static void keyboard_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_VALUE_CHANGED || !s_active_field) {
        return;
    }

    uint32_t button_id = lv_buttonmatrix_get_selected_button(s_keyboard);
    const char *key = lv_buttonmatrix_get_button_text(s_keyboard, button_id);
    if (!key) {
        return;
    }

    if (strcmp(key, LV_SYMBOL_BACKSPACE) == 0) {
        reset_pending_t9_key();
        lv_textarea_delete_char(s_active_field);
    } else if (strcmp(key, LV_SYMBOL_CLOSE) == 0) {
        hide_t9_keyboard();
    } else {
        char digit = key[0];
        const char *chars = t9_chars_for_key(digit);
        if (!chars) {
            return;
        }

        if (s_pending_t9_key == digit) {
            s_pending_t9_press_count++;
            lv_textarea_delete_char(s_active_field);
        } else {
            s_pending_t9_key = digit;
            s_pending_t9_press_count = 1;
        }

        size_t char_count = strlen(chars);
        char text[2] = {
            chars[(s_pending_t9_press_count - 1) % char_count],
            '\0'
        };
        lv_textarea_add_text(s_active_field, text);
    }
}

static void add_editor_field(lv_obj_t *parent, size_t index)
{
    int32_t row_y = 8 + (int32_t)index * NEW_ENTRY_FIELD_ROW_HEIGHT;

    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, ui_i18n_get(s_field_names[index]));
    lv_obj_set_style_text_color(label, lv_color_hex(0xE3E4E8), 0);
    lv_obj_set_style_text_font(label, ui_i18n_font_35(), 0);
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, NEW_ENTRY_FIELD_LABEL_X, row_y + 17);
    lv_obj_update_layout(label);

    int32_t input_x = NEW_ENTRY_FIELD_LABEL_X + lv_obj_get_width(label) + NEW_ENTRY_FIELD_GAP;
    int32_t input_width = NEW_ENTRY_PANEL_WIDTH - input_x - NEW_ENTRY_FIELD_LABEL_X;
    if (input_width < NEW_ENTRY_FIELD_MIN_WIDTH) {
        input_width = NEW_ENTRY_FIELD_MIN_WIDTH;
        input_x = NEW_ENTRY_PANEL_WIDTH - NEW_ENTRY_FIELD_LABEL_X - input_width;
        lv_obj_set_width(label, input_x - NEW_ENTRY_FIELD_LABEL_X - NEW_ENTRY_FIELD_GAP);
        lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
    }

    lv_obj_t *field = lv_textarea_create(parent);
    lv_obj_remove_style_all(field);
    lv_obj_set_size(field, input_width, 68);
    lv_obj_align(field, LV_ALIGN_TOP_LEFT, input_x, row_y + 2);
    lv_textarea_set_one_line(field, true);
    lv_textarea_set_max_length(field, index == 4 ? 256 : 128);
    lv_textarea_set_placeholder_text(field, "");
    lv_obj_set_style_bg_opa(field, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(field, lv_color_hex(0x1D1F24), LV_STATE_FOCUSED);
    lv_obj_set_style_bg_opa(field, LV_OPA_COVER, LV_STATE_FOCUSED);
    lv_obj_set_style_border_color(field, lv_color_hex(0x454750), 0);
    lv_obj_set_style_border_color(field, lv_color_hex(0x2997E8), LV_STATE_FOCUSED);
    lv_obj_set_style_border_width(field, 0, 0);
    lv_obj_set_style_border_width(field, 1, LV_STATE_FOCUSED);
    lv_obj_set_style_radius(field, 4, 0);
    lv_obj_set_style_text_color(field, lv_color_hex(0xF1F1F3), 0);
    lv_obj_set_style_text_font(field, ui_i18n_font_35(), 0);
    lv_obj_set_style_text_color(field, lv_color_hex(0xECECEF), LV_PART_TEXTAREA_PLACEHOLDER);
    lv_obj_set_style_bg_color(field, lv_color_white(), LV_PART_CURSOR | LV_STATE_FOCUSED);
    lv_obj_set_style_bg_opa(field, LV_OPA_COVER, LV_PART_CURSOR | LV_STATE_FOCUSED);
    lv_obj_set_style_width(field, 2, LV_PART_CURSOR | LV_STATE_FOCUSED);
    lv_obj_set_style_pad_left(field, 10, 0);
    lv_obj_set_style_pad_right(field, 10, 0);
    lv_obj_set_style_pad_top(field, 12, 0);
    lv_obj_set_style_pad_bottom(field, 8, 0);
    lv_obj_add_event_cb(field, field_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(field, field_event_cb, LV_EVENT_FOCUSED, NULL);

    s_fields[index] = field;
}

static void add_t9_keyboard(lv_obj_t *screen)
{
    s_keyboard = lv_buttonmatrix_create(screen);
    lv_buttonmatrix_set_map(s_keyboard, s_t9_map);
    lv_obj_set_size(s_keyboard, NEW_ENTRY_WIDTH, NEW_ENTRY_KEYBOARD_HEIGHT);
    lv_obj_align(s_keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(s_keyboard, lv_color_hex(0x17191F), 0);
    lv_obj_set_style_bg_opa(s_keyboard, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_keyboard, 0, 0);
    lv_obj_set_style_radius(s_keyboard, 0, 0);
    lv_obj_set_style_pad_all(s_keyboard, 4, 0);
    lv_obj_set_style_pad_row(s_keyboard, 5, 0);
    lv_obj_set_style_pad_column(s_keyboard, 5, 0);
    lv_obj_set_style_radius(s_keyboard, 5, LV_PART_ITEMS);
    lv_obj_set_style_text_font(s_keyboard, &lv_font_montserrat_24, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(s_keyboard, lv_color_hex(0x30333B), LV_PART_ITEMS);
    lv_obj_set_style_bg_color(s_keyboard, lv_color_hex(0x2997E8),
                              LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_text_color(s_keyboard, lv_color_white(), LV_PART_ITEMS);
    lv_obj_add_event_cb(s_keyboard, keyboard_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
}

static lv_obj_t *add_small_button(lv_obj_t *parent, const char *text, int32_t width)
{
    lv_obj_t *button = lv_button_create(parent);
    lv_obj_set_size(button, width, 56);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x2997E8), 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x197FC7), LV_STATE_PRESSED);
    lv_obj_set_style_radius(button, 7, 0);
    lv_obj_set_style_shadow_width(button, 0, 0);
    lv_obj_set_style_pad_all(button, 0, 0);

    lv_obj_t *label = lv_label_create(button);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_set_style_text_font(label, ui_i18n_font_35(), 0);
    lv_obj_center(label);
    return button;
}

static lv_obj_t *add_option_row(lv_obj_t *parent, const char *text, int32_t y, bool enabled)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, lv_color_hex(0xE3E4E8), 0);
    lv_obj_set_style_text_font(label, ui_i18n_font_35(), 0);
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, 16, y + 8);

    lv_obj_t *sw = lv_switch_create(parent);
    lv_obj_set_size(sw, 88, 44);
    lv_obj_align(sw, LV_ALIGN_TOP_RIGHT, -16, y);
    lv_obj_set_style_bg_color(sw, lv_color_hex(0x34373C), LV_PART_MAIN);
    lv_obj_set_style_bg_color(sw, lv_color_hex(0x249CEB), LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(sw, lv_color_white(), LV_PART_KNOB);
    if (enabled) {
        lv_obj_add_state(sw, LV_STATE_CHECKED);
    }
    return sw;
}

static void update_length_label(void)
{
    char text[8];
    snprintf(text, sizeof(text), "%ld", (long)lv_slider_get_value(s_length_slider));
    lv_label_set_text(s_length_label, text);
}

static void length_changed_cb(lv_event_t *event)
{
    (void)event;
    update_length_label();
}

static void generate_password_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }

    static const char *sets[] = {
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ",
        "abcdefghijklmnopqrstuvwxyz",
        "0123456789",
        "!@#$%^&*_-+=?",
    };
    char allowed[96] = "";
    for (size_t i = 0; i < 4; i++) {
        if (lv_obj_has_state(s_option_switches[i], LV_STATE_CHECKED)) {
            strlcat(allowed, sets[i], sizeof(allowed));
        }
    }

    if (allowed[0] == '\0') {
        lv_obj_add_state(s_option_switches[1], LV_STATE_CHECKED);
        strlcpy(allowed, sets[1], sizeof(allowed));
    }

    char password[PASSWORD_MAX_LENGTH + 1];
    size_t allowed_len = strlen(allowed);
    size_t password_len = (size_t)lv_slider_get_value(s_length_slider);
    for (size_t i = 0; i < password_len; i++) {
        password[i] = allowed[esp_random() % allowed_len];
    }
    password[password_len] = '\0';
    lv_textarea_set_text(s_fields[2], password);
}

static bool write_csv_field(FILE *file, const char *value)
{
    if (fputc('"', file) == EOF) {
        return false;
    }
    for (const char *cursor = value; *cursor; cursor++) {
        if (*cursor == '"' && fputc('"', file) == EOF) {
            return false;
        }
        if (fputc(*cursor, file) == EOF) {
            return false;
        }
    }
    return fputc('"', file) != EOF;
}

static esp_err_t sync_temporary_entries_to_csv(void)
{
    esp_err_t ret = usb_hid_mount_usbstore_app();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount USB storage for %s: %s",
                 TEMPORARY_CSV_PATH, esp_err_to_name(ret));
        return ret;
    }

    FILE *file = fopen(TEMPORARY_CSV_PATH, "wb");
    if (!file) {
        ESP_LOGE(TAG, "Failed to open %s for writing", TEMPORARY_CSV_PATH);
        return ESP_FAIL;
    }

    bool ok = fputs("Title,Username,Password,URL,Notes\r\n", file) >= 0;
    for (size_t i = 0; ok && i < s_temporary_entry_count; i++) {
        const temporary_entry_t *entry = &s_temporary_entries[i];
        const char *fields[] = {
            entry->title,
            entry->account,
            entry->password,
            entry->website,
            entry->notes,
        };
        for (size_t field = 0; ok && field < sizeof(fields) / sizeof(fields[0]); field++) {
            ok = write_csv_field(file, fields[field]);
            if (ok) {
                ok = fputc(field + 1 == sizeof(fields) / sizeof(fields[0]) ? '\n' : ',', file) != EOF;
            }
        }
    }

    if (ok) {
        ok = fflush(file) == 0;
    }
    if (fclose(file) != 0) {
        ok = false;
    }
    if (!ok) {
        ESP_LOGE(TAG, "Failed while writing %s", TEMPORARY_CSV_PATH);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Saved %u temporary entries to %s",
             (unsigned)s_temporary_entry_count, TEMPORARY_CSV_PATH);
    return ESP_OK;
}

static bool parse_csv_line(const char *line, temporary_entry_t *entry)
{
    char *outputs[] = {
        entry->title,
        entry->account,
        entry->password,
        entry->website,
        entry->notes,
    };
    const size_t capacities[] = {
        sizeof(entry->title),
        sizeof(entry->account),
        sizeof(entry->password),
        sizeof(entry->website),
        sizeof(entry->notes),
    };
    const char *cursor = line;

    memset(entry, 0, sizeof(*entry));
    for (size_t field = 0; field < sizeof(outputs) / sizeof(outputs[0]); field++) {
        bool quoted = *cursor == '"';
        if (quoted) {
            cursor++;
        }

        size_t length = 0;
        while (*cursor && *cursor != '\r' && *cursor != '\n') {
            if (quoted && *cursor == '"') {
                if (cursor[1] == '"') {
                    cursor++;
                } else {
                    cursor++;
                    break;
                }
            } else if (!quoted && *cursor == ',') {
                break;
            }

            if (length + 1 < capacities[field]) {
                outputs[field][length++] = *cursor;
            }
            cursor++;
        }
        outputs[field][length] = '\0';

        if (field + 1 < sizeof(outputs) / sizeof(outputs[0])) {
            if (*cursor != ',') {
                return false;
            }
            cursor++;
        }
    }
    return entry->title[0] != '\0';
}

static void load_temporary_entries(void)
{
    if (s_temporary_entries_loaded) {
        return;
    }

    esp_err_t ret = usb_hid_mount_usbstore_app();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Temporary CSV unavailable: %s", esp_err_to_name(ret));
        return;
    }

    FILE *file = fopen(TEMPORARY_CSV_PATH, "rb");
    if (!file) {
        s_temporary_entries_loaded = true;
        return;
    }

    char line[TEMPORARY_CSV_LINE_SIZE];
    bool first_line = true;
    s_temporary_entry_count = 0;
    while (s_temporary_entry_count < TEMPORARY_ENTRY_MAX_COUNT &&
           fgets(line, sizeof(line), file)) {
        if (first_line && strncmp(line, "Title,Username,Password,URL,Notes", 33) == 0) {
            first_line = false;
            continue;
        }
        first_line = false;
        temporary_entry_t entry;
        if (parse_csv_line(line, &entry)) {
            s_temporary_entries[s_temporary_entry_count++] = entry;
        }
    }
    fclose(file);
    s_temporary_entries_loaded = true;
    ESP_LOGI(TAG, "Loaded %u temporary entries from %s",
             (unsigned)s_temporary_entry_count, TEMPORARY_CSV_PATH);
}

static void temporary_detail_show(void);

static void temporary_item_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }

    size_t index = (size_t)(uintptr_t)lv_event_get_user_data(event);
    if (index >= s_temporary_entry_count) {
        return;
    }
    s_selected_temporary_entry = index;
    ui_nav_push(ui_new_entry_show, temporary_detail_show);
}

static void refresh_temporary_list(void)
{
    if (!s_temporary_list || !lv_obj_is_valid(s_temporary_list)) {
        return;
    }

    lv_obj_clean(s_temporary_list);
    if (s_temporary_entry_count == 0) {
        lv_obj_t *empty = lv_label_create(s_temporary_list);
        lv_label_set_text(empty, ui_i18n_get(UI_STR_NO_TEMPORARY_ENTRIES));
        lv_obj_set_width(empty, NEW_ENTRY_PANEL_WIDTH - 40);
        lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(empty, lv_color_hex(0x8D919C), 0);
        lv_obj_set_style_text_font(empty, ui_i18n_font_35(), 0);
        return;
    }

    for (size_t i = 0; i < s_temporary_entry_count; i++) {
        lv_obj_t *row = lv_obj_create(s_temporary_list);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, LV_PCT(100), 94);
        lv_obj_set_style_bg_color(row, lv_color_hex(0x1F2027), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(row, lv_color_hex(0x34363E), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_STATE_PRESSED);
        lv_obj_set_style_radius(row, 4, 0);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(row, temporary_item_event_cb, LV_EVENT_CLICKED,
                            (void *)(uintptr_t)i);

        lv_obj_t *icon = lv_label_create(row);
        lv_label_set_text(icon, LV_SYMBOL_FILE);
        lv_obj_set_style_text_color(icon, lv_color_hex(0xFFAE18), 0);
        lv_obj_set_style_text_font(icon, &lv_font_montserrat_24, 0);
        lv_obj_align(icon, LV_ALIGN_LEFT_MID, 16, 0);

        lv_obj_t *title = lv_label_create(row);
        lv_label_set_text(title, s_temporary_entries[i].title);
        lv_obj_set_width(title, NEW_ENTRY_PANEL_WIDTH - 116);
        lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_color(title, lv_color_hex(0xECECEF), 0);
        lv_obj_set_style_text_font(title, ui_i18n_font_35(), 0);
        lv_obj_align(title, LV_ALIGN_LEFT_MID, 58, 0);

    }
}

static void temporary_save_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }

    const char *title = lv_textarea_get_text(s_fields[0]);
    if (!title || title[0] == '\0') {
        lv_textarea_set_placeholder_text(s_fields[0], ui_i18n_get(UI_STR_TITLE_REQUIRED));
        lv_obj_add_state(s_fields[0], LV_STATE_FOCUSED);
        return;
    }

    if (s_temporary_entry_count == TEMPORARY_ENTRY_MAX_COUNT) {
        memmove(&s_temporary_entries[0], &s_temporary_entries[1],
                sizeof(s_temporary_entries[0]) * (TEMPORARY_ENTRY_MAX_COUNT - 1));
        s_temporary_entry_count--;
    }

    temporary_entry_t *entry = &s_temporary_entries[s_temporary_entry_count++];
    strlcpy(entry->title, title, sizeof(entry->title));
    strlcpy(entry->account, lv_textarea_get_text(s_fields[1]), sizeof(entry->account));
    strlcpy(entry->password, lv_textarea_get_text(s_fields[2]), sizeof(entry->password));
    strlcpy(entry->website, lv_textarea_get_text(s_fields[3]), sizeof(entry->website));
    strlcpy(entry->notes, lv_textarea_get_text(s_fields[4]), sizeof(entry->notes));
    s_temporary_entries_loaded = true;

    refresh_temporary_list();
    esp_err_t ret = sync_temporary_entries_to_csv();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Temporary entry kept in memory but CSV sync failed: %s",
                 esp_err_to_name(ret));
    }

    for (size_t i = 0; i < NEW_ENTRY_FIELD_COUNT; i++) {
        lv_textarea_set_text(s_fields[i], "");
    }
    hide_t9_keyboard();
    ESP_LOGI(TAG, "Temporary entry saved: title=%s account=%s website=%s",
             entry->title, entry->account, entry->website);
}

static void add_temporary_detail_field(lv_obj_t *parent,
                                       const char *label_text,
                                       const char *value_text)
{
    const int32_t label_width = 132;
    const int32_t value_width = NEW_ENTRY_PANEL_WIDTH - label_width - 44;
    const char *display_value = value_text && value_text[0] ? value_text : "-";
    const lv_font_t *font = ui_i18n_font_35();
    lv_point_t value_size;
    lv_text_get_size(&value_size, display_value, font, 0, 2,
                     value_width, LV_TEXT_FLAG_NONE);
    int32_t row_height = LV_MAX(96, value_size.y + 24);

    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), row_height);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *label = lv_label_create(row);
    lv_label_set_text(label, label_text);
    lv_obj_set_width(label, label_width);
    lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_color(label, lv_color_hex(0xB6B9C2), 0);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, 16, 0);

    lv_obj_t *value = lv_label_create(row);
    lv_label_set_text(value, display_value);
    lv_obj_set_width(value, value_width);
    lv_obj_set_height(value, LV_SIZE_CONTENT);
    lv_label_set_long_mode(value, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(value, lv_color_hex(0xF4F4F6), 0);
    lv_obj_set_style_text_font(value, font, 0);
    lv_obj_set_style_text_line_space(value, 2, 0);
    lv_obj_set_style_text_align(value,
                                value_size.y > font->line_height
                                    ? LV_TEXT_ALIGN_CENTER
                                    : LV_TEXT_ALIGN_LEFT,
                                0);
    lv_obj_align(value, LV_ALIGN_LEFT_MID, label_width + 24, 0);

    lv_obj_t *divider = lv_obj_create(row);
    lv_obj_remove_style_all(divider);
    lv_obj_set_size(divider, NEW_ENTRY_PANEL_WIDTH - 32, 1);
    lv_obj_set_style_bg_color(divider, lv_color_hex(0x3A3C46), 0);
    lv_obj_set_style_bg_opa(divider, LV_OPA_COVER, 0);
    lv_obj_align(divider, LV_ALIGN_BOTTOM_MID, 0, 0);
}

static void temporary_detail_show(void)
{
    lv_obj_t *screen = lv_screen_active();
    lv_obj_clean(screen);
    ui_nav_enable_right_swipe(screen);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x11141A), 0);
    lv_obj_set_style_pad_all(screen, 0, 0);

    ui_status_bar_add(screen, NEW_ENTRY_WIDTH);

    lv_obj_t *title = lv_label_create(screen);
    lv_label_set_text(title, ui_i18n_get(UI_STR_TEMPORARY_ENTRY));
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_set_style_text_font(title, ui_i18n_font_35(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 16, 64);

    lv_obj_t *panel = lv_obj_create(screen);
    style_panel(panel);
    lv_obj_set_size(panel, NEW_ENTRY_PANEL_WIDTH, 510);
    lv_obj_align(panel, LV_ALIGN_TOP_MID, 0, 116);
    lv_obj_set_style_pad_left(panel, 0, 0);
    lv_obj_set_style_pad_right(panel, 0, 0);
    lv_obj_set_style_pad_top(panel, 8, 0);
    lv_obj_set_style_pad_bottom(panel, 8, 0);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(panel, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(panel, LV_SCROLLBAR_MODE_AUTO);

    if (s_selected_temporary_entry >= s_temporary_entry_count) {
        add_temporary_detail_field(panel,
                                   ui_i18n_get(UI_STR_ERROR),
                                   ui_i18n_get(UI_STR_ENTRY_NOT_FOUND));
        return;
    }

    const temporary_entry_t *entry = &s_temporary_entries[s_selected_temporary_entry];
    add_temporary_detail_field(panel, ui_i18n_get(UI_STR_FIELD_TITLE), entry->title);
    add_temporary_detail_field(panel, ui_i18n_get(UI_STR_ACCOUNT), entry->account);
    add_temporary_detail_field(panel, ui_i18n_get(UI_STR_PASSWORD), entry->password);
    add_temporary_detail_field(panel, ui_i18n_get(UI_STR_WEBSITE), entry->website);
    add_temporary_detail_field(panel, ui_i18n_get(UI_STR_NOTES), entry->notes);
}

void ui_new_entry_show(void)
{
    memset(s_fields, 0, sizeof(s_fields));
    memset(s_option_switches, 0, sizeof(s_option_switches));
    s_content = NULL;
    s_keyboard = NULL;
    s_active_field = NULL;
    s_temporary_list = NULL;
    reset_pending_t9_key();
    load_temporary_entries();

    lv_obj_t *screen = lv_screen_active();
    lv_obj_clean(screen);
    ui_nav_enable_right_swipe(screen);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x11141A), 0);
    lv_obj_set_style_pad_all(screen, 0, 0);

    ui_status_bar_add(screen, NEW_ENTRY_WIDTH);

    lv_obj_t *content = lv_obj_create(screen);
    s_content = content;
    lv_obj_remove_style_all(content);
    lv_obj_set_size(content, NEW_ENTRY_WIDTH, NEW_ENTRY_VIEW_HEIGHT);
    lv_obj_align(content, LV_ALIGN_TOP_MID, 0, UI_STATUS_BAR_HEIGHT);
    lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(content, 0, 0);
    lv_obj_set_style_pad_bottom(content, 20, 0);
    lv_obj_set_scroll_dir(content, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(content, LV_SCROLLBAR_MODE_AUTO);

    lv_obj_t *title = lv_label_create(content);
    lv_label_set_text(title, ui_i18n_get(UI_STR_NEW_ENTRY_TITLE));
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_set_style_text_font(title, ui_i18n_font_35(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 16, 12);

    lv_obj_t *editor = lv_obj_create(content);
    style_panel(editor);
    lv_obj_set_size(editor, NEW_ENTRY_PANEL_WIDTH, NEW_ENTRY_EDITOR_HEIGHT);
    lv_obj_align(editor, LV_ALIGN_TOP_MID, 0, 58);
    for (size_t i = 0; i < NEW_ENTRY_FIELD_COUNT; i++) {
        add_editor_field(editor, i);
    }

    lv_obj_t *save_divider = lv_obj_create(editor);
    lv_obj_remove_style_all(save_divider);
    lv_obj_set_size(save_divider, NEW_ENTRY_PANEL_WIDTH - 16, 1);
    lv_obj_set_style_bg_color(save_divider, lv_color_hex(0x35363D), 0);
    lv_obj_set_style_bg_opa(save_divider, LV_OPA_COVER, 0);
    lv_obj_align(save_divider, LV_ALIGN_BOTTOM_MID, 0, -82);

    lv_obj_t *save_text = lv_label_create(editor);
    lv_label_set_text(save_text, ui_i18n_get(UI_STR_TEMPORARY_ENTRY));
    lv_obj_set_style_text_color(save_text, lv_color_hex(0xECECEF), 0);
    lv_obj_set_style_text_font(save_text, ui_i18n_font_35(), 0);
    lv_obj_align(save_text, LV_ALIGN_BOTTOM_LEFT, 16, -24);

    lv_obj_t *save_button = add_small_button(editor, ui_i18n_get(UI_STR_SAVE), 108);
    lv_obj_align(save_button, LV_ALIGN_BOTTOM_RIGHT, -16, -12);
    lv_obj_add_event_cb(save_button, temporary_save_cb, LV_EVENT_CLICKED, NULL);

    add_section_label(content, ui_i18n_get(UI_STR_PASSWORD_SETTINGS), 546);

    lv_obj_t *settings = lv_obj_create(content);
    style_panel(settings);
    lv_obj_set_size(settings, NEW_ENTRY_PANEL_WIDTH, NEW_ENTRY_SETTINGS_HEIGHT);
    lv_obj_align(settings, LV_ALIGN_TOP_MID, 0, 590);

    lv_obj_t *random_label = lv_label_create(settings);
    lv_label_set_text(random_label, ui_i18n_get(UI_STR_RANDOM_PASSWORD));
    lv_obj_set_width(random_label, 250);
    lv_label_set_long_mode(random_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(random_label, lv_color_hex(0xE3E4E8), 0);
    lv_obj_set_style_text_font(random_label, ui_i18n_font_35(), 0);
    lv_obj_align(random_label, LV_ALIGN_TOP_LEFT, 16, 18);

    lv_obj_t *generate = add_small_button(settings, ui_i18n_get(UI_STR_GENERATE), 150);
    lv_obj_align(generate, LV_ALIGN_TOP_RIGHT, -16, 9);
    lv_obj_add_event_cb(generate, generate_password_cb, LV_EVENT_CLICKED, NULL);

    s_option_switches[0] = add_option_row(settings, ui_i18n_get(UI_STR_UPPERCASE_LETTERS), 118, true);
    s_option_switches[1] = add_option_row(settings, ui_i18n_get(UI_STR_LOWERCASE_LETTERS), 186, true);
    s_option_switches[2] = add_option_row(settings, ui_i18n_get(UI_STR_NUMBERS), 254, true);
    s_option_switches[3] = add_option_row(settings, ui_i18n_get(UI_STR_SPECIAL_CHARACTERS), 322, false);

    lv_obj_t *length_title = lv_label_create(settings);
    lv_label_set_text(length_title, ui_i18n_get(UI_STR_LENGTH));
    lv_obj_set_style_text_color(length_title, lv_color_hex(0x8D919C), 0);
    lv_obj_set_style_text_font(length_title, ui_i18n_font_35(), 0);
    lv_obj_align(length_title, LV_ALIGN_BOTTOM_LEFT, 16, -34);

    s_length_slider = lv_slider_create(settings);
    lv_obj_set_size(s_length_slider, 215, 20);
    lv_obj_align(s_length_slider, LV_ALIGN_BOTTOM_LEFT, 145, -33);
    lv_slider_set_range(s_length_slider, PASSWORD_MIN_LENGTH, PASSWORD_MAX_LENGTH);
    lv_slider_set_value(s_length_slider, 16, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_length_slider, lv_color_hex(0x17415A), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_length_slider, lv_color_hex(0x249CEB), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_length_slider, lv_color_hex(0x249CEB), LV_PART_KNOB);
    lv_obj_set_style_width(s_length_slider, 38, LV_PART_KNOB);
    lv_obj_set_style_height(s_length_slider, 38, LV_PART_KNOB);
    lv_obj_add_event_cb(s_length_slider, length_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);

    s_length_label = lv_label_create(settings);
    lv_obj_set_width(s_length_label, 64);
    lv_obj_set_style_text_align(s_length_label, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_color(s_length_label, lv_color_hex(0xD8DAE0), 0);
    lv_obj_set_style_text_font(s_length_label, ui_i18n_font_35(), 0);
    lv_obj_align(s_length_label, LV_ALIGN_BOTTOM_RIGHT, -10, -22);
    update_length_label();

    add_section_label(content, ui_i18n_get(UI_STR_TEMPORARY_LIST), 1056);

    lv_obj_t *temporary = lv_obj_create(content);
    style_panel(temporary);
    s_temporary_list = temporary;
    lv_obj_set_size(temporary, NEW_ENTRY_PANEL_WIDTH, NEW_ENTRY_TEMPORARY_HEIGHT);
    lv_obj_align(temporary, LV_ALIGN_TOP_MID, 0, 1100);
    lv_obj_set_style_pad_all(temporary, 16, 0);
    lv_obj_set_style_pad_row(temporary, 12, 0);
    lv_obj_set_flex_flow(temporary, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(temporary, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(temporary, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(temporary, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_bg_color(temporary, lv_color_hex(0x5B5E68), LV_PART_SCROLLBAR);
    lv_obj_set_style_width(temporary, 4, LV_PART_SCROLLBAR);
    refresh_temporary_list();

    add_t9_keyboard(screen);

}

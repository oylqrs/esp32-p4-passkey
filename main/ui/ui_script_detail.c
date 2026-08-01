#include "ui_script_detail.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_err.h"
#include "esp_log.h"
#include "lvgl.h"
#include "ui_nav.h"
#include "usb_msc.h"

#define SCRIPT_UI_WIDTH 480
#define SCRIPT_DIR "/data/script"
#define SCRIPT_MAX_ITEMS 64

static const char *TAG = "ui_script";

static void add_message(lv_obj_t *parent, const char *text, lv_color_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_width(label, SCRIPT_UI_WIDTH - 48);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(label, color, 0);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_24, 0);
    lv_obj_set_style_pad_top(label, 14, 0);
    lv_obj_set_style_pad_bottom(label, 14, 0);
}

static void add_script_item(lv_obj_t *parent, const struct dirent *entry)
{
    char path[512];
    struct stat file_stat = {0};
    bool is_dir = entry->d_type == DT_DIR;

    snprintf(path, sizeof(path), "%s/%s", SCRIPT_DIR, entry->d_name);
    if (!is_dir && stat(path, &file_stat) != 0) {
        ESP_LOGW(TAG, "stat failed for %s", path);
    }

    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, SCRIPT_UI_WIDTH - 48, 72);
    lv_obj_set_style_bg_color(row, lv_color_hex(0x292A32), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(row, lv_color_hex(0x3A3C46), 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_radius(row, 5, 0);
    lv_obj_set_style_pad_left(row, 16, 0);
    lv_obj_set_style_pad_right(row, 16, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *icon = lv_label_create(row);
    lv_label_set_text(icon, is_dir ? LV_SYMBOL_DIRECTORY : LV_SYMBOL_FILE);
    lv_obj_set_style_text_color(icon, is_dir ? lv_color_hex(0xFFC958) : lv_color_hex(0xF27A7A), 0);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_28, 0);
    lv_obj_set_width(icon, 42);

    lv_obj_t *name = lv_label_create(row);
    lv_label_set_text(name, entry->d_name);
    lv_obj_set_width(name, SCRIPT_UI_WIDTH - 180);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(name, lv_color_white(), 0);
    lv_obj_set_style_text_font(name, &lv_font_montserrat_24, 0);

    lv_obj_t *size = lv_label_create(row);
    if (is_dir) {
        lv_label_set_text(size, "DIR");
    } else {
        lv_label_set_text_fmt(size, "%ld B", (long)file_stat.st_size);
    }
    lv_obj_set_width(size, 78);
    lv_obj_set_style_text_align(size, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_color(size, lv_color_hex(0x9EA3AD), 0);
    lv_obj_set_style_text_font(size, &lv_font_montserrat_16, 0);
}

static void load_script_directory(lv_obj_t *list)
{
    esp_err_t mount_ret = usb_hid_mount_usbstore_app();
    if (mount_ret != ESP_OK) {
        ESP_LOGW(TAG, "mount usbstore to APP failed: %s", esp_err_to_name(mount_ret));
        add_message(list, "USB storage is not available", lv_color_hex(0xF27A7A));
        return;
    }

    DIR *dir = opendir(SCRIPT_DIR);
    if (!dir) {
        ESP_LOGW(TAG, "script directory not found: %s", SCRIPT_DIR);
        add_message(list, "script folder not found", lv_color_hex(0xFFC958));
        return;
    }

    size_t count = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && count < SCRIPT_MAX_ITEMS) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        add_script_item(list, entry);
        count++;
    }
    closedir(dir);

    if (count == 0) {
        add_message(list, "script folder is empty", lv_color_hex(0x9EA3AD));
    } else {
        ESP_LOGI(TAG, "loaded %u script directory entries", (unsigned)count);
    }
}

void ui_script_detail_show(void)
{
    lv_obj_t *screen = lv_screen_active();
    lv_obj_clean(screen);
    ui_nav_enable_right_swipe(screen);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x11141A), 0);
    lv_obj_set_style_pad_all(screen, 0, 0);

    lv_obj_t *title = lv_label_create(screen);
    lv_label_set_text(title, "Script");
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 24, 24);

    lv_obj_t *list = lv_obj_create(screen);
    lv_obj_remove_style_all(list);
    lv_obj_set_size(list, SCRIPT_UI_WIDTH - 48, 540);
    lv_obj_align(list, LV_ALIGN_TOP_MID, 0, 82);
    lv_obj_set_style_pad_all(list, 0, 0);
    lv_obj_set_style_pad_row(list, 10, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, LV_PART_SCROLLBAR);

    load_script_directory(list);
}

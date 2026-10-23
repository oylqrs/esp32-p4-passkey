#include "ui_script_detail.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_err.h"
#include "esp_log.h"
#include "lvgl.h"
#include "ui_nav.h"
#include "ui/i18n/ui_i18n.h"
#include "ui_status_bar.h"
#include "usb_msc.h"
#include "usb_script/usb_script.h"

#define SCRIPT_UI_WIDTH 480
#define SCRIPT_UI_HEIGHT 640
#define SCRIPT_LIST_MARGIN 24
#define SCRIPT_LIST_TOP 118
#define SCRIPT_LIST_BOTTOM 20
#define SCRIPT_ITEM_HEIGHT 120
#define SCRIPT_DIR "/data/script"
#define SCRIPT_MAX_ITEMS 64

static const char *TAG = "ui_script";

LV_IMAGE_DECLARE(ascii_icon);

typedef struct {
    char path[512];
} script_item_data_t;

#if 0
typedef struct {
    lv_obj_t *status_value;
    lv_obj_t *step_value;
    lv_timer_t *timer;
} script_status_ui_t;

static void script_status_timer_cb(lv_timer_t *timer)
{
    script_status_ui_t *ui = lv_timer_get_user_data(timer);
    usb_script_progress_t progress;
    usb_script_get_progress(&progress);

    const char *status_text = ui_i18n_get(UI_STR_IDLE);
    lv_color_t status_color = lv_color_hex(0x9EA3AD);
    if (progress.status == USB_SCRIPT_STATUS_RUNNING) {
        status_text = ui_i18n_get(UI_STR_RUNNING);
        status_color = lv_color_hex(0xFFC958);
    } else if (progress.status == USB_SCRIPT_STATUS_COMPLETED) {
        status_text = ui_i18n_get(UI_STR_COMPLETED);
        status_color = lv_color_hex(0x6FD08C);
    } else if (progress.status == USB_SCRIPT_STATUS_FAILED) {
        status_text = ui_i18n_get(UI_STR_FAILED);
        status_color = lv_color_hex(0xF27A7A);
    }

    lv_label_set_text(ui->status_value, status_text);
    lv_obj_set_style_text_color(ui->status_value, status_color, 0);
    lv_label_set_text_fmt(ui->step_value, ui_i18n_get(UI_STR_STEP_PROGRESS),
                          (unsigned)progress.step, (unsigned)progress.total);
}

static void script_status_delete_cb(lv_event_t *event)
{
    script_status_ui_t *ui = lv_event_get_user_data(event);
    if (ui->timer) lv_timer_delete(ui->timer);
    lv_free(ui);
}

static void add_script_status(lv_obj_t *screen)
{
    lv_obj_t *panel = lv_obj_create(screen);
    lv_obj_remove_style_all(panel);
    lv_obj_set_size(panel, SCRIPT_UI_WIDTH - 48, 166);
    lv_obj_align(panel, LV_ALIGN_TOP_MID, 0, 448);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x20232B), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(0x3A3C46), 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_radius(panel, 6, 0);
    lv_obj_set_style_pad_all(panel, 16, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(panel);
    lv_label_set_text(title, ui_i18n_get(UI_STR_STATUS));
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_set_style_text_font(title, ui_i18n_font_35(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);

    script_status_ui_t *ui = lv_malloc(sizeof(*ui));
    if (!ui) {
        lv_obj_delete(panel);
        return;
    }

    ui->status_value = lv_label_create(panel);
    lv_obj_set_style_text_font(ui->status_value, ui_i18n_font_35(), 0);
    lv_obj_align(ui->status_value, LV_ALIGN_TOP_LEFT, 0, 44);

    ui->step_value = lv_label_create(panel);
    lv_obj_set_style_text_color(ui->step_value, lv_color_hex(0xD5D8DE), 0);
    lv_obj_set_style_text_font(ui->step_value, ui_i18n_font_35(), 0);
    lv_obj_align(ui->step_value, LV_ALIGN_TOP_LEFT, 0, 88);

    ui->timer = lv_timer_create(script_status_timer_cb, 100, ui);
    lv_obj_add_event_cb(panel, script_status_delete_cb, LV_EVENT_DELETE, ui);
    script_status_timer_cb(ui->timer);
}
#endif

static void script_item_event_cb(lv_event_t *event)
{
    script_item_data_t *item = lv_event_get_user_data(event);

    if (lv_event_get_code(event) == LV_EVENT_DELETE) {
        lv_free(item);
        return;
    }
    if (lv_event_get_code(event) != LV_EVENT_CLICKED || !item) {
        return;
    }

    esp_err_t ret = usb_script_send_file(item->path);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to send %s through HID: %s", item->path, esp_err_to_name(ret));
    }
}

static void add_message(lv_obj_t *parent, const char *text, lv_color_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_width(label, SCRIPT_UI_WIDTH - (SCRIPT_LIST_MARGIN * 2));
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(label, color, 0);
    lv_obj_set_style_text_font(label, ui_i18n_font_35(), 0);
    lv_obj_set_style_pad_top(label, 14, 0);
    lv_obj_set_style_pad_bottom(label, 14, 0);
}

static void add_script_item(lv_obj_t *parent, const struct dirent *entry)
{
    char path[512];
    bool is_dir = entry->d_type == DT_DIR;

    snprintf(path, sizeof(path), "%s/%s", SCRIPT_DIR, entry->d_name);

    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, SCRIPT_UI_WIDTH - (SCRIPT_LIST_MARGIN * 2), SCRIPT_ITEM_HEIGHT);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_set_style_bg_color(row, lv_color_hex(0x292A32), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(row, lv_color_hex(0x353740), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_pad_left(row, 16, 0);
    lv_obj_set_style_pad_right(row, 16, 0);
    lv_obj_set_style_pad_column(row, 14, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    if (!is_dir) {
        script_item_data_t *item = lv_malloc(sizeof(*item));
        if (item) {
            strlcpy(item->path, path, sizeof(item->path));
            lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(row, script_item_event_cb, LV_EVENT_ALL, item);
        } else {
            ESP_LOGW(TAG, "Failed to allocate click data for %s", path);
        }
    }

    lv_obj_t *icon_box = lv_obj_create(row);
    lv_obj_remove_style_all(icon_box);
    lv_obj_set_size(icon_box, 75, 75);
    lv_obj_clear_flag(icon_box, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *icon = lv_image_create(icon_box);
    lv_image_set_src(icon, &ascii_icon);
    lv_image_set_scale(icon, 75 * LV_SCALE_NONE / 48);
    lv_obj_center(icon);

    lv_obj_t *name = lv_label_create(row);
    lv_label_set_text(name, entry->d_name);
    lv_obj_set_width(name, 0);
    lv_obj_set_flex_grow(name, 1);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(name, lv_color_white(), 0);
    lv_obj_set_style_text_font(name, ui_i18n_font_35(), 0);
}

static void load_script_directory(lv_obj_t *list)
{
    esp_err_t mount_ret = usb_hid_mount_usbstore_app();
    if (mount_ret != ESP_OK) {
        ESP_LOGW(TAG, "mount usbstore to APP failed: %s", esp_err_to_name(mount_ret));
        add_message(list, ui_i18n_get(UI_STR_USB_STORAGE_UNAVAILABLE), lv_color_hex(0xF27A7A));
        return;
    }

    DIR *dir = opendir(SCRIPT_DIR);
    if (!dir && errno == ENOENT) {
        if (mkdir(SCRIPT_DIR, 0775) == 0 || errno == EEXIST) {
            ESP_LOGI(TAG, "created script directory: %s", SCRIPT_DIR);
            dir = opendir(SCRIPT_DIR);
        } else {
            ESP_LOGW(TAG, "failed to create script directory %s: errno=%d",
                     SCRIPT_DIR, errno);
        }
    }
    if (!dir) {
        ESP_LOGW(TAG, "failed to open script directory %s: errno=%d",
                 SCRIPT_DIR, errno);
        add_message(list, ui_i18n_get(UI_STR_SCRIPT_FOLDER_NOT_FOUND), lv_color_hex(0xFFC958));
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
        add_message(list, ui_i18n_get(UI_STR_SCRIPT_FOLDER_EMPTY), lv_color_hex(0x9EA3AD));
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

    ui_status_bar_add(screen, SCRIPT_UI_WIDTH);

    lv_obj_t *title = lv_label_create(screen);
    lv_label_set_text(title, ui_i18n_get(UI_STR_SCRIPT));
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_set_style_text_font(title, ui_i18n_font_35(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 24, 64);

    lv_obj_t *list = lv_obj_create(screen);
    lv_obj_remove_style_all(list);
    lv_obj_set_size(list,
                    SCRIPT_UI_WIDTH - (SCRIPT_LIST_MARGIN * 2),
                    SCRIPT_UI_HEIGHT - SCRIPT_LIST_TOP - SCRIPT_LIST_BOTTOM);
    lv_obj_align(list, LV_ALIGN_TOP_MID, 0, SCRIPT_LIST_TOP);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(list, 0, 0);
    lv_obj_set_style_pad_row(list, 0, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(list, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_width(list, 4, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_color(list, lv_color_hex(0x6B6E78), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(list, LV_OPA_50, LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(list, 2, LV_PART_SCROLLBAR);

    load_script_directory(list);
#if 0
    add_script_status(screen);
#endif
}

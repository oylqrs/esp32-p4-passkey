#include "ui_password_manager.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "kdbx/kdbx_loader.h"
#include "lvgl.h"
#include "ui_nav.h"
#include "ui_password_detail.h"
#include "usb_hid_keyboard.h"

#define PASSWORD_MANAGER_WIDTH  480
#define KDBX_LOAD_TASK_STACK    24576
#define KDBX_LOAD_TASK_PRIORITY (tskIDLE_PRIORITY + 1)
#define PASSWORD_UI_MAX_ENTRIES 24

static const char *TAG = "ui_password";
static lv_obj_t *s_status_label;
static bool s_kdbx_load_in_progress;
static bool s_kdbx_auto_load_attempted;
static bool s_password_manager_visible;
static QueueHandle_t s_ui_cmd_queue;
static lv_timer_t *s_ui_cmd_timer;

typedef enum {
    PASSWORD_UI_CMD_LOAD_FINISHED,
} password_ui_cmd_t;

static void kdbx_load_finished_ui(void);
static void password_ui_cmd_timer_cb(lv_timer_t *timer);
static bool post_password_ui_cmd(password_ui_cmd_t cmd);
static void start_kdbx_load(bool update_status);

static void set_status_text(const char *text)
{
    if (s_status_label) {
        lv_label_set_text(s_status_label, text);
    }
}

bool ui_password_manager_is_loading(void)
{
    return s_kdbx_load_in_progress;
}

static void kdbx_load_task(void *arg)
{
    (void)arg;

    esp_err_t ret = usb_hid_mount_usbstore_app();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "usbstore is not mounted to APP: %s", esp_err_to_name(ret));
        s_kdbx_load_in_progress = false;
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGW(TAG, "usbstore is mounted to APP");
    ret = kdbx_load_first_database_from_data();
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "KDBX parsed");
    } else if (ret == ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGI(TAG, "KDBX parser reached current implementation limit");
    } else {
        ESP_LOGW(TAG, "Failed to parse KDBX: %s", esp_err_to_name(ret));
    }

    s_kdbx_load_in_progress = false;
    post_password_ui_cmd(PASSWORD_UI_CMD_LOAD_FINISHED);
    vTaskDelete(NULL);
}

static void start_kdbx_load(bool update_status)
{
    if (s_kdbx_load_in_progress) {
        if (update_status) {
            set_status_text("KDBX parsing...");
        }
        return;
    }

    if (update_status) {
        set_status_text("KDBX parsing...");
    }
    s_kdbx_auto_load_attempted = true;
    s_kdbx_load_in_progress = true;
    if (xTaskCreate(kdbx_load_task, "kdbx_load", KDBX_LOAD_TASK_STACK, NULL, KDBX_LOAD_TASK_PRIORITY, NULL) != pdPASS) {
        s_kdbx_load_in_progress = false;
        if (update_status) {
            set_status_text("KDBX task failed");
        }
        ESP_LOGW(TAG, "Failed to create KDBX load task");
    }
}

static void load_kdbx_from_data(void)
{
    start_kdbx_load(true);
}

void ui_password_manager_start_background_load(void)
{
    start_kdbx_load(false);
}

typedef enum {
    PASSWORD_ITEM_ACTION_TYPE,
    PASSWORD_ITEM_ACTION_DUMP_STORAGE,
    PASSWORD_ITEM_ACTION_LOAD_KDBX,
    PASSWORD_ITEM_ACTION_SHOW_DETAIL,
} password_item_action_t;

typedef struct {
    const char *symbol;
    const char *text;
    const char *hid_text;
    password_item_action_t action;
    size_t entry_index;
    lv_color_t color;
} password_item_t;

static password_item_t s_entry_items[PASSWORD_UI_MAX_ENTRIES];

static bool post_password_ui_cmd(password_ui_cmd_t cmd)
{
    if (!s_ui_cmd_queue) {
        s_ui_cmd_queue = xQueueCreate(4, sizeof(cmd));
        if (!s_ui_cmd_queue) {
            ESP_LOGW(TAG, "Failed to create password UI command queue");
            return false;
        }
    }

    if (xQueueSend(s_ui_cmd_queue, &cmd, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Password UI command queue full");
        return false;
    }

    return true;
}

static void password_ui_cmd_timer_cb(lv_timer_t *timer)
{
    (void)timer;

    if (!s_ui_cmd_queue) {
        return;
    }

    password_ui_cmd_t cmd;
    while (xQueueReceive(s_ui_cmd_queue, &cmd, 0) == pdTRUE) {
        if (cmd == PASSWORD_UI_CMD_LOAD_FINISHED) {
            kdbx_load_finished_ui();
        }
    }
}

static void ensure_password_ui_cmd_timer(void)
{
    if (!s_ui_cmd_queue) {
        s_ui_cmd_queue = xQueueCreate(4, sizeof(password_ui_cmd_t));
        if (!s_ui_cmd_queue) {
            ESP_LOGW(TAG, "Failed to create password UI command queue");
            return;
        }
    }

    if (!s_ui_cmd_timer) {
        s_ui_cmd_timer = lv_timer_create(password_ui_cmd_timer_cb, 20, NULL);
    }
}

static void kdbx_load_finished_ui(void)
{
    if (s_password_manager_visible) {
        ui_password_manager_show();
    }
}

static void add_kdbx_loading_popup(lv_obj_t *screen)
{
    lv_obj_t *overlay = lv_obj_create(screen);
    lv_obj_remove_style_all(overlay);
    lv_obj_set_size(overlay, PASSWORD_MANAGER_WIDTH, LV_PCT(100));
    lv_obj_align(overlay, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(overlay, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_50, 0);
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *dialog = lv_obj_create(overlay);
    lv_obj_remove_style_all(dialog);
    lv_obj_set_size(dialog, PASSWORD_MANAGER_WIDTH - 96, 120);
    lv_obj_align(dialog, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(dialog, lv_color_hex(0x292A32), 0);
    lv_obj_set_style_bg_opa(dialog, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(dialog, 6, 0);
    lv_obj_set_style_border_color(dialog, lv_color_hex(0x3A3C46), 0);
    lv_obj_set_style_border_width(dialog, 1, 0);
    lv_obj_clear_flag(dialog, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *label = lv_label_create(dialog);
    lv_label_set_text(label, "KDBX Loading");
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_28, 0);
    lv_obj_center(label);
}

static void add_status_bar(lv_obj_t *screen)
{
    lv_obj_t *status = lv_obj_create(screen);
    lv_obj_remove_style_all(status);
    lv_obj_set_size(status, PASSWORD_MANAGER_WIDTH, 28);
    lv_obj_align(status, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(status, lv_color_hex(0x0F1117), 0);
    lv_obj_set_style_bg_opa(status, LV_OPA_COVER, 0);

    lv_obj_t *time_label = lv_label_create(status);
    lv_label_set_text(time_label, "07-25 12:05");
    lv_obj_set_style_text_color(time_label, lv_color_hex(0x9EA3AD), 0);
    lv_obj_set_style_text_font(time_label, &lv_font_montserrat_16, 0);
    lv_obj_align(time_label, LV_ALIGN_TOP_RIGHT, -34, 3);

    lv_obj_t *battery = lv_label_create(status);
    lv_label_set_text(battery, LV_SYMBOL_BATTERY_FULL);
    lv_obj_set_style_text_color(battery, lv_color_hex(0x9EA3AD), 0);
    lv_obj_set_style_text_font(battery, &lv_font_montserrat_16, 0);
    lv_obj_align(battery, LV_ALIGN_TOP_RIGHT, -8, 3);
}

static void password_item_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }

    const password_item_t *item = lv_event_get_user_data(event);
    if (!item) {
        return;
    }

    if (item->action == PASSWORD_ITEM_ACTION_LOAD_KDBX || item->action == PASSWORD_ITEM_ACTION_DUMP_STORAGE) {
        if (item->action == PASSWORD_ITEM_ACTION_LOAD_KDBX) {
            load_kdbx_from_data();
            return;
        }

        set_status_text("Mounting /data...");
        esp_err_t ret = usb_hid_mount_usbstore_app();
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "usbstore is not mounted to APP: %s", esp_err_to_name(ret));
            set_status_text("Mount /data failed");
            return;
        }

        ret = usb_hid_dump_storage_files();
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to start usbstore dump: %s", esp_err_to_name(ret));
            set_status_text("Dump failed");
        } else {
            set_status_text("Dump started");
        }
        return;
    }

    if (item->action == PASSWORD_ITEM_ACTION_SHOW_DETAIL) {
        s_password_manager_visible = false;
        ui_password_detail_open(item->entry_index);
        return;
    }

    if (!item->hid_text) {
        return;
    }

    usb_hid_keyboard_type_text(item->hid_text);
}

static void add_password_item(lv_obj_t *parent, const password_item_t *item)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, PASSWORD_MANAGER_WIDTH - 80, 120);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row, password_item_event_cb, LV_EVENT_CLICKED, (void *)item);

    lv_obj_t *icon_box = lv_obj_create(row);
    lv_obj_remove_style_all(icon_box);
    lv_obj_set_size(icon_box, 120, 120);
    lv_obj_align(icon_box, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_bg_opa(icon_box, LV_OPA_TRANSP, 0);

    lv_obj_t *icon = lv_label_create(icon_box);
    lv_label_set_text(icon, item->symbol);
    lv_obj_set_style_text_color(icon, item->color, 0);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_28, 0);
    lv_obj_center(icon);

    lv_obj_t *label = lv_label_create(row);
    lv_label_set_text(label, item->text);
    lv_obj_set_width(label, PASSWORD_MANAGER_WIDTH - 230);
    lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_28, 0);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, 140, 0);
}

void ui_password_manager_show(void)
{
    ensure_password_ui_cmd_timer();
    s_password_manager_visible = true;

    lv_obj_t *screen = lv_screen_active();
    lv_obj_clean(screen);
    ui_nav_enable_right_swipe(screen);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x11141A), 0);
    lv_obj_set_style_pad_all(screen, 0, 0);

    add_status_bar(screen);

    lv_obj_t *title = lv_label_create(screen);
    lv_label_set_text(title, "Password Manager");
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 24, 46);

    s_status_label = lv_label_create(screen);
    char status_text[48];
    size_t entry_count = kdbx_get_entry_count();
    if (entry_count > 0) {
        snprintf(status_text, sizeof(status_text), "%u entries", (unsigned int)entry_count);
        lv_label_set_long_mode(s_status_label, LV_LABEL_LONG_CLIP);
        lv_label_set_text(s_status_label, status_text);
    } else if (s_kdbx_load_in_progress) {
        snprintf(status_text, sizeof(status_text), "KDBX parsing...");
        lv_label_set_long_mode(s_status_label, LV_LABEL_LONG_CLIP);
        lv_label_set_text(s_status_label, status_text);
    } else {
        snprintf(status_text, sizeof(status_text), "Ready");
        lv_label_set_long_mode(s_status_label, LV_LABEL_LONG_CLIP);
        lv_label_set_text(s_status_label, status_text);
    }
    lv_obj_set_width(s_status_label, PASSWORD_MANAGER_WIDTH - 48);
    lv_obj_set_style_text_color(s_status_label, lv_color_hex(0x9EA3AD), 0);
    lv_obj_set_style_text_font(s_status_label, &lv_font_montserrat_16, 0);
    lv_obj_align(s_status_label, LV_ALIGN_TOP_LEFT, 24, 72);

    lv_obj_t *panel = lv_obj_create(screen);
    lv_obj_remove_style_all(panel);
    lv_obj_set_size(panel, PASSWORD_MANAGER_WIDTH - 48, 524);
    lv_obj_align(panel, LV_ALIGN_TOP_MID, 0, 92);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x292A32), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(panel, 6, 0);
    lv_obj_set_style_pad_left(panel, 18, 0);
    lv_obj_set_style_pad_right(panel, 18, 0);
    lv_obj_set_style_pad_top(panel, 18, 0);
    lv_obj_set_style_pad_bottom(panel, 18, 0);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(panel, 4, 0);
    lv_obj_add_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(panel, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(panel, LV_SCROLLBAR_MODE_AUTO);

    static const password_item_t items[] = {
        {LV_SYMBOL_REFRESH,   "Load KDBX",       NULL, PASSWORD_ITEM_ACTION_LOAD_KDBX,       0, LV_COLOR_MAKE(0xFF, 0xC6, 0x2E)},
    };

    if (entry_count == 0) {
        add_password_item(panel, &items[0]);
    } else {
        size_t visible_count = entry_count < PASSWORD_UI_MAX_ENTRIES ? entry_count : PASSWORD_UI_MAX_ENTRIES;
        for (size_t i = 0; i < visible_count; i++) {
            const kdbx_entry_t *entry = kdbx_get_entry(i);
            if (!entry) {
                continue;
            }

            s_entry_items[i] = (password_item_t) {
                .symbol = LV_SYMBOL_KEYBOARD,
                .text = entry->title,
                .hid_text = NULL,
                .action = PASSWORD_ITEM_ACTION_SHOW_DETAIL,
                .entry_index = i,
                .color = LV_COLOR_MAKE(0xFF, 0xA8, 0x12),
            };
            add_password_item(panel, &s_entry_items[i]);
        }
    }

    if (entry_count == 0 && !s_kdbx_auto_load_attempted) {
        load_kdbx_from_data();
    }

    if (entry_count == 0 && s_kdbx_load_in_progress) {
        add_kdbx_loading_popup(screen);
    }
}

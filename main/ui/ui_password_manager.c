#include "ui_password_manager.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdatomic.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "hide_menu_item_ui.h"
#include "kdbx/kdbx_loader.h"
#include "lvgl.h"
#include "ui_nav.h"
#include "ui/i18n/ui_i18n.h"
#include "ui_password_detail.h"
#include "ui_status_bar.h"
#include "hid_keyboard.h"
#include "storage_operation_guard.h"
#include "usb_msc.h"

#define PASSWORD_MANAGER_WIDTH  480
#define PASSWORD_MANAGER_HEIGHT 640
#define KDBX_LOAD_TASK_STACK    24576
#define KDBX_LOAD_TASK_PRIORITY 3
#define KDBX_UI_PROGRESS_INTERVAL_MS 250
#define PASSWORD_UI_MAX_ENTRIES 100
#define PASSWORD_UI_MAX_GROUPS  32
#define PASSWORD_ENTRY_ICON_SCALE 166
#define PASSWORD_NETWORK_ICON_SCALE 166
#define PASSWORD_EMAIL_ICON_SCALE 260
#define PASSWORD_GENERAL_ICON_SCALE 111
#define PASSWORD_LIST_MARGIN 20
#define PASSWORD_LIST_TOP 116
#define PASSWORD_LIST_TOP_WITH_STATUS 158
#define PASSWORD_LIST_BOTTOM 20
#define PASSWORD_ITEM_HEIGHT 100
#define PASSWORD_ICON_BOX_SIZE 72
#define PASSWORD_ITEM_DRAG_THRESHOLD 10

LV_IMAGE_DECLARE(C00_Password);
LV_IMAGE_DECLARE(C01_Package_Network);
LV_IMAGE_DECLARE(C03_Server);
LV_IMAGE_DECLARE(C19_Mail_Generic);
LV_IMAGE_DECLARE(C37_KPercentage);
LV_IMAGE_DECLARE(C38_Samba_Unmount);
LV_IMAGE_DECLARE(C48_Folder);

static const char *TAG = "ui_password";
static lv_obj_t *s_status_label;
static atomic_bool s_kdbx_load_in_progress;
static atomic_int s_kdbx_last_result = ATOMIC_VAR_INIT(ESP_ERR_INVALID_STATE);
static bool s_kdbx_auto_load_attempted;
static bool s_password_manager_visible;
static volatile password_database_mode_t s_database_mode;
static QueueHandle_t s_ui_cmd_queue;
static lv_timer_t *s_ui_cmd_timer;
static lv_obj_t *s_password_items_panel;
static size_t s_current_group_index = SIZE_MAX;
static atomic_bool s_load_progress_dirty;
static uint32_t s_last_progress_refresh_tick;
static size_t s_rendered_group_items;
static size_t s_rendered_entry_items;
static bool s_rendered_has_content;
static lv_obj_t *s_pressed_password_item;
static lv_point_t s_password_item_press_point;
static bool s_password_item_dragged;

typedef enum {
    PASSWORD_UI_CMD_LOAD_FINISHED,
} password_ui_cmd_t;

static void kdbx_load_finished_ui(void);
static void password_ui_cmd_timer_cb(lv_timer_t *timer);
static void ensure_password_ui_cmd_timer(void);
static bool post_password_ui_cmd(password_ui_cmd_t cmd);
static void start_kdbx_load(bool update_status);
static void password_manager_gesture_event_cb(lv_event_t *event);
static void refresh_password_manager_progress(void);

static void password_manager_show_async(void *user_data)
{
    (void)user_data;
    ui_password_manager_show();
}

static bool kdbx_has_visible_content(void)
{
    if (kdbx_get_entry_count() > 0) {
        return true;
    }
    for (size_t i = 0; i < kdbx_get_group_count(); i++) {
        const kdbx_group_t *group = kdbx_get_group(i);
        if (group && group->depth > 0) {
            return true;
        }
    }
    return false;
}

static const char *database_display_name(void)
{
    size_t group_count = kdbx_get_group_count();
    for (size_t i = 0; i < group_count; i++) {
        const kdbx_group_t *group = kdbx_get_group(i);
        if (group && group->depth == 0 && group->name[0] != '\0') {
            return group->name;
        }
    }
    return "KDBX";
}

static size_t database_root_group_index(void)
{
    size_t group_count = kdbx_get_group_count();
    for (size_t i = 0; i < group_count; i++) {
        const kdbx_group_t *group = kdbx_get_group(i);
        if (group && group->depth == 0) {
            return i;
        }
    }
    return SIZE_MAX;
}

static bool current_group_is_root(void)
{
    return s_current_group_index == database_root_group_index();
}

static const char *current_group_display_name(void)
{
    const kdbx_group_t *group = kdbx_get_group(s_current_group_index);
    return group && group->name[0] ? group->name : database_display_name();
}

static void current_folder_item_counts(size_t *group_items, size_t *entry_items)
{
    size_t current_group = s_current_group_index;
    if (current_group == SIZE_MAX) {
        current_group = database_root_group_index();
    }

    size_t visible_groups = 0;
    size_t group_count = kdbx_get_group_count();
    for (size_t i = 0; i < group_count && visible_groups < PASSWORD_UI_MAX_GROUPS; i++) {
        const kdbx_group_t *group = kdbx_get_group(i);
        if (group && group->parent_index == current_group) {
            visible_groups++;
        }
    }

    size_t visible_entries = 0;
    size_t entry_count = kdbx_get_entry_count();
    for (size_t i = 0; i < entry_count && visible_entries < PASSWORD_UI_MAX_ENTRIES; i++) {
        const kdbx_entry_t *entry = kdbx_get_entry(i);
        if (entry && entry->group_index == current_group) {
            visible_entries++;
        }
    }

    *group_items = visible_groups;
    *entry_items = visible_entries;
}

static void set_status_text(const char *text)
{
    if (s_status_label) {
        lv_label_set_text(s_status_label, text);
    }
}

bool ui_password_manager_is_loading(void)
{
    return atomic_load_explicit(&s_kdbx_load_in_progress, memory_order_acquire);
}

void ui_password_manager_notify_load_progress(void)
{
    atomic_store_explicit(&s_load_progress_dirty, true, memory_order_release);
}

void ui_password_manager_reload(void)
{
    start_kdbx_load(false);
}

static void kdbx_load_task(void *arg)
{
    (void)arg;
    bool task_wdt_registered = esp_task_wdt_add(NULL) == ESP_OK;

    esp_err_t ret = usb_hid_mount_usbstore_app();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "usbstore is not mounted to APP: %s", esp_err_to_name(ret));
        goto finished;
    }

    ESP_LOGW(TAG, "usbstore is mounted to APP");
    password_database_mode_t loaded_mode;
    do {
        loaded_mode = s_database_mode;
        if (loaded_mode == PASSWORD_DATABASE_MODE_EMERGENCY) {
            ret = kdbx_load_emergency_database_from_data();
        } else if (loaded_mode == PASSWORD_DATABASE_MODE_DURESS) {
            ret = kdbx_load_duress_database_from_data();
        } else {
            ret = kdbx_load_first_database_from_data();
        }
    } while (loaded_mode != s_database_mode);
    if (ret == ESP_OK) {
        if (kdbx_has_visible_content()) {
            ESP_LOGI(TAG, "KDBX parsed: entries=%u groups=%u",
                     (unsigned)kdbx_get_entry_count(), (unsigned)kdbx_get_group_count());
        } else {
            ESP_LOGW(TAG, "KDBX parsed without visible entries: entries=%u groups=%u",
                     (unsigned)kdbx_get_entry_count(), (unsigned)kdbx_get_group_count());
        }
    } else if (ret == ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGI(TAG, "KDBX parser reached current implementation limit");
    } else {
        ESP_LOGW(TAG, "Failed to parse KDBX: %s", esp_err_to_name(ret));
    }

finished:
    atomic_store_explicit(&s_kdbx_last_result, ret, memory_order_relaxed);
    if (task_wdt_registered) {
        esp_task_wdt_delete(NULL);
    }
    storage_operation_release(STORAGE_OPERATION_KDBX_LOAD);
    atomic_store_explicit(&s_kdbx_load_in_progress, false, memory_order_release);
    post_password_ui_cmd(PASSWORD_UI_CMD_LOAD_FINISHED);
    vTaskDelete(NULL);
}

static void start_kdbx_load(bool update_status)
{
    ensure_password_ui_cmd_timer();
    if (!hide_menu_item_ui_is_visible(MAIN_MENU_ITEM_PASSWORD_MANAGER)) {
        ESP_LOGI(TAG, "Password Management menu item is disabled; skipping KDBX load");
        return;
    }

    bool expected = false;
    if (!atomic_compare_exchange_strong_explicit(&s_kdbx_load_in_progress,
                                                  &expected,
                                                  true,
                                                  memory_order_acq_rel,
                                                  memory_order_acquire)) {
        if (update_status) {
            set_status_text(ui_i18n_get(UI_STR_KDBX_PARSING));
        }
        return;
    }

    if (!storage_operation_try_acquire(STORAGE_OPERATION_KDBX_LOAD)) {
        storage_operation_t active_operation = storage_operation_get();
        atomic_store_explicit(&s_kdbx_last_result, ESP_ERR_INVALID_STATE, memory_order_relaxed);
        atomic_store_explicit(&s_kdbx_load_in_progress, false, memory_order_release);
        if (update_status) {
            set_status_text(ui_i18n_get(UI_STR_KDBX_BLOCKED_BY_UPDATE));
        }
        ESP_LOGW(TAG, "KDBX load blocked by active %s operation",
                 storage_operation_name(active_operation));
        return;
    }

    if (update_status) {
        set_status_text(ui_i18n_get(UI_STR_KDBX_PARSING));
    }
    if (s_ui_cmd_timer) {
        ui_status_bar_set_refresh_visible(true);
    }
    s_kdbx_auto_load_attempted = true;
    s_current_group_index = SIZE_MAX;
    atomic_store_explicit(&s_kdbx_last_result, ESP_ERR_INVALID_STATE, memory_order_relaxed);
    if (xTaskCreate(kdbx_load_task, "kdbx_load", KDBX_LOAD_TASK_STACK, NULL, KDBX_LOAD_TASK_PRIORITY, NULL) != pdPASS) {
        storage_operation_release(STORAGE_OPERATION_KDBX_LOAD);
        atomic_store_explicit(&s_kdbx_last_result, ESP_ERR_NO_MEM, memory_order_relaxed);
        atomic_store_explicit(&s_kdbx_load_in_progress, false, memory_order_release);
        ui_status_bar_set_refresh_visible(false);
        if (update_status) {
            set_status_text(ui_i18n_get(UI_STR_KDBX_TASK_FAILED));
        }
        ESP_LOGW(TAG, "Failed to create KDBX load task");
    }
}

static void load_kdbx_from_data(void)
{
    start_kdbx_load(true);
}

void ui_password_manager_set_database_mode(password_database_mode_t mode)
{
    bool changed = s_database_mode != mode;
    s_database_mode = mode;
    if (changed || kdbx_get_entry_count() == 0) {
        start_kdbx_load(false);
    }
}

typedef enum {
    PASSWORD_ITEM_ACTION_TYPE,
    PASSWORD_ITEM_ACTION_DUMP_STORAGE,
    PASSWORD_ITEM_ACTION_LOAD_KDBX,
    PASSWORD_ITEM_ACTION_OPEN_GROUP,
    PASSWORD_ITEM_ACTION_SHOW_DETAIL,
} password_item_action_t;

typedef struct {
    const char *symbol;
    const char *text;
    const char *hid_text;
    password_item_action_t action;
    size_t entry_index;
    size_t group_index;
    lv_color_t color;
    const lv_image_dsc_t *image;
    uint16_t image_scale;
} password_item_t;

static password_item_t s_entry_items[PASSWORD_UI_MAX_ENTRIES];
static password_item_t s_group_items[PASSWORD_UI_MAX_GROUPS];

static const lv_image_dsc_t *get_group_image(const char *name, uint16_t *scale)
{
    if (strcmp(name, "Internet") == 0) {
        *scale = PASSWORD_NETWORK_ICON_SCALE;
        return &C01_Package_Network;
    }
    if (strcmp(name, "Network") == 0) {
        *scale = PASSWORD_NETWORK_ICON_SCALE;
        return &C03_Server;
    }
    if (strcmp(name, "Email") == 0 || strcmp(name, "email") == 0 ||
        strcmp(name, "eMail") == 0) {
        *scale = PASSWORD_EMAIL_ICON_SCALE;
        return &C19_Mail_Generic;
    }
    if (strcmp(name, "General") == 0) {
        *scale = PASSWORD_GENERAL_ICON_SCALE;
        return &C48_Folder;
    }
    if (strcmp(name, "Windows") == 0) {
        *scale = PASSWORD_NETWORK_ICON_SCALE;
        return &C38_Samba_Unmount;
    }
    if (strcmp(name, "Homebanking") == 0 || strcmp(name, "homebanking") == 0) {
        *scale = PASSWORD_NETWORK_ICON_SCALE;
        return &C37_KPercentage;
    }
    *scale = 0;
    return NULL;
}

static const char *get_group_display_name(const char *name)
{
    if (strcmp(name, "General") == 0 || strcmp(name, "general") == 0) {
        return ui_i18n_get(UI_STR_GROUP_GENERAL);
    }
    if (strcmp(name, "Windows") == 0 || strcmp(name, "windows") == 0) {
        return ui_i18n_get(UI_STR_GROUP_WINDOWS);
    }
    if (strcmp(name, "Network") == 0 || strcmp(name, "network") == 0) {
        return ui_i18n_get(UI_STR_GROUP_NETWORK);
    }
    if (strcmp(name, "Internet") == 0 || strcmp(name, "internet") == 0) {
        return ui_i18n_get(UI_STR_GROUP_INTERNET);
    }
    if (strcmp(name, "Email") == 0 || strcmp(name, "email") == 0 ||
        strcmp(name, "eMail") == 0) {
        return ui_i18n_get(UI_STR_GROUP_EMAIL);
    }
    if (strcmp(name, "Homebanking") == 0 || strcmp(name, "homebanking") == 0) {
        return ui_i18n_get(UI_STR_GROUP_HOMEBANKING);
    }
    return name;
}

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

    uint32_t now = lv_tick_get();
    if (atomic_load_explicit(&s_load_progress_dirty, memory_order_acquire) &&
        lv_tick_elaps(s_last_progress_refresh_tick) >= KDBX_UI_PROGRESS_INTERVAL_MS) {
        atomic_exchange_explicit(&s_load_progress_dirty, false, memory_order_acq_rel);
        s_last_progress_refresh_tick = now;
        refresh_password_manager_progress();
    }

    if (!s_ui_cmd_queue) {
        return;
    }

    password_ui_cmd_t cmd;
    while (xQueueReceive(s_ui_cmd_queue, &cmd, 0) == pdTRUE) {
        if (cmd == PASSWORD_UI_CMD_LOAD_FINISHED) {
            atomic_store_explicit(&s_load_progress_dirty, false, memory_order_release);
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
    ui_status_bar_set_refresh_visible(ui_password_manager_is_loading());
    if (s_password_manager_visible) {
        ui_password_manager_show();
    }
}

static void password_status_delete_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_DELETE &&
        lv_event_get_target(event) == s_status_label) {
        s_status_label = NULL;
        s_password_manager_visible = false;
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
    lv_label_set_text(label, ui_i18n_get(UI_STR_KDBX_LOADING));
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_set_style_text_font(label, ui_i18n_font_35(), 0);
    lv_obj_center(label);
}

static void password_item_event_cb(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);
    lv_obj_t *target = lv_event_get_target(event);

    if (code == LV_EVENT_PRESSED) {
        lv_indev_t *indev = lv_indev_active();
        s_pressed_password_item = target;
        s_password_item_dragged = false;
        if (indev) {
            lv_indev_get_point(indev, &s_password_item_press_point);
        }
        return;
    }

    if (code == LV_EVENT_PRESSING && target == s_pressed_password_item) {
        lv_indev_t *indev = lv_indev_active();
        if (indev) {
            lv_point_t point;
            lv_indev_get_point(indev, &point);
            int32_t delta_x = point.x - s_password_item_press_point.x;
            int32_t delta_y = point.y - s_password_item_press_point.y;
            if (delta_x > PASSWORD_ITEM_DRAG_THRESHOLD ||
                delta_x < -PASSWORD_ITEM_DRAG_THRESHOLD ||
                delta_y > PASSWORD_ITEM_DRAG_THRESHOLD ||
                delta_y < -PASSWORD_ITEM_DRAG_THRESHOLD) {
                s_password_item_dragged = true;
            }
        }
        return;
    }

    if (code == LV_EVENT_PRESS_LOST) {
        if (target == s_pressed_password_item) {
            s_pressed_password_item = NULL;
            s_password_item_dragged = false;
        }
        return;
    }

    if (code != LV_EVENT_CLICKED) {
        return;
    }

    bool ignore_click = target != s_pressed_password_item || s_password_item_dragged;
    s_pressed_password_item = NULL;
    s_password_item_dragged = false;
    if (ignore_click) {
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
        lv_obj_remove_event_cb(lv_screen_active(), password_manager_gesture_event_cb);
        ui_password_detail_open(item->entry_index);
        return;
    }

    if (item->action == PASSWORD_ITEM_ACTION_OPEN_GROUP) {
        if (kdbx_get_group(item->group_index)) {
            s_current_group_index = item->group_index;
            lv_async_call(password_manager_show_async, NULL);
        }
        return;
    }

    if (!item->hid_text) {
        return;
    }

    hid_type_string((char *)item->hid_text);
}

static void add_password_item(lv_obj_t *parent, const password_item_t *item)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, PASSWORD_MANAGER_WIDTH - (PASSWORD_LIST_MARGIN * 2) - 24,
                    PASSWORD_ITEM_HEIGHT);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(row, lv_color_hex(0x353740), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_radius(row, 4, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row, password_item_event_cb, LV_EVENT_ALL, (void *)item);

    lv_obj_t *icon_box = lv_obj_create(row);
    lv_obj_remove_style_all(icon_box);
    lv_obj_set_size(icon_box, PASSWORD_ICON_BOX_SIZE, PASSWORD_ICON_BOX_SIZE);
    lv_obj_align(icon_box, LV_ALIGN_LEFT_MID, 4, 0);
    lv_obj_set_style_bg_opa(icon_box, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(icon_box, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *icon;
    if (item->image) {
        icon = lv_image_create(icon_box);
        lv_image_set_src(icon, item->image);
        lv_image_set_scale(icon, item->image_scale ? item->image_scale : PASSWORD_ENTRY_ICON_SCALE);
    } else {
        icon = lv_label_create(icon_box);
        lv_label_set_text(icon, item->symbol);
        lv_obj_set_style_text_color(icon, item->color, 0);
        lv_obj_set_style_text_font(icon, ui_i18n_font_35(), 0);
    }
    lv_obj_center(icon);

    lv_obj_t *label = lv_label_create(row);
    lv_label_set_text(label, item->text);
    lv_obj_set_width(label, PASSWORD_MANAGER_WIDTH - 162);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_set_style_text_font(label, ui_i18n_font_35(), 0);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, 86, 0);
}

static void initialize_entry_item(password_item_t *item,
                                  size_t entry_index,
                                  const kdbx_entry_t *entry)
{
    bool use_folder_icon = entry->icon_id == 48;
    *item = (password_item_t) {
        .text = entry->title,
        .hid_text = NULL,
        .action = PASSWORD_ITEM_ACTION_SHOW_DETAIL,
        .entry_index = entry_index,
        .color = LV_COLOR_MAKE(0xFF, 0xA8, 0x12),
        .image = use_folder_icon ? &C48_Folder : &C00_Password,
        .image_scale = use_folder_icon ? PASSWORD_GENERAL_ICON_SCALE :
                                         PASSWORD_ENTRY_ICON_SCALE,
    };
}

static void append_current_folder_entries(void)
{
    if (!s_password_items_panel || s_rendered_entry_items >= PASSWORD_UI_MAX_ENTRIES) {
        return;
    }

    size_t matching_index = 0;
    size_t entry_count = kdbx_get_entry_count();
    for (size_t i = 0; i < entry_count && matching_index < PASSWORD_UI_MAX_ENTRIES; i++) {
        const kdbx_entry_t *entry = kdbx_get_entry(i);
        if (!entry || entry->group_index != s_current_group_index) {
            continue;
        }

        if (matching_index >= s_rendered_entry_items) {
            initialize_entry_item(&s_entry_items[matching_index], i, entry);
            add_password_item(s_password_items_panel, &s_entry_items[matching_index]);
        }
        matching_index++;
    }
    s_rendered_entry_items = matching_index;
}

static void refresh_password_manager_progress(void)
{
    if (!s_password_manager_visible || !ui_password_manager_is_loading()) {
        return;
    }

    size_t root_group = database_root_group_index();
    if (!s_rendered_has_content ||
        (s_current_group_index == SIZE_MAX && root_group != SIZE_MAX) ||
        !s_password_items_panel) {
        if (kdbx_has_visible_content()) {
            ui_password_manager_show();
        }
        return;
    }

    size_t group_items;
    size_t entry_items;
    current_folder_item_counts(&group_items, &entry_items);
    if (group_items != s_rendered_group_items || entry_items < s_rendered_entry_items) {
        ui_password_manager_show();
    } else if (entry_items > s_rendered_entry_items) {
        append_current_folder_entries();
    }
}

static void password_manager_go_up(void)
{
    const kdbx_group_t *group = kdbx_get_group(s_current_group_index);
    if (group && !current_group_is_root()) {
        s_current_group_index = group->parent_index;
        lv_async_call(password_manager_show_async, NULL);
    } else {
        ui_nav_go_back();
    }
}

static void password_manager_back_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_CLICKED) {
        password_manager_go_up();
    }
}

static void password_manager_gesture_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_GESTURE &&
        lv_indev_get_gesture_dir(lv_indev_active()) == LV_DIR_RIGHT) {
        password_manager_go_up();
    }
}

void ui_password_manager_show(void)
{
    ensure_password_ui_cmd_timer();

    lv_obj_t *screen = lv_screen_active();
    lv_obj_clean(screen);
    s_password_items_panel = NULL;
    s_password_manager_visible = true;
    ui_nav_disable_right_swipe(screen);
    lv_obj_remove_event_cb(screen, password_manager_gesture_event_cb);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x11141A), 0);
    lv_obj_set_style_pad_all(screen, 0, 0);

    ui_status_bar_add(screen, PASSWORD_MANAGER_WIDTH);

    uint16_t kdbx_major;
    uint16_t kdbx_minor;
    bool has_file_version = atomic_load_explicit(&s_kdbx_last_result, memory_order_acquire) == ESP_OK &&
                            kdbx_get_file_version(&kdbx_major, &kdbx_minor);

    size_t root_group_index = database_root_group_index();
    if (s_current_group_index == SIZE_MAX && root_group_index != SIZE_MAX) {
        s_current_group_index = root_group_index;
    } else if (s_current_group_index != SIZE_MAX && !kdbx_get_group(s_current_group_index)) {
        s_current_group_index = root_group_index;
    }
    bool nested_group = !current_group_is_root();
    if (nested_group) {
        lv_obj_add_event_cb(screen, password_manager_gesture_event_cb,
                            LV_EVENT_GESTURE, NULL);
    } else {
        ui_nav_enable_right_swipe(screen);
    }

    if (nested_group) {
        lv_obj_t *back = lv_button_create(screen);
        lv_obj_set_size(back, 48, 48);
        lv_obj_align(back, LV_ALIGN_TOP_LEFT, 12, 58);
        lv_obj_set_style_bg_opa(back, LV_OPA_TRANSP, 0);
        lv_obj_set_style_shadow_width(back, 0, 0);
        lv_obj_add_event_cb(back, password_manager_back_event_cb, LV_EVENT_CLICKED, NULL);

        lv_obj_t *back_icon = lv_label_create(back);
        lv_label_set_text(back_icon, LV_SYMBOL_LEFT);
        lv_obj_set_style_text_color(back_icon, lv_color_white(), 0);
        lv_obj_set_style_text_font(back_icon, &lv_font_montserrat_28, 0);
        lv_obj_center(back_icon);
    }

    lv_obj_t *title = lv_label_create(screen);
    lv_label_set_text(title, current_group_display_name());
    lv_obj_set_width(title, has_file_version ? (nested_group ? 240 : 300) :
                                               (nested_group ? 356 : PASSWORD_MANAGER_WIDTH - 48));
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_set_style_text_font(title, ui_i18n_font_35(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT,
                 nested_group ? 72 : PASSWORD_LIST_MARGIN, 64);

    if (has_file_version) {
        char version_text[20];
        snprintf(version_text, sizeof(version_text), "KDBX %u.%u", kdbx_major, kdbx_minor);

        lv_obj_t *version_label = lv_label_create(screen);
        lv_label_set_text(version_label, version_text);
        lv_obj_set_style_text_color(version_label, lv_color_hex(0x9EA3AD), 0);
        lv_obj_set_style_text_font(version_label, &lv_font_montserrat_24, 0);
        lv_obj_align(version_label, LV_ALIGN_TOP_RIGHT, -PASSWORD_LIST_MARGIN, 72);
    }

    s_status_label = lv_label_create(screen);
    char status_text[48];
    size_t entry_count = kdbx_get_entry_count();
    size_t group_count = kdbx_get_group_count();
    bool has_content = kdbx_has_visible_content();
    if (has_content) {
        status_text[0] = '\0';
        lv_label_set_long_mode(s_status_label, LV_LABEL_LONG_CLIP);
        lv_label_set_text(s_status_label, status_text);
        lv_obj_add_flag(s_status_label, LV_OBJ_FLAG_HIDDEN);
    } else if (ui_password_manager_is_loading()) {
        snprintf(status_text, sizeof(status_text), "%s", ui_i18n_get(UI_STR_KDBX_PARSING));
        lv_label_set_long_mode(s_status_label, LV_LABEL_LONG_CLIP);
        lv_label_set_text(s_status_label, status_text);
    } else if (!s_kdbx_auto_load_attempted) {
        snprintf(status_text, sizeof(status_text), "%s", ui_i18n_get(UI_STR_KDBX_PARSING));
        lv_label_set_long_mode(s_status_label, LV_LABEL_LONG_CLIP);
        lv_label_set_text(s_status_label, status_text);
    } else {
        esp_err_t load_result = atomic_load_explicit(&s_kdbx_last_result, memory_order_acquire);
        const char *message;
        if (load_result == ESP_OK) {
            message = ui_i18n_get(UI_STR_KDBX_EMPTY);
        } else if (load_result == KDBX_ERR_DATABASE_NOT_FOUND) {
            message = ui_i18n_get(UI_STR_KDBX_FILE_NOT_FOUND);
        } else {
            message = ui_i18n_get(UI_STR_KDBX_LOAD_FAILED);
        }
        snprintf(status_text, sizeof(status_text), "%s", message);
        lv_label_set_long_mode(s_status_label, LV_LABEL_LONG_CLIP);
        lv_label_set_text(s_status_label, status_text);
    }
    lv_obj_set_width(s_status_label, PASSWORD_MANAGER_WIDTH - 48);
    lv_obj_set_style_text_color(s_status_label, lv_color_hex(0x9EA3AD), 0);
    lv_obj_set_style_text_font(s_status_label, ui_i18n_font_35(), 0);
    lv_obj_align(s_status_label, LV_ALIGN_TOP_LEFT, PASSWORD_LIST_MARGIN, 108);
    lv_obj_add_event_cb(s_status_label, password_status_delete_cb, LV_EVENT_DELETE, NULL);

    lv_obj_t *panel = lv_obj_create(screen);
    s_password_items_panel = panel;
    lv_obj_remove_style_all(panel);
    int32_t panel_top = has_content ? PASSWORD_LIST_TOP : PASSWORD_LIST_TOP_WITH_STATUS;
    lv_obj_set_size(panel, PASSWORD_MANAGER_WIDTH - (PASSWORD_LIST_MARGIN * 2),
                    PASSWORD_MANAGER_HEIGHT - panel_top - PASSWORD_LIST_BOTTOM);
    lv_obj_align(panel, LV_ALIGN_TOP_MID, 0, panel_top);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x292A32), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(panel, 4, 0);
    lv_obj_set_style_pad_left(panel, 12, 0);
    lv_obj_set_style_pad_right(panel, 12, 0);
    lv_obj_set_style_pad_top(panel, 10, 0);
    lv_obj_set_style_pad_bottom(panel, 10, 0);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(panel, 0, 0);
    lv_obj_add_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(panel, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_set_scroll_dir(panel, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(panel, LV_SCROLLBAR_MODE_AUTO);

    const password_item_t items[] = {
        {
            .symbol = LV_SYMBOL_REFRESH,
            .text = ui_i18n_get(UI_STR_LOAD_KDBX),
            .action = PASSWORD_ITEM_ACTION_LOAD_KDBX,
            .color = LV_COLOR_MAKE(0xFF, 0xC6, 0x2E),
        },
    };

    if (!has_content) {
        s_rendered_group_items = 0;
        s_rendered_entry_items = 0;
        add_password_item(panel, &items[0]);
    } else {
        size_t group_item_count = 0;
        for (size_t i = 0; i < group_count && group_item_count < PASSWORD_UI_MAX_GROUPS; i++) {
            const kdbx_group_t *group = kdbx_get_group(i);
            if (!group || group->parent_index != s_current_group_index) {
                continue;
            }
            uint16_t image_scale;
            const lv_image_dsc_t *group_image;
            if (!nested_group) {
                image_scale = PASSWORD_GENERAL_ICON_SCALE;
                group_image = &C48_Folder;
            } else {
                group_image = get_group_image(group->name, &image_scale);
            }
            s_group_items[group_item_count] = (password_item_t) {
                .symbol = LV_SYMBOL_DIRECTORY,
                .text = get_group_display_name(group->name),
                .action = PASSWORD_ITEM_ACTION_OPEN_GROUP,
                .group_index = i,
                .color = LV_COLOR_MAKE(0x65, 0xB8, 0xFF),
                .image = group_image,
                .image_scale = image_scale,
            };
            add_password_item(panel, &s_group_items[group_item_count++]);
        }
        s_rendered_group_items = group_item_count;

        size_t entry_item_count = 0;
        for (size_t i = 0; i < entry_count && entry_item_count < PASSWORD_UI_MAX_ENTRIES; i++) {
            const kdbx_entry_t *entry = kdbx_get_entry(i);
            if (!entry || entry->group_index != s_current_group_index) {
                continue;
            }

            initialize_entry_item(&s_entry_items[entry_item_count], i, entry);
            add_password_item(panel, &s_entry_items[entry_item_count++]);
        }
        s_rendered_entry_items = entry_item_count;
    }
    s_rendered_has_content = has_content;

    if (!has_content && !s_kdbx_auto_load_attempted) {
        load_kdbx_from_data();
    }

    if (!has_content && ui_password_manager_is_loading()) {
        add_kdbx_loading_popup(screen);
    }
}

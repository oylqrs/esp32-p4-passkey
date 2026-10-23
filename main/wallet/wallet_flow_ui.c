#include "wallet_flow_ui.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "Qrcode/qrcode_text.h"
#include "jade_wallet_ui.h"
#include "jade_usb_protocol.h"
#include "lvgl.h"
#include "mbedtls/platform_util.h"
#include "ui/i18n/ui_i18n.h"
#include "ui/ui_nav.h"
#include "ui/ui_status_bar.h"
#include "wallet_service.h"

#define WALLET_UI_WIDTH 480
#define WALLET_HEADER_TOP 56
#define WALLET_HEADER_HEIGHT 72
#define WALLET_CONTENT_TOP 140
#define WALLET_ITEM_HEIGHT 86
#define WALLET_ITEM_STEP 100
#define WALLET_UNLOCK_TASK_STACK_SIZE 8192
#define WALLET_UNLOCK_TASK_PRIORITY 3

typedef enum {
    PIN_FLOW_CREATE,
    PIN_FLOW_UNLOCK,
    PIN_FLOW_CHANGE_UNLOCK,
    PIN_FLOW_CHANGE_NEW,
} pin_flow_t;

typedef struct {
    char pin[WALLET_STORAGE_PIN_MAX_LEN + 1];
    pin_flow_t flow;
} wallet_unlock_job_t;

typedef struct {
    esp_err_t result;
    pin_flow_t flow;
} wallet_unlock_result_t;

static char s_pending_mnemonic[WALLET_STORAGE_MNEMONIC_MAX_LEN];
static char s_first_pin[WALLET_STORAGE_PIN_MAX_LEN + 1];
static lv_obj_t *s_pin_input;
static lv_obj_t *s_pin_hint;
static lv_obj_t *s_sensitive_input;
static pin_flow_t s_pin_flow;
static bool s_pin_confirming;
static wallet_network_t s_network = WALLET_NETWORK_BITCOIN;
static uint32_t s_receive_display_index[WALLET_STORAGE_NETWORK_COUNT];
static bool s_receive_display_valid[WALLET_STORAGE_NETWORK_COUNT];
static pin_flow_t s_pending_pin_flow;
static char s_psbt_base64[WALLET_PSBT_BASE64_MAX_LEN];
static char s_signed_psbt_base64[WALLET_PSBT_BASE64_MAX_LEN];
static wallet_psbt_summary_t s_psbt_summary;
static lv_obj_t *s_psbt_input;
static QueueHandle_t s_unlock_result_queue;
static lv_timer_t *s_unlock_result_timer;
static lv_obj_t *s_unlock_overlay;
static bool s_unlock_in_progress;
static const char *TAG = "wallet_flow_ui";

static void pin_screen_event_adapter(lv_event_t *event);
static void async_wallet_home(void *unused);
static void async_show_pin(void *unused);
static void wallet_flow_render_receive(void);
static void wallet_flow_show_psbt_review(void);
static void wallet_flow_show_signed_psbt(void);
static void wallet_flow_show_signed_psbt_qr(void);

static void hide_unlock_loading(void)
{
    if (s_unlock_overlay && lv_obj_is_valid(s_unlock_overlay)) {
        lv_obj_delete(s_unlock_overlay);
    }
    s_unlock_overlay = NULL;
}

static void unlock_result_timer_cb(lv_timer_t *timer)
{
    wallet_unlock_result_t result;
    if (!s_unlock_result_queue ||
        xQueueReceive(s_unlock_result_queue, &result, 0) != pdTRUE) {
        return;
    }

    lv_timer_delete(timer);
    s_unlock_result_timer = NULL;
    s_unlock_in_progress = false;
    hide_unlock_loading();

    if (result.result != ESP_OK) {
        if (s_pin_hint && lv_obj_is_valid(s_pin_hint)) {
            lv_label_set_text(s_pin_hint, ui_i18n_get(UI_STR_WALLET_WRONG_PIN));
        }
        return;
    }
    if (result.flow == PIN_FLOW_CHANGE_UNLOCK) {
        s_pending_pin_flow = PIN_FLOW_CHANGE_NEW;
        lv_async_call(async_show_pin, NULL);
    } else {
        lv_async_call(async_wallet_home, NULL);
    }
}

static void wallet_unlock_task(void *arg)
{
    wallet_unlock_job_t *job = arg;
    wallet_unlock_result_t result = {
        .result = wallet_service_unlock(job->pin),
        .flow = job->flow,
    };
    mbedtls_platform_zeroize(job, sizeof(*job));
    free(job);
    (void)xQueueSend(s_unlock_result_queue, &result, portMAX_DELAY);
    vTaskDelete(NULL);
}

static void show_unlock_loading(void)
{
    s_unlock_overlay = lv_obj_create(lv_screen_active());
    lv_obj_add_flag(s_unlock_overlay, LV_OBJ_FLAG_FLOATING | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(s_unlock_overlay, WALLET_UI_WIDTH, LV_PCT(100));
    lv_obj_center(s_unlock_overlay);
    lv_obj_set_style_bg_color(s_unlock_overlay, lv_color_hex(0x080A0D), 0);
    lv_obj_set_style_bg_opa(s_unlock_overlay, LV_OPA_90, 0);
    lv_obj_set_style_border_width(s_unlock_overlay, 0, 0);
    lv_obj_set_style_radius(s_unlock_overlay, 0, 0);
    lv_obj_clear_flag(s_unlock_overlay, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *spinner = lv_spinner_create(s_unlock_overlay);
    lv_obj_set_size(spinner, 92, 92);
    lv_obj_set_style_arc_width(spinner, 9, LV_PART_MAIN);
    lv_obj_set_style_arc_width(spinner, 9, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(spinner, lv_color_hex(0x3A3D45), LV_PART_MAIN);
    lv_obj_set_style_arc_color(spinner, lv_color_hex(0x00A688), LV_PART_INDICATOR);
    lv_obj_align(spinner, LV_ALIGN_CENTER, 0, -42);

    lv_obj_t *label = lv_label_create(s_unlock_overlay);
    lv_label_set_text(label, ui_i18n_get(UI_STR_WALLET_UNLOCKING));
    lv_obj_set_width(label, 420);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_set_style_text_font(label, ui_i18n_font_28(), 0);
    lv_obj_align(label, LV_ALIGN_CENTER, 0, 48);
}

static bool start_wallet_unlock(const char *pin, pin_flow_t flow)
{
    if (s_unlock_in_progress) {
        return false;
    }
    if (!s_unlock_result_queue) {
        s_unlock_result_queue = xQueueCreate(1, sizeof(wallet_unlock_result_t));
    }
    wallet_unlock_job_t *job = calloc(1, sizeof(*job));
    if (!s_unlock_result_queue || !job) {
        free(job);
        return false;
    }
    strlcpy(job->pin, pin, sizeof(job->pin));
    job->flow = flow;
    show_unlock_loading();
    s_unlock_in_progress = true;
    s_unlock_result_timer = lv_timer_create(unlock_result_timer_cb, 30, NULL);
    if (!s_unlock_result_timer ||
        xTaskCreate(wallet_unlock_task, "wallet_unlock",
                    WALLET_UNLOCK_TASK_STACK_SIZE, job,
                    WALLET_UNLOCK_TASK_PRIORITY, NULL) != pdPASS) {
        if (s_unlock_result_timer) {
            lv_timer_delete(s_unlock_result_timer);
            s_unlock_result_timer = NULL;
        }
        s_unlock_in_progress = false;
        hide_unlock_loading();
        mbedtls_platform_zeroize(job, sizeof(*job));
        free(job);
        return false;
    }
    return true;
}

static const char *const s_pin_map[] = {
    "1", "2", "3", "\n",
    "4", "5", "6", "\n",
    "7", "8", "9", "\n",
    LV_SYMBOL_BACKSPACE, "0", LV_SYMBOL_OK, ""
};

static void wipe_textarea(lv_obj_t *textarea)
{
    if (!textarea || !lv_obj_is_valid(textarea)) {
        return;
    }
    size_t length = strlen(lv_textarea_get_text(textarea));
    char overwrite[WALLET_STORAGE_MNEMONIC_MAX_LEN];
    if (length >= sizeof(overwrite)) {
        length = sizeof(overwrite) - 1;
    }
    memset(overwrite, '0', length);
    overwrite[length] = '\0';
    lv_textarea_set_text(textarea, overwrite);
    lv_textarea_set_text(textarea, "");
    mbedtls_platform_zeroize(overwrite, sizeof(overwrite));
}

static void async_wallet_home(void *unused)
{
    (void)unused;
    jade_wallet_ui_show();
}

static void async_show_setup(void *unused)
{
    (void)unused;
    wallet_flow_show_setup();
}

static void async_show_unlock(void *unused)
{
    (void)unused;
    wallet_flow_show_unlock();
}

static void async_show_receive(void *unused)
{
    (void)unused;
    wallet_flow_show_receive();
}

static void async_show_options(void *unused)
{
    (void)unused;
    wallet_flow_show_options();
}

static void async_refresh_receive(void *unused)
{
    (void)unused;
    wallet_flow_render_receive();
}

static void async_show_xpub(void *unused)
{
    (void)unused;
    wallet_flow_show_xpub();
}

static void async_show_psbt_input(void *unused)
{
    (void)unused;
    wallet_flow_show_psbt_input();
}

static void async_show_psbt_review(void *unused)
{
    (void)unused;
    wallet_flow_show_psbt_review();
}

static void async_show_signed_psbt(void *unused)
{
    (void)unused;
    wallet_flow_show_signed_psbt();
}

static void async_show_signed_psbt_qr(void *unused)
{
    (void)unused;
    wallet_flow_show_signed_psbt_qr();
}

static void go_home_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_CLICKED) {
        wipe_textarea(s_pin_input);
        wipe_textarea(s_sensitive_input);
        mbedtls_platform_zeroize(s_pending_mnemonic, sizeof(s_pending_mnemonic));
        mbedtls_platform_zeroize(s_first_pin, sizeof(s_first_pin));
        mbedtls_platform_zeroize(s_psbt_base64, sizeof(s_psbt_base64));
        mbedtls_platform_zeroize(s_signed_psbt_base64, sizeof(s_signed_psbt_base64));
        memset(&s_psbt_summary, 0, sizeof(s_psbt_summary));
        lv_async_call(async_wallet_home, NULL);
    }
}

static void style_screen(const char *title)
{
    lv_obj_t *screen = lv_screen_active();
    lv_obj_clean(screen);
    s_pin_input = NULL;
    s_pin_hint = NULL;
    s_sensitive_input = NULL;
    s_psbt_input = NULL;
    ui_nav_disable_right_swipe(screen);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x080A0D), 0);
    lv_obj_set_style_pad_all(screen, 0, 0);
    ui_status_bar_add(screen, WALLET_UI_WIDTH);

    lv_obj_t *back = lv_button_create(screen);
    lv_obj_set_size(back, 108, WALLET_HEADER_HEIGHT);
    lv_obj_align(back, LV_ALIGN_TOP_LEFT, 4, WALLET_HEADER_TOP);
    lv_obj_set_style_bg_color(back, lv_color_hex(0x252830), 0);
    lv_obj_set_style_bg_opa(back, LV_OPA_50, 0);
    lv_obj_set_style_bg_opa(back, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(back, lv_color_hex(0x4B4E57), LV_STATE_PRESSED);
    lv_obj_set_style_radius(back, 8, 0);
    lv_obj_set_style_shadow_width(back, 0, 0);
    lv_obj_add_event_cb(back, go_home_event_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *icon = lv_label_create(back);
    lv_label_set_text(icon, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(icon, lv_color_white(), 0);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_48, 0);
    lv_obj_center(icon);

    lv_obj_t *heading = lv_label_create(screen);
    lv_label_set_text(heading, title);
    lv_obj_set_width(heading, 336);
    lv_label_set_long_mode(heading, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(heading, lv_color_white(), 0);
    lv_obj_set_style_text_font(heading, ui_i18n_font_28(), 0);
    lv_obj_align(heading, LV_ALIGN_TOP_LEFT, 124, 76);
}

static lv_obj_t *add_button(lv_obj_t *parent, const char *text, int32_t y,
                            lv_color_t color)
{
    lv_obj_t *button = lv_button_create(parent);
    lv_obj_set_size(button, 432, WALLET_ITEM_HEIGHT);
    lv_obj_align(button, LV_ALIGN_TOP_MID, 0, y);
    lv_obj_set_style_bg_color(button, color, 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x3A3D45), LV_STATE_PRESSED);
    lv_obj_set_style_radius(button, 8, 0);
    lv_obj_set_style_shadow_width(button, 0, 0);

    lv_obj_t *label = lv_label_create(button);
    lv_label_set_text(label, text);
    lv_obj_set_width(label, 396);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_set_style_text_font(label, ui_i18n_font_28(), 0);
    lv_obj_center(label);
    return button;
}

static void show_message(ui_string_id_t id)
{
    lv_obj_t *dialog = lv_msgbox_create(NULL);
    lv_msgbox_add_title(dialog, ui_i18n_get(UI_STR_WALLET));
    lv_msgbox_add_text(dialog, ui_i18n_get(id));
    lv_msgbox_add_close_button(dialog);
    lv_obj_set_width(dialog, 410);
    lv_obj_set_style_bg_color(dialog, lv_color_hex(0x292A32), 0);
    lv_obj_set_style_text_color(dialog, lv_color_white(), 0);
    lv_obj_set_style_text_font(dialog, ui_i18n_font_24(), 0);
}

static void normalize_mnemonic(char *text)
{
    char *read = text;
    char *write = text;
    bool pending_space = false;
    while (*read && isspace((unsigned char)*read)) {
        ++read;
    }
    while (*read) {
        if (isspace((unsigned char)*read)) {
            pending_space = write != text;
        } else {
            if (pending_space) {
                *write++ = ' ';
                pending_space = false;
            }
            *write++ = (char)tolower((unsigned char)*read);
        }
        ++read;
    }
    *write = '\0';
}

static void show_pin_screen(pin_flow_t flow);

static void async_show_pin(void *unused)
{
    (void)unused;
    show_pin_screen(s_pending_pin_flow);
}

static void show_backup_page(void *unused)
{
    (void)unused;
    style_screen(ui_i18n_get(UI_STR_WALLET_BACKUP_TITLE));
    lv_obj_t *warning = lv_label_create(lv_screen_active());
    lv_label_set_text(warning, ui_i18n_get(UI_STR_WALLET_BACKUP_WARNING));
    lv_obj_set_width(warning, 420);
    lv_label_set_long_mode(warning, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(warning, lv_color_hex(0xFFCA66), 0);
    lv_obj_set_style_text_font(warning, ui_i18n_font_24(), 0);
    lv_obj_align(warning, LV_ALIGN_TOP_MID, 0, WALLET_CONTENT_TOP);

    lv_obj_t *words_label = lv_label_create(lv_screen_active());
    lv_label_set_text_static(words_label, s_pending_mnemonic);
    lv_obj_set_size(words_label, 420, 310);
    lv_label_set_long_mode(words_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_bg_color(words_label, lv_color_hex(0x202228), 0);
    lv_obj_set_style_bg_opa(words_label, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(words_label, 18, 0);
    lv_obj_set_style_radius(words_label, 5, 0);
    lv_obj_set_style_text_color(words_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(words_label, &lv_font_montserrat_24, 0);
    lv_obj_align(words_label, LV_ALIGN_TOP_MID, 0, 205);

    lv_obj_t *next = add_button(lv_screen_active(),
                                ui_i18n_get(UI_STR_WALLET_CONTINUE),
                                550, lv_color_hex(0x00A688));
    lv_obj_add_event_cb(next, pin_screen_event_adapter,
                        LV_EVENT_CLICKED, (void *)(uintptr_t)PIN_FLOW_CREATE);
}

static void create_words_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }
    size_t words = (size_t)(uintptr_t)lv_event_get_user_data(event);
    mbedtls_platform_zeroize(s_pending_mnemonic, sizeof(s_pending_mnemonic));
    esp_err_t ret = wallet_service_generate_mnemonic(
        words, s_pending_mnemonic, sizeof(s_pending_mnemonic));
    if (ret != ESP_OK) {
        show_message(UI_STR_WALLET_STORAGE_ERROR);
        return;
    }

    lv_async_call(show_backup_page, NULL);
}

static void pin_screen_event_adapter(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_CLICKED) {
        s_pending_pin_flow = (pin_flow_t)(uintptr_t)lv_event_get_user_data(event);
        lv_async_call(async_show_pin, NULL);
    }
}

static void import_continue_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }
    lv_obj_t *textarea = lv_event_get_user_data(event);
    const char *text = lv_textarea_get_text(textarea);
    size_t length = strlen(text);
    if (length >= sizeof(s_pending_mnemonic)) {
        show_message(UI_STR_WALLET_INVALID_MNEMONIC);
        return;
    }
    memcpy(s_pending_mnemonic, text, length + 1);
    normalize_mnemonic(s_pending_mnemonic);
    if (wallet_crypto_validate_mnemonic(s_pending_mnemonic) != ESP_OK) {
        mbedtls_platform_zeroize(s_pending_mnemonic, sizeof(s_pending_mnemonic));
        show_message(UI_STR_WALLET_INVALID_MNEMONIC);
        return;
    }
    wipe_textarea(textarea);
    s_pending_pin_flow = PIN_FLOW_CREATE;
    lv_async_call(async_show_pin, NULL);
}

static void show_import_page(void *unused)
{
    (void)unused;
    style_screen(ui_i18n_get(UI_STR_WALLET_IMPORT));

    lv_obj_t *textarea = lv_textarea_create(lv_screen_active());
    s_sensitive_input = textarea;
    lv_obj_set_size(textarea, 430, 200);
    lv_obj_align(textarea, LV_ALIGN_TOP_MID, 0, WALLET_CONTENT_TOP);
    lv_textarea_set_one_line(textarea, false);
    lv_textarea_set_max_length(textarea, WALLET_STORAGE_MNEMONIC_MAX_LEN - 1);
    lv_textarea_set_placeholder_text(textarea, ui_i18n_get(UI_STR_WALLET_ENTER_MNEMONIC));
    lv_obj_set_style_bg_color(textarea, lv_color_hex(0x202228), 0);
    lv_obj_set_style_text_color(textarea, lv_color_white(), 0);
    lv_obj_set_style_text_font(textarea, &lv_font_montserrat_24, 0);

    lv_obj_t *keyboard = lv_keyboard_create(lv_screen_active());
    lv_obj_set_size(keyboard, WALLET_UI_WIDTH, 250);
    lv_obj_align(keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(keyboard, textarea);
    lv_obj_set_style_text_font(keyboard, &lv_font_montserrat_24, LV_PART_ITEMS);

    lv_obj_t *next = add_button(lv_screen_active(),
                                ui_i18n_get(UI_STR_WALLET_CONTINUE),
                                324, lv_color_hex(0x00A688));
    lv_obj_set_size(next, 432, 62);
    lv_obj_add_event_cb(next, import_continue_event_cb, LV_EVENT_CLICKED, textarea);
}

static void show_import_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_CLICKED) {
        lv_async_call(show_import_page, NULL);
    }
}

void wallet_flow_show_setup(void)
{
    style_screen(ui_i18n_get(UI_STR_WALLET_SET_UP));
    lv_obj_t *create12 = add_button(lv_screen_active(),
                                    ui_i18n_get(UI_STR_WALLET_CREATE_12),
                                    WALLET_CONTENT_TOP, lv_color_hex(0x00A688));
    lv_obj_add_event_cb(create12, create_words_event_cb, LV_EVENT_CLICKED,
                        (void *)(uintptr_t)12);
    lv_obj_t *create24 = add_button(lv_screen_active(),
                                    ui_i18n_get(UI_STR_WALLET_CREATE_24),
                                    WALLET_CONTENT_TOP + WALLET_ITEM_STEP,
                                    lv_color_hex(0x3272B8));
    lv_obj_add_event_cb(create24, create_words_event_cb, LV_EVENT_CLICKED,
                        (void *)(uintptr_t)24);
    lv_obj_t *import = add_button(lv_screen_active(),
                                  ui_i18n_get(UI_STR_WALLET_IMPORT),
                                  WALLET_CONTENT_TOP + WALLET_ITEM_STEP * 2,
                                  lv_color_hex(0x4B4E57));
    lv_obj_add_event_cb(import, show_import_event_cb, LV_EVENT_CLICKED, NULL);
}

static void pin_keypad_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_VALUE_CHANGED || !s_pin_input) {
        return;
    }
    lv_obj_t *keypad = lv_event_get_target_obj(event);
    uint32_t id = lv_buttonmatrix_get_selected_button(keypad);
    const char *key = lv_buttonmatrix_get_button_text(keypad, id);
    if (!key) {
        return;
    }
    if (strcmp(key, LV_SYMBOL_BACKSPACE) == 0) {
        lv_textarea_delete_char(s_pin_input);
        return;
    }
    if (strcmp(key, LV_SYMBOL_OK) != 0) {
        lv_textarea_add_text(s_pin_input, key);
        return;
    }

    const char *pin = lv_textarea_get_text(s_pin_input);
    if (!wallet_storage_pin_is_valid(pin)) {
        lv_label_set_text(s_pin_hint, ui_i18n_get(UI_STR_WALLET_PIN_REQUIREMENTS));
        return;
    }

    if (s_pin_flow == PIN_FLOW_UNLOCK || s_pin_flow == PIN_FLOW_CHANGE_UNLOCK) {
        pin_flow_t flow = s_pin_flow;
        bool started = start_wallet_unlock(pin, flow);
        wipe_textarea(s_pin_input);
        if (!started) {
            lv_label_set_text(s_pin_hint,
                              ui_i18n_get(UI_STR_WALLET_STORAGE_ERROR));
        }
        return;
    }

    if (!s_pin_confirming) {
        memcpy(s_first_pin, pin, strlen(pin) + 1);
        s_pin_confirming = true;
        wipe_textarea(s_pin_input);
        lv_label_set_text(s_pin_hint, ui_i18n_get(UI_STR_WALLET_CONFIRM_PIN));
        return;
    }
    if (strcmp(pin, s_first_pin) != 0) {
        mbedtls_platform_zeroize(s_first_pin, sizeof(s_first_pin));
        s_pin_confirming = false;
        wipe_textarea(s_pin_input);
        lv_label_set_text(s_pin_hint, ui_i18n_get(UI_STR_WALLET_PIN_MISMATCH));
        return;
    }

    esp_err_t ret = s_pin_flow == PIN_FLOW_CREATE
                        ? wallet_service_create(s_pending_mnemonic, pin)
                        : wallet_service_change_pin(pin);
    mbedtls_platform_zeroize(s_first_pin, sizeof(s_first_pin));
    mbedtls_platform_zeroize(s_pending_mnemonic, sizeof(s_pending_mnemonic));
    wipe_textarea(s_pin_input);
    if (ret != ESP_OK) {
        s_pin_confirming = false;
        lv_label_set_text(s_pin_hint, ui_i18n_get(UI_STR_WALLET_STORAGE_ERROR));
        return;
    }
    lv_async_call(async_wallet_home, NULL);
}

static void show_pin_screen(pin_flow_t flow)
{
    s_pin_flow = flow;
    s_pin_confirming = false;
    mbedtls_platform_zeroize(s_first_pin, sizeof(s_first_pin));
    ui_string_id_t title = (flow == PIN_FLOW_UNLOCK || flow == PIN_FLOW_CHANGE_UNLOCK)
                               ? UI_STR_WALLET_ENTER_PIN
                               : UI_STR_WALLET_SET_PIN;
    style_screen(ui_i18n_get(title));

    s_pin_hint = lv_label_create(lv_screen_active());
    lv_label_set_text(s_pin_hint, ui_i18n_get(UI_STR_WALLET_PIN_REQUIREMENTS));
    lv_obj_set_width(s_pin_hint, 420);
    lv_obj_set_style_text_color(s_pin_hint, lv_color_hex(0xC8CAD0), 0);
    lv_obj_set_style_text_font(s_pin_hint, ui_i18n_font_24(), 0);
    lv_obj_align(s_pin_hint, LV_ALIGN_TOP_MID, 0, 136);

    s_pin_input = lv_textarea_create(lv_screen_active());
    lv_obj_set_size(s_pin_input, 410, 70);
    lv_obj_align(s_pin_input, LV_ALIGN_TOP_MID, 0, 174);
    lv_textarea_set_one_line(s_pin_input, true);
    lv_textarea_set_password_mode(s_pin_input, true);
    lv_textarea_set_max_length(s_pin_input, WALLET_STORAGE_PIN_MAX_LEN);
    lv_obj_set_style_bg_color(s_pin_input, lv_color_hex(0x292A32), 0);
    lv_obj_set_style_text_color(s_pin_input, lv_color_white(), 0);
    lv_obj_set_style_text_font(s_pin_input, &lv_font_montserrat_32, 0);

    lv_obj_t *keypad = lv_buttonmatrix_create(lv_screen_active());
    lv_buttonmatrix_set_map(keypad, s_pin_map);
    lv_obj_set_size(keypad, 468, 382);
    lv_obj_align(keypad, LV_ALIGN_BOTTOM_MID, 0, -4);
    lv_obj_set_style_bg_opa(keypad, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(keypad, 0, 0);
    lv_obj_set_style_pad_all(keypad, 4, 0);
    lv_obj_set_style_pad_row(keypad, 4, 0);
    lv_obj_set_style_pad_column(keypad, 4, 0);
    lv_obj_set_style_bg_color(keypad, lv_color_hex(0x292A32), LV_PART_ITEMS);
    lv_obj_set_style_bg_color(keypad, lv_color_hex(0x00A688),
                              LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_text_color(keypad, lv_color_white(), LV_PART_ITEMS);
    lv_obj_set_style_text_font(keypad, &lv_font_montserrat_28, LV_PART_ITEMS);
    lv_obj_add_event_cb(keypad, pin_keypad_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
}

void wallet_flow_show_unlock(void)
{
    show_pin_screen(PIN_FLOW_UNLOCK);
}

static void option_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }
    wallet_action_t action = (wallet_action_t)(uintptr_t)lv_event_get_user_data(event);
    switch (action) {
    case WALLET_ACTION_INITIALIZE:
        lv_async_call(async_show_setup, NULL);
        break;
    case WALLET_ACTION_CONNECT:
        lv_async_call(async_show_unlock, NULL);
        break;
    case WALLET_ACTION_SESSION:
        lv_async_call(async_show_receive, NULL);
        break;
    case WALLET_ACTION_SETTINGS:
        s_pending_pin_flow = wallet_service_get_status() == WALLET_STATUS_READY
                                 ? PIN_FLOW_CHANGE_NEW
                                 : PIN_FLOW_CHANGE_UNLOCK;
        lv_async_call(async_show_pin, NULL);
        break;
    case WALLET_ACTION_EXPORT_XPUB:
        lv_async_call(async_show_xpub, NULL);
        break;
    case WALLET_ACTION_SIGN_PSBT:
        lv_async_call(async_show_psbt_input, NULL);
        break;
    case WALLET_ACTION_USB_JADE:
        if (jade_usb_protocol_is_running()) {
            if (jade_usb_protocol_stop() != ESP_OK) {
                show_message(UI_STR_WALLET_USB_JADE_ERROR);
                break;
            }
        } else if (jade_usb_protocol_start() != ESP_OK) {
            show_message(UI_STR_WALLET_USB_JADE_ERROR);
            break;
        }
        lv_async_call(async_show_options, NULL);
        break;
    default:
        break;
    }
}

static void lock_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_CLICKED) {
        wallet_service_lock();
        lv_async_call(async_wallet_home, NULL);
    }
}

static void delete_confirm_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }
    if (wallet_service_delete() != ESP_OK) {
        show_message(UI_STR_WALLET_STORAGE_ERROR);
        return;
    }
    lv_async_call(async_wallet_home, NULL);
}

static void show_delete_page(void *unused)
{
    (void)unused;
    style_screen(ui_i18n_get(UI_STR_WALLET_DELETE));
    lv_obj_t *warning = lv_label_create(lv_screen_active());
    lv_label_set_text(warning, ui_i18n_get(UI_STR_WALLET_DELETE_WARNING));
    lv_obj_set_width(warning, 420);
    lv_label_set_long_mode(warning, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(warning, lv_color_hex(0xFFCA66), 0);
    lv_obj_set_style_text_font(warning, ui_i18n_font_28(), 0);
    lv_obj_align(warning, LV_ALIGN_TOP_MID, 0, 150);
    lv_obj_t *confirm = add_button(lv_screen_active(), ui_i18n_get(UI_STR_WALLET_DELETE),
                                   380, lv_color_hex(0xB43A3A));
    lv_obj_add_event_cb(confirm, delete_confirm_event_cb, LV_EVENT_CLICKED, NULL);
}

static void show_delete_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_CLICKED) {
        lv_async_call(show_delete_page, NULL);
    }
}

void wallet_flow_show_options(void)
{
    style_screen(ui_i18n_get(UI_STR_WALLET_OPTIONS));
    lv_obj_t *list = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(list);
    lv_obj_set_size(list, WALLET_UI_WIDTH, 500);
    lv_obj_align(list, LV_ALIGN_TOP_MID, 0, 136);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_pad_bottom(list, 14, 0);

    wallet_status_t status = wallet_service_get_status();
    int32_t y = 4;
    if (status == WALLET_STATUS_UNINITIALIZED) {
        lv_obj_t *setup = add_button(list, ui_i18n_get(UI_STR_WALLET_CREATE_NEW),
                                     y, lv_color_hex(0x00A688));
        lv_obj_add_event_cb(setup, option_event_cb, LV_EVENT_CLICKED,
                            (void *)(uintptr_t)WALLET_ACTION_INITIALIZE);
        return;
    }
    if (status == WALLET_STATUS_LOCKED) {
        lv_obj_t *unlock = add_button(list, ui_i18n_get(UI_STR_WALLET_ENTER_PIN),
                                      y, lv_color_hex(0x00A688));
        lv_obj_add_event_cb(unlock, option_event_cb, LV_EVENT_CLICKED,
                            (void *)(uintptr_t)WALLET_ACTION_CONNECT);
        y += WALLET_ITEM_STEP;
    } else if (status == WALLET_STATUS_READY) {
        lv_obj_t *receive = add_button(list, ui_i18n_get(UI_STR_WALLET_RECEIVE),
                                       y, lv_color_hex(0x00A688));
        lv_obj_add_event_cb(receive, option_event_cb, LV_EVENT_CLICKED,
                            (void *)(uintptr_t)WALLET_ACTION_SESSION);
        y += WALLET_ITEM_STEP;
        lv_obj_t *xpub = add_button(list, ui_i18n_get(UI_STR_WALLET_EXPORT_XPUB),
                                   y, lv_color_hex(0x3272B8));
        lv_obj_add_event_cb(xpub, option_event_cb, LV_EVENT_CLICKED,
                            (void *)(uintptr_t)WALLET_ACTION_EXPORT_XPUB);
        y += WALLET_ITEM_STEP;
        lv_obj_t *sign = add_button(list, ui_i18n_get(UI_STR_WALLET_SEND_BITCOIN),
                                   y, lv_color_hex(0x3272B8));
        lv_obj_add_event_cb(sign, option_event_cb, LV_EVENT_CLICKED,
                            (void *)(uintptr_t)WALLET_ACTION_SIGN_PSBT);
        y += WALLET_ITEM_STEP;
        lv_obj_t *usb_jade = add_button(
            list,
            ui_i18n_get(jade_usb_protocol_is_running()
                            ? UI_STR_WALLET_USB_JADE_DISABLE
                            : UI_STR_WALLET_USB_JADE_ENABLE),
            y, jade_usb_protocol_is_running() ? lv_color_hex(0x8E5B32)
                                              : lv_color_hex(0x3272B8));
        lv_obj_add_event_cb(usb_jade, option_event_cb, LV_EVENT_CLICKED,
                            (void *)(uintptr_t)WALLET_ACTION_USB_JADE);
        y += WALLET_ITEM_STEP;
        lv_obj_t *change = add_button(list, ui_i18n_get(UI_STR_WALLET_CHANGE_PIN),
                                      y, lv_color_hex(0x3272B8));
        lv_obj_add_event_cb(change, option_event_cb, LV_EVENT_CLICKED,
                            (void *)(uintptr_t)WALLET_ACTION_SETTINGS);
        y += WALLET_ITEM_STEP;
        lv_obj_t *lock = add_button(list, ui_i18n_get(UI_STR_WALLET_LOCK),
                                    y, lv_color_hex(0x4B4E57));
        lv_obj_add_event_cb(lock, lock_event_cb, LV_EVENT_CLICKED, NULL);
        y += WALLET_ITEM_STEP;
    }
    lv_obj_t *remove = add_button(list, ui_i18n_get(UI_STR_WALLET_DELETE),
                                  y, lv_color_hex(0x8E3434));
    lv_obj_add_event_cb(remove, show_delete_event_cb, LV_EVENT_CLICKED, NULL);
}

static lv_obj_t *add_network_selector(lv_event_cb_t callback,
                                      int32_t y, int32_t height)
{
    static const char *network_map[3];
    network_map[0] = ui_i18n_get(UI_STR_WALLET_MAINNET);
    network_map[1] = ui_i18n_get(UI_STR_WALLET_TESTNET);
    network_map[2] = "";

    lv_obj_t *network = lv_buttonmatrix_create(lv_screen_active());
    lv_buttonmatrix_set_map(network, network_map);
    lv_buttonmatrix_set_one_checked(network, true);
    lv_buttonmatrix_set_button_ctrl_all(network, LV_BUTTONMATRIX_CTRL_CHECKABLE);
    lv_buttonmatrix_set_button_ctrl(network, (uint32_t)s_network,
                                    LV_BUTTONMATRIX_CTRL_CHECKED);
    lv_obj_set_size(network, 432, height);
    lv_obj_align(network, LV_ALIGN_TOP_MID, 0, y);
    lv_obj_set_style_text_font(network, ui_i18n_font_24(), LV_PART_ITEMS);
    lv_obj_add_event_cb(network, callback, LV_EVENT_VALUE_CHANGED, NULL);
    return network;
}

static void xpub_network_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_VALUE_CHANGED) {
        return;
    }
    s_network = lv_buttonmatrix_get_selected_button(lv_event_get_target_obj(event)) == 0
                    ? WALLET_NETWORK_BITCOIN
                    : WALLET_NETWORK_TESTNET;
    lv_async_call(async_show_xpub, NULL);
}

void wallet_flow_show_xpub(void)
{
    style_screen(ui_i18n_get(UI_STR_WALLET_EXPORT_XPUB));
    add_network_selector(xpub_network_event_cb, WALLET_CONTENT_TOP, 64);

    uint32_t account = 0;
    char xpub[WALLET_XPUB_MAX_LEN] = {0};
    esp_err_t ret = wallet_service_get_address_state(s_network, &account, NULL, NULL);
    if (ret == ESP_OK) {
        ret = wallet_service_get_bip84_xpub(s_network, xpub, sizeof(xpub));
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "XPUB export failed: %s", esp_err_to_name(ret));
        show_message(UI_STR_WALLET_XPUB_ERROR);
        return;
    }

    char path[64];
    snprintf(path, sizeof(path), "%s: m/84'/%lu'/%lu'",
             ui_i18n_get(UI_STR_WALLET_XPUB_PATH),
             (unsigned long)(s_network == WALLET_NETWORK_BITCOIN ? 0U : 1U),
             (unsigned long)account);
    lv_obj_t *path_label = lv_label_create(lv_screen_active());
    lv_label_set_text(path_label, path);
    lv_obj_set_width(path_label, 432);
    lv_obj_set_style_text_color(path_label, lv_color_hex(0xC8CAD0), 0);
    lv_obj_set_style_text_font(path_label, ui_i18n_font_24(), 0);
    lv_obj_align(path_label, LV_ALIGN_TOP_MID, 0, 216);

    lv_obj_t *value = lv_label_create(lv_screen_active());
    lv_label_set_text(value, xpub);
    lv_obj_set_size(value, 432, 104);
    lv_label_set_long_mode(value, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_bg_color(value, lv_color_hex(0x202228), 0);
    lv_obj_set_style_bg_opa(value, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(value, 12, 0);
    lv_obj_set_style_radius(value, 6, 0);
    lv_obj_set_style_text_color(value, lv_color_white(), 0);
    lv_obj_set_style_text_font(value, &lv_font_montserrat_16, 0);
    lv_obj_align(value, LV_ALIGN_TOP_MID, 0, 250);

    qrcode_text_config_t config = {
        .size = 230,
        .quiet_zone = 10,
        .dark_color = LV_COLOR_MAKE(0x00, 0x00, 0x00),
        .light_color = LV_COLOR_MAKE(0xFF, 0xFF, 0xFF),
    };
    lv_obj_t *qrcode = qrcode_text_create_with_config(lv_screen_active(), xpub, &config);
    if (qrcode) {
        lv_obj_align(qrcode, LV_ALIGN_TOP_MID, 0, 370);
    }
    mbedtls_platform_zeroize(xpub, sizeof(xpub));
}

static void psbt_network_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_VALUE_CHANGED) {
        return;
    }
    s_network = lv_buttonmatrix_get_selected_button(lv_event_get_target_obj(event)) == 0
                    ? WALLET_NETWORK_BITCOIN
                    : WALLET_NETWORK_TESTNET;
    lv_async_call(async_show_psbt_input, NULL);
}

static void psbt_review_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED || !s_psbt_input) {
        return;
    }
    const char *text = lv_textarea_get_text(s_psbt_input);
    size_t length = strlen(text);
    if (length == 0 || length >= sizeof(s_psbt_base64)) {
        show_message(UI_STR_WALLET_INVALID_PSBT);
        return;
    }
    memcpy(s_psbt_base64, text, length + 1);
    if (wallet_service_inspect_psbt_base64(s_network, s_psbt_base64,
                                           &s_psbt_summary) != ESP_OK) {
        mbedtls_platform_zeroize(s_psbt_base64, sizeof(s_psbt_base64));
        show_message(UI_STR_WALLET_INVALID_PSBT);
        return;
    }
    lv_async_call(async_show_psbt_review, NULL);
}

void wallet_flow_show_psbt_input(void)
{
    mbedtls_platform_zeroize(s_psbt_base64, sizeof(s_psbt_base64));
    mbedtls_platform_zeroize(s_signed_psbt_base64, sizeof(s_signed_psbt_base64));
    memset(&s_psbt_summary, 0, sizeof(s_psbt_summary));
    style_screen(ui_i18n_get(UI_STR_WALLET_SIGN_PSBT));
    add_network_selector(psbt_network_event_cb, WALLET_CONTENT_TOP, 64);

    s_psbt_input = lv_textarea_create(lv_screen_active());
    lv_obj_set_size(s_psbt_input, 432, 150);
    lv_obj_align(s_psbt_input, LV_ALIGN_TOP_MID, 0, 214);
    lv_textarea_set_one_line(s_psbt_input, false);
    lv_textarea_set_max_length(s_psbt_input, WALLET_PSBT_BASE64_MAX_LEN - 1);
    lv_textarea_set_placeholder_text(s_psbt_input,
                                     ui_i18n_get(UI_STR_WALLET_ENTER_PSBT));
    lv_textarea_set_accepted_chars(
        s_psbt_input,
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/=");
    lv_obj_set_style_bg_color(s_psbt_input, lv_color_hex(0x202228), 0);
    lv_obj_set_style_text_color(s_psbt_input, lv_color_white(), 0);
    lv_obj_set_style_text_font(s_psbt_input, &lv_font_montserrat_16, 0);

    lv_obj_t *review = add_button(lv_screen_active(),
                                  ui_i18n_get(UI_STR_WALLET_REVIEW),
                                  372, lv_color_hex(0x00A688));
    lv_obj_set_size(review, 432, 54);
    lv_obj_add_event_cb(review, psbt_review_event_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *keyboard = lv_keyboard_create(lv_screen_active());
    lv_obj_set_size(keyboard, WALLET_UI_WIDTH, 205);
    lv_obj_align(keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(keyboard, s_psbt_input);
    lv_obj_set_style_text_font(keyboard, &lv_font_montserrat_24, LV_PART_ITEMS);
}

static void format_btc(uint64_t satoshi, char *text, size_t text_size)
{
    snprintf(text, text_size, "%llu.%08llu BTC",
             (unsigned long long)(satoshi / UINT64_C(100000000)),
             (unsigned long long)(satoshi % UINT64_C(100000000)));
}

static void append_text(char *buffer, size_t buffer_size, size_t *used,
                        const char *format, ...)
{
    if (*used >= buffer_size) {
        return;
    }
    va_list args;
    va_start(args, format);
    int written = vsnprintf(buffer + *used, buffer_size - *used, format, args);
    va_end(args);
    if (written < 0) {
        return;
    }
    size_t available = buffer_size - *used;
    *used += (size_t)written < available ? (size_t)written : available - 1;
}

static void psbt_confirm_sign_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }
    if (wallet_service_sign_psbt_base64(
            s_network, s_psbt_base64, &s_psbt_summary,
            s_signed_psbt_base64, sizeof(s_signed_psbt_base64)) != ESP_OK) {
        show_message(UI_STR_WALLET_INVALID_PSBT);
        return;
    }
    lv_async_call(async_show_signed_psbt, NULL);
}

static void wallet_flow_show_psbt_review(void)
{
    style_screen(ui_i18n_get(UI_STR_WALLET_REVIEW_PSBT));

    lv_obj_t *content = lv_obj_create(lv_screen_active());
    lv_obj_set_size(content, 448, 390);
    lv_obj_align(content, LV_ALIGN_TOP_MID, 0, 134);
    lv_obj_set_style_bg_color(content, lv_color_hex(0x15171C), 0);
    lv_obj_set_style_border_width(content, 0, 0);
    lv_obj_set_style_radius(content, 6, 0);
    lv_obj_set_style_pad_all(content, 14, 0);
    lv_obj_set_scroll_dir(content, LV_DIR_VER);

    char send[32], change[32], fee[32];
    format_btc(s_psbt_summary.external_amount, send, sizeof(send));
    format_btc(s_psbt_summary.change_amount, change, sizeof(change));
    format_btc(s_psbt_summary.fee_amount, fee, sizeof(fee));

    char review[2048] = {0};
    size_t used = 0;
    append_text(review, sizeof(review), &used, "%s %u   %s %u   %s %u\n",
                ui_i18n_get(UI_STR_WALLET_INPUTS),
                (unsigned)s_psbt_summary.input_count,
                ui_i18n_get(UI_STR_WALLET_OUTPUTS),
                (unsigned)s_psbt_summary.output_count,
                ui_i18n_get(UI_STR_WALLET_SIGNING_INPUTS),
                (unsigned)s_psbt_summary.signing_input_count);
    append_text(review, sizeof(review), &used, "%s: %s\n%s: %s\n%s: %s\n\n",
                ui_i18n_get(UI_STR_WALLET_SEND_AMOUNT), send,
                ui_i18n_get(UI_STR_WALLET_CHANGE_AMOUNT), change,
                ui_i18n_get(UI_STR_WALLET_FEE), fee);
    for (size_t i = 0; i < s_psbt_summary.external_output_count; ++i) {
        char amount[32];
        format_btc(s_psbt_summary.external_outputs[i].amount,
                   amount, sizeof(amount));
        append_text(review, sizeof(review), &used, "#%u  %s\n%s\n\n",
                    (unsigned)(i + 1), amount,
                    s_psbt_summary.external_outputs[i].address);
    }

    lv_obj_t *label = lv_label_create(content);
    lv_label_set_text(label, review);
    lv_obj_set_width(label, 410);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_set_style_text_font(label, ui_i18n_font_24(), 0);
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *confirm = add_button(lv_screen_active(),
                                   ui_i18n_get(UI_STR_WALLET_CONFIRM_SIGN),
                                   538, lv_color_hex(0x00A688));
    lv_obj_add_event_cb(confirm, psbt_confirm_sign_event_cb,
                        LV_EVENT_CLICKED, NULL);
}

static void show_signed_qr_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_CLICKED) {
        lv_async_call(async_show_signed_psbt_qr, NULL);
    }
}

static void wallet_flow_show_signed_psbt(void)
{
    style_screen(ui_i18n_get(UI_STR_WALLET_SIGNED_PSBT));
    lv_obj_t *value = lv_textarea_create(lv_screen_active());
    lv_obj_set_size(value, 432, 380);
    lv_obj_align(value, LV_ALIGN_TOP_MID, 0, WALLET_CONTENT_TOP);
    lv_textarea_set_one_line(value, false);
    lv_textarea_set_text(value, s_signed_psbt_base64);
    lv_obj_remove_flag(value, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_set_style_bg_color(value, lv_color_hex(0x202228), 0);
    lv_obj_set_style_text_color(value, lv_color_white(), 0);
    lv_obj_set_style_text_font(value, &lv_font_montserrat_16, 0);

    lv_obj_t *qr = add_button(lv_screen_active(),
                              ui_i18n_get(UI_STR_WALLET_SHOW_QR),
                              536, lv_color_hex(0x3272B8));
    lv_obj_add_event_cb(qr, show_signed_qr_event_cb, LV_EVENT_CLICKED, NULL);
}

static void wallet_flow_show_signed_psbt_qr(void)
{
    style_screen(ui_i18n_get(UI_STR_WALLET_SIGNED_PSBT));
    qrcode_text_config_t config = {
        .size = 400,
        .quiet_zone = 12,
        .dark_color = LV_COLOR_MAKE(0x00, 0x00, 0x00),
        .light_color = LV_COLOR_MAKE(0xFF, 0xFF, 0xFF),
    };
    lv_obj_t *qrcode = qrcode_text_create_with_config(
        lv_screen_active(), s_signed_psbt_base64, &config);
    if (qrcode) {
        lv_obj_align(qrcode, LV_ALIGN_TOP_MID, 0, WALLET_CONTENT_TOP);
    } else {
        lv_obj_t *error = lv_label_create(lv_screen_active());
        lv_label_set_text(error, ui_i18n_get(UI_STR_WALLET_QR_TOO_LARGE));
        lv_obj_set_width(error, 420);
        lv_label_set_long_mode(error, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_color(error, lv_color_hex(0xFFCA66), 0);
        lv_obj_set_style_text_font(error, ui_i18n_font_24(), 0);
        lv_obj_align(error, LV_ALIGN_TOP_MID, 0, 190);
    }
}

static void network_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_VALUE_CHANGED) {
        return;
    }
    lv_obj_t *control = lv_event_get_target_obj(event);
    s_network = lv_buttonmatrix_get_selected_button(control) == 0
                    ? WALLET_NETWORK_BITCOIN
                    : WALLET_NETWORK_TESTNET;
    lv_async_call(async_refresh_receive, NULL);
}

static lv_obj_t *add_receive_button(const char *text, int32_t x, int32_t width,
                                    lv_event_cb_t callback, bool enabled)
{
    lv_obj_t *button = lv_button_create(lv_screen_active());
    lv_obj_set_size(button, width, 76);
    lv_obj_align(button, LV_ALIGN_TOP_LEFT, x, 458);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x3272B8), 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x3A3D45), LV_STATE_PRESSED);
    lv_obj_set_style_radius(button, 8, 0);
    lv_obj_set_style_shadow_width(button, 0, 0);
    if (!enabled) {
        lv_obj_add_state(button, LV_STATE_DISABLED);
        lv_obj_set_style_opa(button, LV_OPA_40, LV_STATE_DISABLED);
    }
    lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, NULL);

    lv_obj_t *label = lv_label_create(button);
    lv_label_set_text(label, text);
    lv_obj_set_width(label, width - 20);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_set_style_text_font(label,
                               text[0] == LV_SYMBOL_LEFT[0] || text[0] == LV_SYMBOL_RIGHT[0]
                                   ? &lv_font_montserrat_28
                                   : ui_i18n_font_24(),
                               0);
    lv_obj_center(label);
    return button;
}

static void previous_address_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }
    uint32_t network_index = (uint32_t)s_network;
    if (s_receive_display_index[network_index] > 0U) {
        s_receive_display_index[network_index]--;
        lv_async_call(async_refresh_receive, NULL);
    }
}

static void next_address_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }
    uint32_t current_index = 0;
    if (wallet_service_get_address_state(s_network, NULL, &current_index, NULL) != ESP_OK) {
        show_message(UI_STR_WALLET_STORAGE_ERROR);
        return;
    }
    uint32_t network_index = (uint32_t)s_network;
    if (s_receive_display_index[network_index] < current_index) {
        s_receive_display_index[network_index]++;
        lv_async_call(async_refresh_receive, NULL);
    }
}

static void new_address_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }
    uint32_t new_index = 0;
    if (wallet_service_advance_receive_index(s_network, &new_index) != ESP_OK) {
        show_message(UI_STR_WALLET_STORAGE_ERROR);
        return;
    }
    uint32_t network_index = (uint32_t)s_network;
    s_receive_display_index[network_index] = new_index;
    s_receive_display_valid[network_index] = true;
    lv_async_call(async_refresh_receive, NULL);
}

static void close_qr_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_CLICKED) {
        lv_obj_delete((lv_obj_t *)lv_event_get_user_data(event));
    }
}

static void address_qr_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }
    uint32_t account = 0;
    uint32_t network_index = (uint32_t)s_network;
    char address[WALLET_PSBT_ADDRESS_MAX_LEN] = {0};
    if (wallet_service_get_address_state(s_network, &account, NULL, NULL) != ESP_OK ||
        wallet_service_get_address(s_network, account, 0,
                                   s_receive_display_index[network_index],
                                   address, sizeof(address)) != ESP_OK) {
        show_message(UI_STR_WALLET_STORAGE_ERROR);
        return;
    }

    lv_obj_t *overlay = lv_obj_create(lv_screen_active());
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_size(overlay, 464, 570);
    lv_obj_center(overlay);
    lv_obj_set_style_bg_color(overlay, lv_color_hex(0x101216), 0);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(overlay, lv_color_hex(0x00A688), 0);
    lv_obj_set_style_border_width(overlay, 2, 0);
    lv_obj_set_style_radius(overlay, 7, 0);
    lv_obj_clear_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(overlay);
    lv_label_set_text(title, ui_i18n_get(UI_STR_WALLET_ADDRESS_QR));
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_set_style_text_font(title, ui_i18n_font_28(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 8, 2);

    lv_obj_t *close = lv_button_create(overlay);
    lv_obj_set_size(close, 64, 64);
    lv_obj_align(close, LV_ALIGN_TOP_RIGHT, -2, -8);
    lv_obj_set_style_bg_color(close, lv_color_hex(0x3A3D45), 0);
    lv_obj_set_style_radius(close, 7, 0);
    lv_obj_add_event_cb(close, close_qr_event_cb, LV_EVENT_CLICKED, overlay);
    lv_obj_t *close_icon = lv_label_create(close);
    lv_label_set_text(close_icon, LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_font(close_icon, &lv_font_montserrat_28, 0);
    lv_obj_center(close_icon);

    qrcode_text_config_t config = {
        .size = 350,
        .quiet_zone = 12,
        .dark_color = LV_COLOR_MAKE(0x00, 0x00, 0x00),
        .light_color = LV_COLOR_MAKE(0xFF, 0xFF, 0xFF),
    };
    lv_obj_t *qrcode = qrcode_text_create_with_config(overlay, address, &config);
    if (qrcode) {
        lv_obj_align(qrcode, LV_ALIGN_TOP_MID, 0, 72);
    }
    lv_obj_t *value = lv_label_create(overlay);
    lv_label_set_text(value, address);
    lv_obj_set_width(value, 420);
    lv_label_set_long_mode(value, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(value, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(value, lv_color_white(), 0);
    lv_obj_set_style_text_font(value, &lv_font_montserrat_16, 0);
    lv_obj_align(value, LV_ALIGN_BOTTOM_MID, 0, -16);
    lv_display_trigger_activity(lv_obj_get_display(overlay));
    mbedtls_platform_zeroize(address, sizeof(address));
}

static void wallet_flow_render_receive(void)
{
    style_screen(ui_i18n_get(UI_STR_WALLET_RECEIVE));
    uint32_t account = 0;
    uint32_t current_index = 0;
    esp_err_t ret = wallet_service_get_address_state(s_network, &account,
                                                      &current_index, NULL);
    if (ret != ESP_OK) {
        show_message(UI_STR_WALLET_STORAGE_ERROR);
        return;
    }

    uint32_t network_index = (uint32_t)s_network;
    if (!s_receive_display_valid[network_index] ||
        s_receive_display_index[network_index] > current_index) {
        s_receive_display_index[network_index] = current_index;
        s_receive_display_valid[network_index] = true;
    }
    uint32_t display_index = s_receive_display_index[network_index];

    char address[96] = {0};
    ret = wallet_service_get_address(s_network, account, 0, display_index,
                                     address, sizeof(address));
    if (ret != ESP_OK) {
        show_message(UI_STR_WALLET_WRONG_PIN);
        return;
    }

    static const char *network_map[3];
    network_map[0] = ui_i18n_get(UI_STR_WALLET_MAINNET);
    network_map[1] = ui_i18n_get(UI_STR_WALLET_TESTNET);
    network_map[2] = "";
    lv_obj_t *network = lv_buttonmatrix_create(lv_screen_active());
    lv_buttonmatrix_set_map(network, network_map);
    lv_buttonmatrix_set_one_checked(network, true);
    lv_buttonmatrix_set_button_ctrl_all(network, LV_BUTTONMATRIX_CTRL_CHECKABLE);
    lv_buttonmatrix_set_button_ctrl(network, (uint32_t)s_network,
                                    LV_BUTTONMATRIX_CTRL_CHECKED);
    lv_obj_set_size(network, 432, 72);
    lv_obj_align(network, LV_ALIGN_TOP_MID, 0, WALLET_CONTENT_TOP);
    lv_obj_set_style_text_font(network, ui_i18n_font_24(), LV_PART_ITEMS);
    lv_obj_add_event_cb(network, network_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *caption = lv_label_create(lv_screen_active());
    lv_label_set_text(caption, ui_i18n_get(UI_STR_WALLET_ADDRESS));
    lv_obj_set_style_text_color(caption, lv_color_hex(0xAEB1B8), 0);
    lv_obj_set_style_text_font(caption, ui_i18n_font_24(), 0);
    lv_obj_align(caption, LV_ALIGN_TOP_LEFT, 24, 224);

    char path[64];
    snprintf(path, sizeof(path), "m/84'/%lu'/%lu'/0/%lu",
             (unsigned long)(s_network == WALLET_NETWORK_BITCOIN ? 0U : 1U),
             (unsigned long)account, (unsigned long)display_index);
    lv_obj_t *path_label = lv_label_create(lv_screen_active());
    lv_label_set_text(path_label, path);
    lv_obj_set_style_text_color(path_label, lv_color_hex(0xAEB1B8), 0);
    lv_obj_set_style_text_font(path_label, &lv_font_montserrat_16, 0);
    lv_obj_align(path_label, LV_ALIGN_TOP_RIGHT, -24, 231);

    char position[64];
    snprintf(position, sizeof(position), ui_i18n_get(UI_STR_WALLET_ADDRESS_POSITION),
             (unsigned long)display_index, (unsigned long)current_index);
    lv_obj_t *position_label = lv_label_create(lv_screen_active());
    lv_label_set_text(position_label, position);
    lv_obj_set_style_text_color(position_label, lv_color_hex(0xD5D8DE), 0);
    lv_obj_set_style_text_font(position_label, ui_i18n_font_24(), 0);
    lv_obj_align(position_label, LV_ALIGN_TOP_LEFT, 24, 260);

    lv_obj_t *value = lv_label_create(lv_screen_active());
    lv_label_set_text(value, address);
    lv_obj_set_size(value, 432, 144);
    lv_label_set_long_mode(value, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_bg_color(value, lv_color_hex(0x202228), 0);
    lv_obj_set_style_bg_opa(value, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(value, 18, 0);
    lv_obj_set_style_radius(value, 5, 0);
    lv_obj_set_style_text_color(value, lv_color_white(), 0);
    lv_obj_set_style_text_font(value, &lv_font_montserrat_24, 0);
    lv_obj_align(value, LV_ALIGN_TOP_MID, 0, 296);

    add_receive_button(LV_SYMBOL_LEFT, 16, 72, previous_address_event_cb,
                       display_index > 0U);
    add_receive_button(LV_SYMBOL_RIGHT, 96, 72, next_address_event_cb,
                       display_index < current_index);
    add_receive_button(ui_i18n_get(UI_STR_WALLET_SHOW_QR), 176, 112,
                       address_qr_event_cb, true);
    lv_obj_t *new_address = add_receive_button(ui_i18n_get(UI_STR_WALLET_NEW_ADDRESS),
                                               296, 168, new_address_event_cb,
                                               current_index < WALLET_ADDRESS_INDEX_MAX);
    lv_obj_set_style_bg_color(new_address, lv_color_hex(0x00A688), 0);
    mbedtls_platform_zeroize(address, sizeof(address));
}

void wallet_flow_show_receive(void)
{
    memset(s_receive_display_valid, 0, sizeof(s_receive_display_valid));
    wallet_flow_render_receive();
}

#include "browser_autofill/browser_confirm.h"

#include <stdatomic.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "ui/i18n/ui_i18n.h"

#define CONFIRM_QUEUE_DEPTH 2
#define CONFIRM_DETAIL_MAX 160
#define CONFIRM_DIALOG_WIDTH 420
#define CONFIRM_DIALOG_HEIGHT 280

typedef struct {
    browser_confirm_kind_t kind;
    uint32_t generation;
    char detail[CONFIRM_DETAIL_MAX];
} browser_confirm_message_t;

static const char *TAG = "browser_confirm";
static QueueHandle_t s_queue;
static SemaphoreHandle_t s_request_mutex;
static SemaphoreHandle_t s_result_sem;
static lv_timer_t *s_timer;
static lv_obj_t *s_dialog;
static uint32_t s_dialog_generation;
static atomic_uint s_generation;
static atomic_uint s_active_generation;
static atomic_bool s_pending;
static bool s_result;

static void finish_request(bool approved)
{
    uint32_t active =
        atomic_load_explicit(&s_active_generation, memory_order_acquire);
    if (!atomic_load_explicit(&s_pending, memory_order_acquire) ||
        s_dialog_generation != active) {
        return;
    }

    s_result = approved;
    atomic_store_explicit(&s_pending, false, memory_order_release);
    xSemaphoreGive(s_result_sem);
}

static void dialog_delete_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_DELETE ||
        lv_event_get_target(event) != s_dialog) {
        return;
    }
    s_dialog = NULL;
    finish_request(false);
}

static void approve_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_CLICKED) {
        finish_request(true);
        if (s_dialog) {
            lv_obj_delete(s_dialog);
        }
    }
}

static void deny_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_CLICKED) {
        finish_request(false);
        if (s_dialog) {
            lv_obj_delete(s_dialog);
        }
    }
}

static lv_obj_t *add_button(lv_obj_t *parent,
                            const char *text,
                            lv_align_t align,
                            int32_t x,
                            lv_color_t color)
{
    lv_obj_t *button = lv_button_create(parent);
    lv_obj_set_size(button, 160, 56);
    lv_obj_align(button, align, x, -18);
    lv_obj_set_style_bg_color(button, color, 0);
    lv_obj_set_style_radius(button, 6, 0);

    lv_obj_t *label = lv_label_create(button);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_set_style_text_font(label, ui_i18n_font_28(), 0);
    lv_obj_center(label);
    return button;
}

static void show_dialog(const browser_confirm_message_t *message)
{
    if (s_dialog || !message) {
        return;
    }

    lv_obj_t *screen = lv_screen_active();
    if (!screen) {
        finish_request(false);
        return;
    }

    s_dialog_generation = message->generation;
    s_dialog = lv_obj_create(screen);
    if (!s_dialog) {
        finish_request(false);
        return;
    }
    lv_obj_add_event_cb(s_dialog, dialog_delete_event_cb, LV_EVENT_DELETE, NULL);
    lv_obj_set_size(s_dialog, CONFIRM_DIALOG_WIDTH, CONFIRM_DIALOG_HEIGHT);
    lv_obj_center(s_dialog);
    lv_obj_set_style_bg_color(s_dialog, lv_color_hex(0x20232B), 0);
    lv_obj_set_style_bg_opa(s_dialog, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_dialog, lv_color_hex(0x279FE8), 0);
    lv_obj_set_style_border_width(s_dialog, 2, 0);
    lv_obj_set_style_radius(s_dialog, 8, 0);
    lv_obj_set_style_pad_all(s_dialog, 18, 0);
    lv_obj_add_flag(s_dialog, LV_OBJ_FLAG_FLOATING);
    lv_obj_clear_flag(s_dialog, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(s_dialog);
    lv_label_set_text(title, message->kind == BROWSER_CONFIRM_PAIR
                                 ? "Pair ChromeKeePass"
                                 : "Allow password access");
    lv_obj_set_width(title, CONFIRM_DIALOG_WIDTH - 40);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_set_style_text_font(title, ui_i18n_font_32(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 2, 0);

    lv_obj_t *detail = lv_label_create(s_dialog);
    lv_label_set_text(detail, message->detail);
    lv_obj_set_width(detail, CONFIRM_DIALOG_WIDTH - 40);
    lv_label_set_long_mode(detail, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(detail, lv_color_hex(0xD8DCE6), 0);
    lv_obj_set_style_text_font(detail, ui_i18n_font_24(), 0);
    lv_obj_align(detail, LV_ALIGN_TOP_LEFT, 2, 58);

    lv_obj_t *deny = add_button(s_dialog, "Deny", LV_ALIGN_BOTTOM_LEFT,
                                2, lv_color_hex(0x4B5263));
    lv_obj_add_event_cb(deny, deny_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *approve = add_button(s_dialog, "Allow", LV_ALIGN_BOTTOM_RIGHT,
                                   -2, lv_color_hex(0x197FC7));
    lv_obj_add_event_cb(approve, approve_event_cb, LV_EVENT_CLICKED, NULL);

    lv_display_trigger_activity(lv_obj_get_display(s_dialog));
}

static void browser_confirm_timer_cb(lv_timer_t *timer)
{
    (void)timer;

    if (s_dialog &&
        (!atomic_load_explicit(&s_pending, memory_order_acquire) ||
         s_dialog_generation !=
             atomic_load_explicit(&s_active_generation, memory_order_acquire))) {
        lv_obj_delete(s_dialog);
    }

    browser_confirm_message_t message;
    while (xQueueReceive(s_queue, &message, 0) == pdTRUE) {
        if (atomic_load_explicit(&s_pending, memory_order_acquire) &&
            message.generation ==
                atomic_load_explicit(&s_active_generation, memory_order_acquire)) {
            show_dialog(&message);
        }
    }
}

void browser_confirm_init(void)
{
    if (!s_queue) {
        s_queue = xQueueCreate(CONFIRM_QUEUE_DEPTH,
                               sizeof(browser_confirm_message_t));
    }
    if (!s_request_mutex) {
        s_request_mutex = xSemaphoreCreateMutex();
    }
    if (!s_result_sem) {
        s_result_sem = xSemaphoreCreateBinary();
    }
    if (!s_timer && s_queue && s_request_mutex && s_result_sem) {
        s_timer = lv_timer_create(browser_confirm_timer_cb, 20, NULL);
    }
    if (!s_queue || !s_request_mutex || !s_result_sem || !s_timer) {
        ESP_LOGE(TAG, "Failed to initialize browser confirmation");
    }
}

bool browser_confirm_request(browser_confirm_kind_t kind,
                             const char *detail,
                             uint32_t timeout_ms)
{
    if (!s_queue || !s_request_mutex || !s_result_sem || !detail ||
        timeout_ms == 0) {
        return false;
    }
    if (xSemaphoreTake(s_request_mutex, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        return false;
    }

    while (xSemaphoreTake(s_result_sem, 0) == pdTRUE) {
    }
    uint32_t generation =
        atomic_fetch_add_explicit(&s_generation, 1, memory_order_acq_rel) + 1;
    atomic_store_explicit(&s_active_generation, generation,
                          memory_order_release);
    atomic_store_explicit(&s_pending, true, memory_order_release);
    s_result = false;

    browser_confirm_message_t message = {
        .kind = kind,
        .generation = generation,
    };
    strlcpy(message.detail, detail, sizeof(message.detail));

    bool approved = false;
    if (xQueueSend(s_queue, &message, 0) == pdTRUE &&
        xSemaphoreTake(s_result_sem, pdMS_TO_TICKS(timeout_ms)) == pdTRUE) {
        approved = s_result;
    } else {
        atomic_store_explicit(&s_pending, false, memory_order_release);
    }

    atomic_store_explicit(&s_pending, false, memory_order_release);
    xSemaphoreGive(s_request_mutex);
    return approved;
}

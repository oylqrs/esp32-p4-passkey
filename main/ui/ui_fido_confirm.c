#include <stdbool.h>
#include <stdint.h>

#include "canokey_esp32p4.h"
#include "device.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "lvgl.h"

#define FIDO_CONFIRM_WIDTH  392
#define FIDO_CONFIRM_HEIGHT 230

static const char *TAG = "ui_fido";
static lv_obj_t *s_dialog;
static bool s_dialog_pending;
static uint8_t s_dialog_cbor_cmd;
static QueueHandle_t s_fido_ui_queue;
static lv_timer_t *s_fido_ui_timer;

typedef enum {
    FIDO_UI_CMD_SHOW_CONFIRM,
} fido_ui_cmd_t;

static const char *fido_confirm_title(uint8_t cbor_cmd)
{
    switch (cbor_cmd) {
    case 0x01:
        return "FIDO Registration";
    case 0x02:
        return "FIDO Login";
    case 0x0A:
        return "FIDO Selection";
    default:
        return "FIDO Confirm";
    }
}

static const char *fido_confirm_message(uint8_t cbor_cmd)
{
    switch (cbor_cmd) {
    case 0x01:
        return "Create a new passkey";
    case 0x02:
        return "Confirm sign-in";
    case 0x0A:
        return "Select this device";
    default:
        return "Confirm FIDO request";
    }
}

static void close_dialog(void)
{
    if (s_dialog) {
        ESP_LOGI(TAG, "close FIDO confirm dialog");
        lv_obj_delete(s_dialog);
        s_dialog = NULL;
    }
    s_dialog_pending = false;
}

static void confirm_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }

    set_touch_result(TOUCH_SHORT);
    ESP_LOGW(TAG, "FIDO user presence confirmed: set TOUCH_SHORT");
    close_dialog();
}

static void cancel_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }

    ESP_LOGW(TAG, "FIDO user presence canceled by UI");
    close_dialog();
}

static lv_obj_t *add_button(lv_obj_t *parent, const char *text, lv_align_t align, int32_t x, lv_color_t bg)
{
    lv_obj_t *button = lv_button_create(parent);
    lv_obj_set_size(button, 150, 54);
    lv_obj_align(button, align, x, -22);
    lv_obj_set_style_bg_color(button, bg, 0);
    lv_obj_set_style_radius(button, 6, 0);

    lv_obj_t *label = lv_label_create(button);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_28, 0);
    lv_obj_center(label);

    return button;
}

static void show_fido_confirm_ui(void)
{
    if (s_dialog) {
        ESP_LOGW(TAG, "skip FIDO confirm dialog: already visible");
        return;
    }

    lv_obj_t *screen = lv_screen_active();
    if (!screen) {
        ESP_LOGE(TAG, "cannot show FIDO confirm dialog: no active screen");
        s_dialog_pending = false;
        return;
    }

    ESP_LOGW(TAG, "show FIDO confirm dialog");
    s_dialog = lv_obj_create(screen);
    lv_obj_set_size(s_dialog, FIDO_CONFIRM_WIDTH, FIDO_CONFIRM_HEIGHT);
    lv_obj_center(s_dialog);
    lv_obj_set_style_bg_color(s_dialog, lv_color_hex(0x20232B), 0);
    lv_obj_set_style_bg_opa(s_dialog, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_dialog, 2, 0);
    lv_obj_set_style_border_color(s_dialog, lv_color_hex(0xB8A8FF), 0);
    lv_obj_set_style_radius(s_dialog, 8, 0);
    lv_obj_set_style_pad_all(s_dialog, 18, 0);
    lv_obj_add_flag(s_dialog, LV_OBJ_FLAG_FLOATING);

    lv_obj_t *title = lv_label_create(s_dialog);
    lv_label_set_text(title, fido_confirm_title(s_dialog_cbor_cmd));
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 4, 2);

    lv_obj_t *message = lv_label_create(s_dialog);
    lv_label_set_text(message, fido_confirm_message(s_dialog_cbor_cmd));
    lv_obj_set_width(message, FIDO_CONFIRM_WIDTH - 44);
    lv_obj_set_style_text_color(message, lv_color_hex(0xD8DCE6), 0);
    lv_obj_set_style_text_font(message, &lv_font_montserrat_28, 0);
    lv_obj_align(message, LV_ALIGN_TOP_LEFT, 4, 62);

    lv_obj_t *cancel = add_button(s_dialog, "Cancel", LV_ALIGN_BOTTOM_LEFT, 4, lv_color_hex(0x4B5263));
    lv_obj_add_event_cb(cancel, cancel_event_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *confirm = add_button(s_dialog, "Confirm", LV_ALIGN_BOTTOM_RIGHT, -4, lv_color_hex(0x5D6CFF));
    lv_obj_add_event_cb(confirm, confirm_event_cb, LV_EVENT_CLICKED, NULL);

    s_dialog_pending = false;
    ESP_LOGI(TAG, "FIDO confirm dialog ready");
}

static void fido_ui_timer_cb(lv_timer_t *timer)
{
    (void)timer;

    if (!s_fido_ui_queue) {
        return;
    }

    fido_ui_cmd_t cmd;
    while (xQueueReceive(s_fido_ui_queue, &cmd, 0) == pdTRUE) {
        if (cmd == FIDO_UI_CMD_SHOW_CONFIRM) {
            show_fido_confirm_ui();
        }
    }
}

static bool post_fido_ui_cmd(fido_ui_cmd_t cmd)
{
    if (!s_fido_ui_queue) {
        ESP_LOGW(TAG, "FIDO UI queue is not ready");
        return false;
    }

    if (xQueueSend(s_fido_ui_queue, &cmd, 0) != pdTRUE) {
        ESP_LOGW(TAG, "FIDO UI queue full");
        return false;
    }

    return true;
}

static void ui_fido_confirm_request(uint8_t entry)
{
    ESP_LOGW(TAG, "user presence request entry=%u dialog=%u pending=%u",
             entry,
             s_dialog ? 1 : 0,
             s_dialog_pending ? 1 : 0);

    if (entry != WAIT_ENTRY_CTAPHID) {
        ESP_LOGW(TAG, "ignore user presence request: unsupported entry=%u", entry);
        return;
    }

    if (s_dialog || s_dialog_pending) {
        ESP_LOGW(TAG, "ignore user presence request: dialog already active");
        return;
    }

    s_dialog_cbor_cmd = canokey_esp32p4_get_current_ctap_cbor_cmd();
    s_dialog_pending = true;
    ESP_LOGI(TAG, "schedule FIDO confirm dialog cmd=0x%02x title=%s",
             s_dialog_cbor_cmd,
             fido_confirm_title(s_dialog_cbor_cmd));
    if (!post_fido_ui_cmd(FIDO_UI_CMD_SHOW_CONFIRM)) {
        s_dialog_pending = false;
    }
}

void ui_fido_confirm_init(void)
{
    if (!s_fido_ui_queue) {
        s_fido_ui_queue = xQueueCreate(4, sizeof(fido_ui_cmd_t));
        if (!s_fido_ui_queue) {
            ESP_LOGW(TAG, "Failed to create FIDO UI queue");
            return;
        }
    }

    if (!s_fido_ui_timer) {
        s_fido_ui_timer = lv_timer_create(fido_ui_timer_cb, 20, NULL);
        if (!s_fido_ui_timer) {
            ESP_LOGW(TAG, "Failed to create FIDO UI timer");
            return;
        }
    }

    canokey_esp32p4_set_user_presence_request_callback(ui_fido_confirm_request);
    ESP_LOGI(TAG, "FIDO confirm callback registered");
}

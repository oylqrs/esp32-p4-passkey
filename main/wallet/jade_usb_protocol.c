/* Minimal Jade-compatible CBOR RPC transport over TinyUSB CDC ACM. */
#include "jade_usb_protocol.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cbor.h>
#include "esp_app_desc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "mbedtls/platform_util.h"
#include "tinyusb_cdc_acm.h"
#include "ui/i18n/ui_i18n.h"
#include "usb/usb_msc.h"
#include "wallet_service.h"
#include <wally_core.h>

#define JADE_RPC_BUFFER_SIZE 8192
#define JADE_RX_STREAM_SIZE 12288
#define JADE_TASK_STACK_SIZE 12288
#define JADE_CONFIRM_TIMEOUT_MS 120000
#define JADE_STOP_TIMEOUT_MS 5000
#define JADE_CONFIRM_DETAIL_MAX 1024

typedef struct {
    uint32_t generation;
    char detail[JADE_CONFIRM_DETAIL_MAX];
} jade_confirm_message_t;

typedef enum {
    RPC_RESULT_UINT,
    RPC_RESULT_BOOL,
    RPC_RESULT_TEXT,
    RPC_RESULT_BYTES,
    RPC_RESULT_VERSION,
} rpc_result_type_t;

typedef struct {
    rpc_result_type_t type;
    union {
        uint64_t uint_value;
        bool bool_value;
        const char *text;
        struct {
            const uint8_t *data;
            size_t size;
        } bytes;
    } value;
} rpc_result_t;

static const char *TAG = "jade_usb";
static StreamBufferHandle_t s_rx_stream;
static TaskHandle_t s_task;
static atomic_bool s_running;
static bool s_cdc_initialized;

static QueueHandle_t s_confirm_queue;
static SemaphoreHandle_t s_confirm_mutex;
static SemaphoreHandle_t s_confirm_result_sem;
static lv_timer_t *s_confirm_timer;
static lv_obj_t *s_confirm_dialog;
static atomic_uint s_confirm_generation;
static atomic_uint s_confirm_active_generation;
static atomic_bool s_confirm_pending;
static uint32_t s_dialog_generation;
static bool s_confirm_result;

static void finish_confirmation(bool approved)
{
    uint32_t active = atomic_load_explicit(&s_confirm_active_generation,
                                           memory_order_acquire);
    if (!atomic_load_explicit(&s_confirm_pending, memory_order_acquire) ||
        s_dialog_generation != active) {
        return;
    }
    s_confirm_result = approved;
    atomic_store_explicit(&s_confirm_pending, false, memory_order_release);
    xSemaphoreGive(s_confirm_result_sem);
}

static void confirm_dialog_delete_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_DELETE &&
        lv_event_get_target(event) == s_confirm_dialog) {
        s_confirm_dialog = NULL;
        finish_confirmation(false);
    }
}

static void confirm_button_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }
    finish_confirmation((bool)(uintptr_t)lv_event_get_user_data(event));
    if (s_confirm_dialog) {
        lv_obj_delete(s_confirm_dialog);
    }
}

static lv_obj_t *add_dialog_button(lv_obj_t *parent, const char *text,
                                   lv_align_t align, lv_color_t color,
                                   bool approved)
{
    lv_obj_t *button = lv_button_create(parent);
    lv_obj_set_size(button, 176, 64);
    lv_obj_align(button, align, approved ? -2 : 2, -14);
    lv_obj_set_style_bg_color(button, color, 0);
    lv_obj_set_style_radius(button, 7, 0);
    lv_obj_add_event_cb(button, confirm_button_cb, LV_EVENT_CLICKED,
                        (void *)(uintptr_t)approved);
    lv_obj_t *label = lv_label_create(button);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, ui_i18n_font_24(), 0);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_center(label);
    return button;
}

static void show_confirmation(const jade_confirm_message_t *message)
{
    if (!message || s_confirm_dialog) {
        return;
    }
    s_dialog_generation = message->generation;
    s_confirm_dialog = lv_obj_create(lv_screen_active());
    if (!s_confirm_dialog) {
        finish_confirmation(false);
        return;
    }
    lv_obj_add_flag(s_confirm_dialog, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_size(s_confirm_dialog, 440, 520);
    lv_obj_center(s_confirm_dialog);
    lv_obj_set_style_bg_color(s_confirm_dialog, lv_color_hex(0x181B20), 0);
    lv_obj_set_style_bg_opa(s_confirm_dialog, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_confirm_dialog, lv_color_hex(0x00A688), 0);
    lv_obj_set_style_border_width(s_confirm_dialog, 2, 0);
    lv_obj_set_style_radius(s_confirm_dialog, 7, 0);
    lv_obj_set_style_pad_all(s_confirm_dialog, 16, 0);
    lv_obj_clear_flag(s_confirm_dialog, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_confirm_dialog, confirm_dialog_delete_cb,
                        LV_EVENT_DELETE, NULL);

    lv_obj_t *title = lv_label_create(s_confirm_dialog);
    lv_label_set_text(title, ui_i18n_get(UI_STR_WALLET_REVIEW_PSBT));
    lv_obj_set_style_text_font(title, ui_i18n_font_28(), 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 4, 0);

    lv_obj_t *detail = lv_label_create(s_confirm_dialog);
    lv_label_set_text(detail, message->detail);
    lv_obj_set_size(detail, 400, 330);
    lv_label_set_long_mode(detail, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(detail, ui_i18n_font_24(), 0);
    lv_obj_set_style_text_color(detail, lv_color_hex(0xE0E3E8), 0);
    lv_obj_align(detail, LV_ALIGN_TOP_LEFT, 4, 58);

    add_dialog_button(s_confirm_dialog, ui_i18n_get(UI_STR_WALLET_CANCEL),
                      LV_ALIGN_BOTTOM_LEFT, lv_color_hex(0x4B4E57), false);
    add_dialog_button(s_confirm_dialog, ui_i18n_get(UI_STR_WALLET_CONFIRM_SIGN),
                      LV_ALIGN_BOTTOM_RIGHT, lv_color_hex(0x00A688), true);
    lv_display_trigger_activity(lv_obj_get_display(s_confirm_dialog));
}

static void confirmation_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (s_confirm_dialog &&
        (!atomic_load_explicit(&s_confirm_pending, memory_order_acquire) ||
         s_dialog_generation != atomic_load_explicit(&s_confirm_active_generation,
                                                      memory_order_acquire))) {
        lv_obj_delete(s_confirm_dialog);
    }
    jade_confirm_message_t message;
    while (s_confirm_queue && xQueueReceive(s_confirm_queue, &message, 0) == pdTRUE) {
        if (atomic_load_explicit(&s_confirm_pending, memory_order_acquire) &&
            message.generation == atomic_load_explicit(&s_confirm_active_generation,
                                                       memory_order_acquire)) {
            show_confirmation(&message);
        }
    }
}

void jade_usb_protocol_ui_init(void)
{
    if (!s_confirm_queue) {
        s_confirm_queue = xQueueCreate(1, sizeof(jade_confirm_message_t));
    }
    if (!s_confirm_mutex) {
        s_confirm_mutex = xSemaphoreCreateMutex();
    }
    if (!s_confirm_result_sem) {
        s_confirm_result_sem = xSemaphoreCreateBinary();
    }
    if (!s_confirm_timer && s_confirm_queue && s_confirm_mutex &&
        s_confirm_result_sem) {
        s_confirm_timer = lv_timer_create(confirmation_timer_cb, 30, NULL);
    }
}

static void format_btc(uint64_t satoshi, char *text, size_t size)
{
    snprintf(text, size, "%llu.%08llu BTC",
             (unsigned long long)(satoshi / UINT64_C(100000000)),
             (unsigned long long)(satoshi % UINT64_C(100000000)));
}

static bool request_confirmation(const wallet_psbt_summary_t *summary)
{
    if (!summary || !s_confirm_queue || !s_confirm_mutex ||
        !s_confirm_result_sem ||
        xSemaphoreTake(s_confirm_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return false;
    }
    while (xSemaphoreTake(s_confirm_result_sem, 0) == pdTRUE) {
    }

    jade_confirm_message_t message = {0};
    char send[40], change[40], fee[40];
    format_btc(summary->external_amount, send, sizeof(send));
    format_btc(summary->change_amount, change, sizeof(change));
    format_btc(summary->fee_amount, fee, sizeof(fee));
    snprintf(message.detail, sizeof(message.detail),
             "%s: %s\n%s: %s\n%s: %s\n%s: %u / %u\n",
             ui_i18n_get(UI_STR_WALLET_SEND_AMOUNT), send,
             ui_i18n_get(UI_STR_WALLET_CHANGE_AMOUNT), change,
             ui_i18n_get(UI_STR_WALLET_FEE), fee,
             ui_i18n_get(UI_STR_WALLET_SIGNING_INPUTS),
             (unsigned)summary->signing_input_count,
             (unsigned)summary->input_count);
    size_t used = strlen(message.detail);
    for (size_t i = 0; i < summary->external_output_count &&
                       used < sizeof(message.detail); ++i) {
        char amount[40];
        format_btc(summary->external_outputs[i].amount, amount, sizeof(amount));
        int written = snprintf(message.detail + used, sizeof(message.detail) - used,
                               "\n%s\n%s", amount,
                               summary->external_outputs[i].address);
        if (written < 0 || (size_t)written >= sizeof(message.detail) - used) {
            break;
        }
        used += (size_t)written;
    }

    message.generation = atomic_fetch_add_explicit(&s_confirm_generation, 1,
                                                    memory_order_acq_rel) + 1;
    atomic_store_explicit(&s_confirm_active_generation, message.generation,
                          memory_order_release);
    atomic_store_explicit(&s_confirm_pending, true, memory_order_release);
    s_confirm_result = false;

    bool approved = false;
    if (xQueueSend(s_confirm_queue, &message, 0) == pdTRUE &&
        xSemaphoreTake(s_confirm_result_sem,
                       pdMS_TO_TICKS(JADE_CONFIRM_TIMEOUT_MS)) == pdTRUE) {
        approved = s_confirm_result;
    }
    atomic_store_explicit(&s_confirm_pending, false, memory_order_release);
    xSemaphoreGive(s_confirm_mutex);
    return approved;
}

static bool copy_text_field(const CborValue *map, const char *name,
                            char *out, size_t out_size)
{
    CborValue value;
    if (!map || !out || out_size < 2 ||
        cbor_value_map_find_value(map, name, &value) != CborNoError ||
        !cbor_value_is_text_string(&value)) {
        return false;
    }
    size_t length = out_size;
    return cbor_value_copy_text_string(&value, out, &length, NULL) == CborNoError &&
           length > 0 && length < out_size;
}

static bool get_params(const CborValue *request, CborValue *params)
{
    return cbor_value_map_find_value(request, "params", params) == CborNoError &&
           cbor_value_is_map(params);
}

static bool parse_network(const CborValue *params, wallet_network_t *network)
{
    char name[16];
    if (!copy_text_field(params, "network", name, sizeof(name))) {
        return false;
    }
    if (strcmp(name, "mainnet") == 0) {
        *network = WALLET_NETWORK_BITCOIN;
        return true;
    }
    if (strcmp(name, "testnet") == 0) {
        *network = WALLET_NETWORK_TESTNET;
        return true;
    }
    return false;
}

static bool encode_result(CborEncoder *map, const rpc_result_t *result)
{
    CborError err = cbor_encode_text_stringz(map, "result");
    if (err != CborNoError) {
        return false;
    }
    switch (result->type) {
    case RPC_RESULT_UINT:
        err = cbor_encode_uint(map, result->value.uint_value);
        break;
    case RPC_RESULT_BOOL:
        err = cbor_encode_boolean(map, result->value.bool_value);
        break;
    case RPC_RESULT_TEXT:
        err = cbor_encode_text_stringz(map, result->value.text);
        break;
    case RPC_RESULT_BYTES:
        err = cbor_encode_byte_string(map, result->value.bytes.data,
                                      result->value.bytes.size);
        break;
    case RPC_RESULT_VERSION: {
        CborEncoder version;
        err = cbor_encoder_create_map(map, &version, 4);
        if (err == CborNoError) err = cbor_encode_text_stringz(&version, "JADE_VERSION");
        if (err == CborNoError) err = cbor_encode_text_stringz(&version, esp_app_get_description()->version);
        if (err == CborNoError) err = cbor_encode_text_stringz(&version, "JADE_CONFIG");
        if (err == CborNoError) err = cbor_encode_text_stringz(&version, "ESP32P4");
        if (err == CborNoError) err = cbor_encode_text_stringz(&version, "BOARD_TYPE");
        if (err == CborNoError) err = cbor_encode_text_stringz(&version, "ESP32-P4-PASSKEY");
        if (err == CborNoError) err = cbor_encode_text_stringz(&version, "JADE_STATE");
        if (err == CborNoError) err = cbor_encode_text_stringz(
            &version, wallet_service_get_status() == WALLET_STATUS_READY ? "READY" : "LOCKED");
        if (err == CborNoError) err = cbor_encoder_close_container(map, &version);
        break;
    }
    }
    return err == CborNoError;
}

static bool cdc_write_all(const uint8_t *data, size_t size)
{
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(5000);
    size_t offset = 0;
    while (offset < size && atomic_load_explicit(&s_running, memory_order_acquire)) {
        size_t written = tinyusb_cdcacm_write_queue(TINYUSB_CDC_ACM_0,
                                                     data + offset, size - offset);
        offset += written;
        (void)tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_0, 0);
        if (!written) {
            if (xTaskGetTickCount() >= deadline) {
                return false;
            }
            vTaskDelay(pdMS_TO_TICKS(2));
        }
    }
    return offset == size &&
           tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_0,
                                      pdMS_TO_TICKS(1000)) == ESP_OK;
}

static void send_reply(const char *id, const rpc_result_t *result)
{
    uint8_t *buffer = malloc(JADE_RPC_BUFFER_SIZE);
    if (!buffer) {
        return;
    }
    CborEncoder encoder, map;
    cbor_encoder_init(&encoder, buffer, JADE_RPC_BUFFER_SIZE, 0);
    CborError err = cbor_encoder_create_map(&encoder, &map, 2);
    if (err == CborNoError) err = cbor_encode_text_stringz(&map, "id");
    if (err == CborNoError) err = cbor_encode_text_stringz(&map, id);
    if (err == CborNoError && !encode_result(&map, result)) err = CborErrorUnknownType;
    if (err == CborNoError) err = cbor_encoder_close_container(&encoder, &map);
    if (err == CborNoError) {
        cdc_write_all(buffer, cbor_encoder_get_buffer_size(&encoder, buffer));
    }
    free(buffer);
}

static void send_error(const char *id, int code, const char *message)
{
    uint8_t buffer[256];
    CborEncoder encoder, map, error;
    cbor_encoder_init(&encoder, buffer, sizeof(buffer), 0);
    CborError err = cbor_encoder_create_map(&encoder, &map, 2);
    if (err == CborNoError) err = cbor_encode_text_stringz(&map, "id");
    if (err == CborNoError) err = cbor_encode_text_stringz(&map, id ? id : "0");
    if (err == CborNoError) err = cbor_encode_text_stringz(&map, "error");
    if (err == CborNoError) err = cbor_encoder_create_map(&map, &error, 2);
    if (err == CborNoError) err = cbor_encode_text_stringz(&error, "code");
    if (err == CborNoError) err = cbor_encode_int(&error, code);
    if (err == CborNoError) err = cbor_encode_text_stringz(&error, "message");
    if (err == CborNoError) err = cbor_encode_text_stringz(&error, message);
    if (err == CborNoError) err = cbor_encoder_close_container(&map, &error);
    if (err == CborNoError) err = cbor_encoder_close_container(&encoder, &map);
    if (err == CborNoError) {
        cdc_write_all(buffer, cbor_encoder_get_buffer_size(&encoder, buffer));
    }
}

static bool parse_account_path(const CborValue *params, wallet_network_t network)
{
    CborValue path, item;
    if (cbor_value_map_find_value(params, "path", &path) != CborNoError ||
        !cbor_value_is_array(&path) || cbor_value_enter_container(&path, &item) != CborNoError) {
        return false;
    }
    uint32_t values[3] = {0};
    size_t count = 0;
    while (!cbor_value_at_end(&item) && count < 4) {
        uint64_t value;
        if (!cbor_value_is_unsigned_integer(&item) ||
            cbor_value_get_uint64(&item, &value) != CborNoError ||
            value > UINT32_MAX) {
            return false;
        }
        if (count < 3) values[count] = (uint32_t)value;
        count++;
        if (cbor_value_advance(&item) != CborNoError) return false;
    }
    if (!cbor_value_at_end(&item) || count != 3) {
        return false;
    }
    uint32_t account = 0;
    if (wallet_service_get_address_state(network, &account, NULL, NULL) != ESP_OK) {
        return false;
    }
    uint32_t coin = network == WALLET_NETWORK_BITCOIN ? 0U : 1U;
    return values[0] == (UINT32_C(84) | UINT32_C(0x80000000)) &&
           values[1] == (coin | UINT32_C(0x80000000)) &&
           values[2] == (account | UINT32_C(0x80000000));
}

static void handle_sign_psbt(const char *id, const CborValue *params,
                             wallet_network_t network)
{
    CborValue value;
    if (cbor_value_map_find_value(params, "psbt", &value) != CborNoError ||
        !cbor_value_is_byte_string(&value)) {
        send_error(id, -32602, "Missing binary PSBT");
        return;
    }
    size_t psbt_size = 0;
    if (cbor_value_get_string_length(&value, &psbt_size) != CborNoError ||
        psbt_size == 0 || psbt_size > 6144) {
        send_error(id, -32602, "PSBT is too large");
        return;
    }
    uint8_t *psbt = malloc(psbt_size);
    char *base64 = NULL;
    char *signed_base64 = NULL;
    uint8_t *signed_psbt = NULL;
    size_t signed_capacity = 0;
    if (!psbt) {
        send_error(id, -32603, "Out of memory");
        return;
    }
    size_t copied = psbt_size;
    if (cbor_value_copy_byte_string(&value, psbt, &copied, NULL) != CborNoError ||
        copied != psbt_size ||
        wally_base64_from_bytes(psbt, psbt_size, 0, &base64) != WALLY_OK ||
        !base64) {
        send_error(id, -32602, "Invalid PSBT");
        goto cleanup;
    }
    wallet_psbt_summary_t summary = {0};
    if (wallet_service_inspect_psbt_base64(network, base64, &summary) != ESP_OK) {
        send_error(id, -32602, "Unsupported or invalid BIP84 PSBT");
        goto cleanup;
    }
    if (!request_confirmation(&summary)) {
        send_error(id, -32000, "Transaction rejected or timed out");
        goto cleanup;
    }
    signed_base64 = calloc(1, WALLET_PSBT_BASE64_MAX_LEN);
    if (!signed_base64 ||
        wallet_service_sign_psbt_base64(network, base64, &summary,
                                        signed_base64,
                                        WALLET_PSBT_BASE64_MAX_LEN) != ESP_OK) {
        send_error(id, -32603, "PSBT signing failed");
        goto cleanup;
    }
    if (wally_base64_get_maximum_length(signed_base64, 0, &signed_capacity) != WALLY_OK ||
        signed_capacity == 0 || signed_capacity > 6144 ||
        !(signed_psbt = malloc(signed_capacity))) {
        send_error(id, -32603, "Signed PSBT is too large");
        goto cleanup;
    }
    size_t signed_size = 0;
    if (wally_base64_to_bytes(signed_base64, 0, signed_psbt, signed_capacity,
                              &signed_size) != WALLY_OK) {
        send_error(id, -32603, "Signed PSBT encoding failed");
        goto cleanup;
    }
    rpc_result_t result = {
        .type = RPC_RESULT_BYTES,
        .value.bytes = {.data = signed_psbt, .size = signed_size},
    };
    send_reply(id, &result);

cleanup:
    if (base64) {
        mbedtls_platform_zeroize(base64, strlen(base64));
        wally_free_string(base64);
    }
    if (psbt) {
        mbedtls_platform_zeroize(psbt, psbt_size);
        free(psbt);
    }
    if (signed_base64) {
        mbedtls_platform_zeroize(signed_base64, WALLET_PSBT_BASE64_MAX_LEN);
        free(signed_base64);
    }
    if (signed_psbt) {
        mbedtls_platform_zeroize(signed_psbt, signed_capacity);
        free(signed_psbt);
    }
}

static void process_request(const uint8_t *data, size_t size)
{
    CborParser parser;
    CborValue request, params;
    char id[33] = "0";
    char method[40] = {0};
    if (cbor_parser_init(data, size, 0, &parser, &request) != CborNoError ||
        !cbor_value_is_map(&request) ||
        !copy_text_field(&request, "id", id, sizeof(id)) ||
        !copy_text_field(&request, "method", method, sizeof(method))) {
        send_error(id, -32600, "Invalid request");
        return;
    }
    rpc_result_t result = {0};
    if (strcmp(method, "ping") == 0) {
        result.type = RPC_RESULT_UINT;
        result.value.uint_value = 0;
        send_reply(id, &result);
    } else if (strcmp(method, "get_version_info") == 0) {
        result.type = RPC_RESULT_VERSION;
        send_reply(id, &result);
    } else if (strcmp(method, "auth_user") == 0) {
        wallet_network_t network;
        if (!get_params(&request, &params) || !parse_network(&params, &network)) {
            send_error(id, -32602, "Unsupported network");
        } else if (wallet_service_get_status() != WALLET_STATUS_READY) {
            send_error(id, -32002, "Unlock wallet on device");
        } else {
            result.type = RPC_RESULT_BOOL;
            result.value.bool_value = true;
            send_reply(id, &result);
        }
    } else if (strcmp(method, "get_xpub") == 0) {
        wallet_network_t network;
        char xpub[WALLET_XPUB_MAX_LEN] = {0};
        if (wallet_service_get_status() != WALLET_STATUS_READY) {
            send_error(id, -32002, "Wallet is locked");
        } else if (!get_params(&request, &params) || !parse_network(&params, &network) ||
                   !parse_account_path(&params, network)) {
            send_error(id, -32602, "Only the current BIP84 account path is allowed");
        } else if (wallet_service_get_bip84_xpub(network, xpub, sizeof(xpub)) != ESP_OK) {
            send_error(id, -32603, "XPUB export failed");
        } else {
            result.type = RPC_RESULT_TEXT;
            result.value.text = xpub;
            send_reply(id, &result);
        }
        memset(xpub, 0, sizeof(xpub));
    } else if (strcmp(method, "sign_psbt") == 0) {
        wallet_network_t network;
        if (wallet_service_get_status() != WALLET_STATUS_READY) {
            send_error(id, -32002, "Wallet is locked");
        } else if (!get_params(&request, &params) || !parse_network(&params, &network)) {
            send_error(id, -32602, "Unsupported network");
        } else {
            handle_sign_psbt(id, &params, network);
        }
    } else if (strcmp(method, "logout") == 0) {
        wallet_service_lock();
        result.type = RPC_RESULT_BOOL;
        result.value.bool_value = true;
        send_reply(id, &result);
    } else {
        send_error(id, -32601, "Method not supported");
    }
}

static size_t complete_message_size(const uint8_t *data, size_t size)
{
    CborParser parser;
    CborValue value, next;
    if (cbor_parser_init(data, size, 0, &parser, &value) != CborNoError ||
        !cbor_value_is_map(&value)) {
        return 0;
    }
    next = value;
    if (cbor_value_advance(&next) != CborNoError) {
        return 0;
    }
    return (size_t)(next.source.ptr - data);
}

static void jade_task(void *unused)
{
    (void)unused;
    uint8_t *buffer = malloc(JADE_RPC_BUFFER_SIZE);
    size_t used = 0;
    if (!buffer) {
        atomic_store_explicit(&s_running, false, memory_order_release);
        s_task = NULL;
        vTaskDelete(NULL);
        return;
    }
    while (atomic_load_explicit(&s_running, memory_order_acquire)) {
        size_t received = xStreamBufferReceive(s_rx_stream, buffer + used,
                                               JADE_RPC_BUFFER_SIZE - used,
                                               pdMS_TO_TICKS(100));
        used += received;
        size_t message_size;
        while (used && (message_size = complete_message_size(buffer, used)) > 0) {
            process_request(buffer, message_size);
            memmove(buffer, buffer + message_size, used - message_size);
            used -= message_size;
        }
        if (used == JADE_RPC_BUFFER_SIZE) {
            send_error("0", -32600, "Request too large or malformed");
            used = 0;
        }
    }
    mbedtls_platform_zeroize(buffer, JADE_RPC_BUFFER_SIZE);
    free(buffer);
    s_task = NULL;
    vTaskDelete(NULL);
}

static void cdc_rx_callback(int itf, cdcacm_event_t *event)
{
    (void)event;
    uint8_t chunk[256];
    size_t received = 0;
    do {
        if (tinyusb_cdcacm_read((tinyusb_cdcacm_itf_t)itf, chunk,
                                sizeof(chunk), &received) != ESP_OK || !received) {
            break;
        }
        if (!s_rx_stream || xStreamBufferSend(s_rx_stream, chunk, received, 0) != received) {
            ESP_LOGW(TAG, "Dropping CDC input: receive buffer full");
        }
    } while (received == sizeof(chunk));
}

esp_err_t jade_usb_protocol_start(void)
{
    if (atomic_load_explicit(&s_running, memory_order_acquire)) {
        return ESP_OK;
    }
    if (!s_confirm_timer) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_rx_stream) {
        s_rx_stream = xStreamBufferCreate(JADE_RX_STREAM_SIZE, 1);
    }
    if (!s_rx_stream) {
        return ESP_ERR_NO_MEM;
    }
    xStreamBufferReset(s_rx_stream);
    esp_err_t ret = usb_hid_enable_jade();
    if (ret != ESP_OK) {
        return ret;
    }
    tinyusb_config_cdcacm_t config = {
        .cdc_port = TINYUSB_CDC_ACM_0,
        .callback_rx = cdc_rx_callback,
    };
    ret = tinyusb_cdcacm_init(&config);
    if (ret != ESP_OK) {
        (void)usb_hid_disable_jade();
        return ret;
    }
    s_cdc_initialized = true;
    atomic_store_explicit(&s_running, true, memory_order_release);
    if (xTaskCreate(jade_task, "jade_usb", JADE_TASK_STACK_SIZE, NULL, 5,
                    &s_task) != pdPASS) {
        atomic_store_explicit(&s_running, false, memory_order_release);
        tinyusb_cdcacm_deinit(TINYUSB_CDC_ACM_0);
        s_cdc_initialized = false;
        (void)usb_hid_disable_jade();
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "Jade USB protocol started");
    return ESP_OK;
}

esp_err_t jade_usb_protocol_stop(void)
{
    atomic_store_explicit(&s_running, false, memory_order_release);
    atomic_store_explicit(&s_confirm_pending, false, memory_order_release);
    if (s_confirm_result_sem) {
        xSemaphoreGive(s_confirm_result_sem);
    }
    for (int i = 0;
         s_task && i < JADE_STOP_TIMEOUT_MS / 20;
         ++i) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (s_task) {
        return ESP_ERR_TIMEOUT;
    }
    if (s_cdc_initialized) {
        (void)tinyusb_cdcacm_unregister_callback(TINYUSB_CDC_ACM_0, CDC_EVENT_RX);
        (void)tinyusb_cdcacm_deinit(TINYUSB_CDC_ACM_0);
        s_cdc_initialized = false;
    }
    esp_err_t ret = usb_hid_disable_jade();
    ESP_LOGI(TAG, "Jade USB protocol stopped: %s", esp_err_to_name(ret));
    return ret;
}

bool jade_usb_protocol_is_running(void)
{
    return atomic_load_explicit(&s_running, memory_order_acquire);
}

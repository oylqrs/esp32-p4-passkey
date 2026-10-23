#include "chameleon_ble_hid.h"

#include <stdatomic.h>
#include <stddef.h>
#include <string.h>

#include "chameleon_uart.h"
#include "chameleon_commands.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "keyboard_layout.h"

#define BLE_HID_CMD_CONFIGURE   0x7F10u
#define BLE_HID_CMD_SEND_REPORT 0x7F11u
#define BLE_HID_CMD_RELEASE_ALL 0x7F12u
#define BLE_HID_CMD_GET_STATUS  0x7F13u

#define BLE_HID_PROTOCOL_VERSION 1u
#define BLE_HID_STATUS_SIZE      11u
#define BLE_HID_QUEUE_DEPTH      8u
#define BLE_HID_TEXT_MAX_LEN     160u
#define BLE_HID_REPORT_INTERVAL_MS 25u
#define BLE_HID_STATUS_INTERVAL_MS 1000u
#define BLE_HID_RETRY_INTERVAL_MS  500u
#define BLE_HID_SLOW_RETRY_INTERVAL_MS 10000u
#define BLE_HID_DISABLED_RETRY_MAX 3u
#define BLE_HID_TASK_STACK_SIZE    4096u
#define BLE_HID_TASK_PRIORITY      5u

#define CHAMELEON_STATUS_NOT_IMPLEMENTED 0x0069u

typedef enum {
    BLE_HID_MESSAGE_TEXT,
    BLE_HID_MESSAGE_KEY_PRESS,
    BLE_HID_MESSAGE_KEY_RELEASE,
    BLE_HID_MESSAGE_DELAY,
    BLE_HID_MESSAGE_SYNC,
    BLE_HID_MESSAGE_RECONFIGURE,
} ble_hid_message_type_t;

typedef struct {
    ble_hid_message_type_t type;
    uint8_t modifier;
    uint8_t key;
    uint32_t delay_ms;
    TaskHandle_t waiter;
    char text[BLE_HID_TEXT_MAX_LEN];
} ble_hid_message_t;

static const char *TAG = "ble_hid";
static QueueHandle_t s_queue;
static TaskHandle_t s_task;
static atomic_bool s_initialized;
static atomic_bool s_desired_enabled;
static atomic_bool s_desired_pairing;
static atomic_bool s_ready;
static atomic_bool s_connected;
static atomic_bool s_encrypted;
static atomic_bool s_bonded;
static atomic_bool s_notifications;
static atomic_bool s_suspended;
static atomic_uchar s_protocol_mode;
static atomic_uchar s_led_state;
static atomic_uchar s_queued_reports;
static bool s_applied_enabled;
static bool s_applied_pairing;
static bool s_config_synchronized;
static bool s_protocol_supported = true;
static uint8_t s_config_retry_count;
static uint8_t s_sequence;

static void clear_cached_status(void)
{
    atomic_store(&s_ready, false);
    atomic_store(&s_connected, false);
    atomic_store(&s_encrypted, false);
    atomic_store(&s_bonded, false);
    atomic_store(&s_notifications, false);
    atomic_store(&s_suspended, false);
    atomic_store(&s_protocol_mode, 1u);
    atomic_store(&s_led_state, 0u);
    atomic_store(&s_queued_reports, 0u);
}

static void update_status(const uint8_t data[BLE_HID_STATUS_SIZE])
{
    if (data[0] != BLE_HID_PROTOCOL_VERSION) {
        clear_cached_status();
        return;
    }
    atomic_store(&s_connected, data[3] != 0u);
    atomic_store(&s_encrypted, data[4] != 0u);
    atomic_store(&s_bonded, data[5] != 0u);
    atomic_store(&s_notifications, data[6] != 0u);
    atomic_store(&s_suspended, data[7] != 0u);
    atomic_store(&s_protocol_mode, data[8]);
    atomic_store(&s_led_state, data[9]);
    atomic_store(&s_queued_reports, data[10]);
    atomic_store(&s_ready,
                 atomic_load(&s_desired_enabled) && data[1] != 0u &&
                 data[3] != 0u && data[4] != 0u && data[6] != 0u &&
                 data[7] == 0u);
}

static esp_err_t transact(uint16_t command, const uint8_t *request,
                          uint16_t request_length, uint8_t *response_data,
                          uint16_t response_capacity, uint16_t *response_length,
                          uint16_t *device_status)
{
    chameleon_uart_response_t response = {
        .data = response_data,
        .capacity = response_capacity,
    };
    esp_err_t ret = chameleon_uart_transceive(command, request, request_length,
                                              &response,
                                              CHAMELEON_COMMAND_TIMEOUT_MS);
    if (response_length != NULL) {
        *response_length = response.length;
    }
    if (device_status != NULL) {
        *device_status = response.status;
    }
    if (ret != ESP_OK) {
        return ret;
    }
    if (response.status == CHAMELEON_STATUS_NOT_IMPLEMENTED) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (response.status != CHAMELEON_STATUS_SUCCESS) {
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

static esp_err_t configure_remote(bool enabled, bool pairing_allowed)
{
    uint8_t request[3] = {
        BLE_HID_PROTOCOL_VERSION,
        enabled ? 1u : 0u,
        pairing_allowed ? 1u : 0u,
    };
    uint8_t response[BLE_HID_STATUS_SIZE];
    uint16_t response_length = 0;
    uint16_t device_status = 0;
    esp_err_t ret = transact(BLE_HID_CMD_CONFIGURE, request, sizeof(request),
                             response, sizeof(response), &response_length,
                             &device_status);
    if (ret != ESP_OK) {
        return ret;
    }
    if (response_length != sizeof(response)) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    update_status(response);
    return ESP_OK;
}

static esp_err_t request_status(void)
{
    uint8_t request = BLE_HID_PROTOCOL_VERSION;
    uint8_t response[BLE_HID_STATUS_SIZE];
    uint16_t response_length = 0;
    uint16_t device_status = 0;
    esp_err_t ret = transact(BLE_HID_CMD_GET_STATUS, &request, sizeof(request),
                             response, sizeof(response), &response_length,
                             &device_status);
    if (ret != ESP_OK) {
        clear_cached_status();
        return ret;
    }
    if (response_length != sizeof(response)) {
        clear_cached_status();
        return ESP_ERR_INVALID_RESPONSE;
    }
    update_status(response);
    if ((response[1] != 0u) != atomic_load(&s_desired_enabled) ||
        (response[2] != 0u) != atomic_load(&s_desired_pairing)) {
        s_config_synchronized = false;
    }
    return ESP_OK;
}

static esp_err_t send_report(const uint8_t report[CHAMELEON_BLE_HID_REPORT_SIZE])
{
    uint8_t request[2 + CHAMELEON_BLE_HID_REPORT_SIZE] = {
        BLE_HID_PROTOCOL_VERSION,
        ++s_sequence,
    };
    memcpy(&request[2], report, CHAMELEON_BLE_HID_REPORT_SIZE);
    uint8_t response[2];
    uint16_t response_length = 0;
    uint16_t device_status = 0;
    esp_err_t ret = transact(BLE_HID_CMD_SEND_REPORT, request, sizeof(request),
                             response, sizeof(response), &response_length,
                             &device_status);
    if (ret != ESP_OK) {
        atomic_store(&s_ready, false);
        return ret;
    }
    if (response_length != sizeof(response) ||
        response[0] != BLE_HID_PROTOCOL_VERSION || response[1] != request[1]) {
        atomic_store(&s_ready, false);
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}

static esp_err_t release_all_remote(void)
{
    uint8_t request = BLE_HID_PROTOCOL_VERSION;
    uint16_t response_length = 0;
    uint16_t device_status = 0;
    return transact(BLE_HID_CMD_RELEASE_ALL, &request, sizeof(request), NULL, 0,
                    &response_length, &device_status);
}

static void synchronize_mode(void)
{
    bool enabled = atomic_load(&s_desired_enabled);
    bool pairing = atomic_load(&s_desired_pairing);
    if (!s_protocol_supported ||
        (s_config_synchronized && enabled == s_applied_enabled &&
         pairing == s_applied_pairing)) {
        return;
    }

    if (s_applied_enabled && !enabled) {
        (void)release_all_remote();
    }
    esp_err_t ret = configure_remote(enabled, pairing);
    if (ret == ESP_ERR_NOT_SUPPORTED) {
        s_protocol_supported = false;
        s_config_retry_count = BLE_HID_DISABLED_RETRY_MAX;
        clear_cached_status();
        ESP_LOGW(TAG, "Chameleon firmware does not support BLE HID protocol v1");
        return;
    }
    if (ret != ESP_OK) {
        s_config_synchronized = false;
        if (s_config_retry_count < BLE_HID_DISABLED_RETRY_MAX) {
            s_config_retry_count++;
        }
        clear_cached_status();
        ESP_LOGW(TAG, "BLE HID configure failed: %s", esp_err_to_name(ret));
        return;
    }

    s_applied_enabled = enabled;
    s_applied_pairing = pairing;
    s_config_synchronized = true;
    s_config_retry_count = 0u;
    ESP_LOGI(TAG, "BLE HID configured: enabled=%u pairing=%u",
             enabled ? 1u : 0u, pairing ? 1u : 0u);
}

static esp_err_t send_key(uint8_t modifier, uint8_t key)
{
    uint8_t report[CHAMELEON_BLE_HID_REPORT_SIZE] = {0};
    report[0] = modifier;
    report[2] = key;
    esp_err_t ret = send_report(report);
    if (ret != ESP_OK) {
        return ret;
    }
    vTaskDelay(pdMS_TO_TICKS(BLE_HID_REPORT_INTERVAL_MS));
    memset(report, 0, sizeof(report));
    ret = send_report(report);
    vTaskDelay(pdMS_TO_TICKS(BLE_HID_REPORT_INTERVAL_MS));
    return ret;
}

static void process_message(const ble_hid_message_t *message)
{
    if (message->type == BLE_HID_MESSAGE_SYNC) {
        xTaskNotifyGive(message->waiter);
        return;
    }
    if (message->type == BLE_HID_MESSAGE_RECONFIGURE) {
        s_protocol_supported = true;
        s_config_retry_count = 0u;
        return;
    }
    if (!chameleon_ble_hid_is_ready()) {
        ESP_LOGW(TAG, "Dropped BLE HID operation while keyboard is not ready");
        return;
    }

    esp_err_t ret = ESP_OK;
    if (message->type == BLE_HID_MESSAGE_KEY_PRESS) {
        uint8_t report[CHAMELEON_BLE_HID_REPORT_SIZE] = {0};
        report[0] = message->modifier;
        report[2] = message->key;
        ret = send_report(report);
        vTaskDelay(pdMS_TO_TICKS(BLE_HID_REPORT_INTERVAL_MS));
    } else if (message->type == BLE_HID_MESSAGE_KEY_RELEASE) {
        ret = release_all_remote();
        vTaskDelay(pdMS_TO_TICKS(BLE_HID_REPORT_INTERVAL_MS));
    } else if (message->type == BLE_HID_MESSAGE_DELAY) {
        vTaskDelay(pdMS_TO_TICKS(message->delay_ms));
    } else {
        for (size_t i = 0; message->text[i] != '\0'; i++) {
            uint8_t modifier = 0;
            uint8_t key = 0;
            if (!keyboard_layout_char(message->text[i], &modifier, &key)) {
                ESP_LOGW(TAG, "Unsupported BLE HID char: 0x%02x",
                         (unsigned char)message->text[i]);
                continue;
            }
            ret = send_key(modifier, key);
            if (ret != ESP_OK) {
                break;
            }
        }
    }

    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "BLE HID operation failed: %s", esp_err_to_name(ret));
        (void)release_all_remote();
    }
}

static void ble_hid_task(void *arg)
{
    (void)arg;
    ble_hid_message_t message;
    while (true) {
        synchronize_mode();
        bool enabled = atomic_load(&s_desired_enabled);
        TickType_t wait = enabled ? pdMS_TO_TICKS(BLE_HID_STATUS_INTERVAL_MS)
                                  : portMAX_DELAY;
        if (!s_config_synchronized) {
            if (enabled) {
                uint32_t retry_ms =
                    s_config_retry_count < BLE_HID_DISABLED_RETRY_MAX
                        ? BLE_HID_RETRY_INTERVAL_MS
                        : BLE_HID_SLOW_RETRY_INTERVAL_MS;
                wait = pdMS_TO_TICKS(retry_ms);
            } else if (s_config_retry_count < BLE_HID_DISABLED_RETRY_MAX) {
                wait = pdMS_TO_TICKS(BLE_HID_RETRY_INTERVAL_MS);
            }
        }
        if (xQueueReceive(s_queue, &message, wait) == pdTRUE) {
            synchronize_mode();
            process_message(&message);
        } else if (atomic_load(&s_desired_enabled)) {
            if (request_status() != ESP_OK) {
                s_config_synchronized = false;
            }
        }
    }
}

esp_err_t chameleon_ble_hid_init(void)
{
    if (atomic_load(&s_initialized)) {
        return ESP_OK;
    }
    s_queue = xQueueCreate(BLE_HID_QUEUE_DEPTH, sizeof(ble_hid_message_t));
    if (s_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(ble_hid_task, "ble_hid", BLE_HID_TASK_STACK_SIZE, NULL,
                    BLE_HID_TASK_PRIORITY, &s_task) != pdPASS) {
        vQueueDelete(s_queue);
        s_queue = NULL;
        return ESP_ERR_NO_MEM;
    }
    clear_cached_status();
    atomic_store(&s_initialized, true);
    return ESP_OK;
}

void chameleon_ble_hid_set_mode(bool enabled, bool pairing_allowed)
{
    if (pairing_allowed) {
        enabled = true;
    }
    atomic_store(&s_desired_enabled, enabled);
    atomic_store(&s_desired_pairing, pairing_allowed);
    atomic_store(&s_ready, false);
    if (s_queue != NULL) {
        ble_hid_message_t message = {.type = BLE_HID_MESSAGE_RECONFIGURE};
        (void)xQueueSend(s_queue, &message, 0);
    }
}

bool chameleon_ble_hid_is_ready(void)
{
    return atomic_load(&s_initialized) && atomic_load(&s_ready);
}

void chameleon_ble_hid_get_cached_status(chameleon_ble_hid_status_t *status)
{
    if (status == NULL) {
        return;
    }
    status->enabled = atomic_load(&s_desired_enabled);
    status->pairing_allowed = atomic_load(&s_desired_pairing);
    status->connected = atomic_load(&s_connected);
    status->encrypted = atomic_load(&s_encrypted);
    status->bonded = atomic_load(&s_bonded);
    status->notifications_enabled = atomic_load(&s_notifications);
    status->suspended = atomic_load(&s_suspended);
    status->protocol_mode = atomic_load(&s_protocol_mode);
    status->led_state = atomic_load(&s_led_state);
    status->queued_reports = atomic_load(&s_queued_reports);
}

static esp_err_t queue_message(const ble_hid_message_t *message)
{
    if (!atomic_load(&s_initialized) || s_queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return xQueueSend(s_queue, message, 0) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t chameleon_ble_hid_type_text(const char *text)
{
    if (!chameleon_ble_hid_is_ready()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (text == NULL || text[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    ble_hid_message_t message = {.type = BLE_HID_MESSAGE_TEXT};
    if (strlen(text) >= sizeof(message.text)) {
        return ESP_ERR_INVALID_SIZE;
    }
    strlcpy(message.text, text, sizeof(message.text));
    return queue_message(&message);
}

esp_err_t chameleon_ble_hid_report_press(uint8_t modifier, uint8_t key)
{
    if (!chameleon_ble_hid_is_ready()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (modifier == 0u && key == 0u) {
        return ESP_ERR_INVALID_ARG;
    }
    ble_hid_message_t message = {
        .type = BLE_HID_MESSAGE_KEY_PRESS,
        .modifier = modifier,
        .key = key,
    };
    return queue_message(&message);
}

esp_err_t chameleon_ble_hid_key_release(void)
{
    ble_hid_message_t message = {.type = BLE_HID_MESSAGE_KEY_RELEASE};
    return queue_message(&message);
}

esp_err_t chameleon_ble_hid_delay(uint32_t delay_ms)
{
    ble_hid_message_t message = {
        .type = BLE_HID_MESSAGE_DELAY,
        .delay_ms = delay_ms,
    };
    return queue_message(&message);
}

esp_err_t chameleon_ble_hid_wait_idle(void)
{
    if (!atomic_load(&s_initialized) || s_queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    ble_hid_message_t message = {
        .type = BLE_HID_MESSAGE_SYNC,
        .waiter = xTaskGetCurrentTaskHandle(),
    };
    if (xQueueSend(s_queue, &message, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    return ESP_OK;
}

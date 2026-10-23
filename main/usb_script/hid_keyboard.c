#include "hid_keyboard.h"

#include <stdatomic.h>

#include "chameleon_uart/chameleon_ble_hid.h"
#include "esp_log.h"
#include "usb_hid_keyboard.h"

static const char *TAG = "hid_keyboard";
static atomic_int s_transport = HID_KEYBOARD_TRANSPORT_USB;

typedef enum {
    HID_BACKEND_NONE,
    HID_BACKEND_USB,
    HID_BACKEND_BLUETOOTH,
} hid_backend_t;

static hid_backend_t selected_backend(void)
{
    hid_keyboard_transport_t transport =
        (hid_keyboard_transport_t)atomic_load(&s_transport);
    if (transport == HID_KEYBOARD_TRANSPORT_USB) {
        return HID_BACKEND_USB;
    }
    if (transport == HID_KEYBOARD_TRANSPORT_BLUETOOTH) {
        return HID_BACKEND_BLUETOOTH;
    }

    bool usb_ready = usb_hid_keyboard_is_ready();
    bool ble_ready = chameleon_ble_hid_is_ready();
    if (usb_ready == ble_ready) {
        return HID_BACKEND_NONE;
    }
    return usb_ready ? HID_BACKEND_USB : HID_BACKEND_BLUETOOTH;
}

void hid_key_press(uint8_t key)
{
    esp_err_t ret = hid_key_report_async(0, key);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to queue key press 0x%02x: %s", key, esp_err_to_name(ret));
    }
}

void hid_key_release(void)
{
    esp_err_t ret = hid_key_release_async();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to queue key release: %s", esp_err_to_name(ret));
    }
}

void hid_type_string(char *str)
{
    esp_err_t ret = hid_type_string_async(str);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to queue string: %s", esp_err_to_name(ret));
    }
}

esp_err_t hid_type_string_async(const char *str)
{
    switch (selected_backend()) {
    case HID_BACKEND_USB:
        return usb_hid_keyboard_type_text(str);
    case HID_BACKEND_BLUETOOTH:
        return chameleon_ble_hid_type_text(str);
    default:
        return ESP_ERR_INVALID_STATE;
    }
}

esp_err_t hid_key_report_async(uint8_t modifier, uint8_t key)
{
    switch (selected_backend()) {
    case HID_BACKEND_USB:
        return usb_hid_keyboard_report_press(modifier, key);
    case HID_BACKEND_BLUETOOTH:
        return chameleon_ble_hid_report_press(modifier, key);
    default:
        return ESP_ERR_INVALID_STATE;
    }
}

esp_err_t hid_key_release_async(void)
{
    switch (selected_backend()) {
    case HID_BACKEND_USB:
        return usb_hid_keyboard_key_release();
    case HID_BACKEND_BLUETOOTH:
        return chameleon_ble_hid_key_release();
    default:
        return ESP_ERR_INVALID_STATE;
    }
}

esp_err_t hid_delay_async(uint32_t delay_ms)
{
    switch (selected_backend()) {
    case HID_BACKEND_USB:
        return usb_hid_keyboard_delay(delay_ms);
    case HID_BACKEND_BLUETOOTH:
        return chameleon_ble_hid_delay(delay_ms);
    default:
        return ESP_ERR_INVALID_STATE;
    }
}

esp_err_t hid_keyboard_wait_idle(void)
{
    switch (selected_backend()) {
    case HID_BACKEND_USB:
        return usb_hid_keyboard_wait_idle();
    case HID_BACKEND_BLUETOOTH:
        return chameleon_ble_hid_wait_idle();
    default:
        return ESP_ERR_INVALID_STATE;
    }
}

bool hid_keyboard_is_ready(void)
{
    switch (selected_backend()) {
    case HID_BACKEND_USB:
        return usb_hid_keyboard_is_ready();
    case HID_BACKEND_BLUETOOTH:
        return chameleon_ble_hid_is_ready();
    default:
        return false;
    }
}

esp_err_t hid_keyboard_set_transport(hid_keyboard_transport_t transport)
{
    if ((unsigned int)transport >= HID_KEYBOARD_TRANSPORT_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    atomic_store(&s_transport, transport);
    return ESP_OK;
}

hid_keyboard_transport_t hid_keyboard_get_transport(void)
{
    int transport = atomic_load(&s_transport);
    return (unsigned int)transport < HID_KEYBOARD_TRANSPORT_COUNT
               ? (hid_keyboard_transport_t)transport
               : HID_KEYBOARD_TRANSPORT_USB;
}

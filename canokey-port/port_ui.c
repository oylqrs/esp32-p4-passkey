#include "device.h"
#include "canokey_esp32p4.h"
#include "canokey_nfc.h"
#include "esp_log.h"

static const char *TAG = "port_ui";
static void (*s_user_presence_request_callback)(uint8_t entry);

void led_on(void)
{
}

void led_off(void)
{
}

void canokey_esp32p4_set_user_presence_request_callback(void (*callback)(uint8_t entry))
{
    s_user_presence_request_callback = callback;
    ESP_LOGI(TAG, "user presence callback %s", callback ? "registered" : "cleared");
}

void device_user_presence_request(uint8_t entry)
{
    if (canokey_nfc_is_active()) {
        entry = CANOKEY_NFC_WAIT_ENTRY;
        canokey_nfc_presence_requested();
    }
    ESP_LOGW(TAG, "user presence request hook entry=%u callback=%u",
             entry,
             s_user_presence_request_callback ? 1 : 0);

    if (s_user_presence_request_callback) {
        s_user_presence_request_callback(entry);
    }
}

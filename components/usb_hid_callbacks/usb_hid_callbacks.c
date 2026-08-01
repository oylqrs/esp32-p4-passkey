#include <stdint.h>

#include "canokey_esp32p4.h"
#include "class/hid/hid_device.h"
#include "esp_log.h"
#include "tinyusb.h"

#define HID_ITF_NUM_KEYBOARD 0
#define HID_ITF_NUM_CTAPHID  1
#define CTAPHID_EP_SIZE      64

static const char *TAG = "usb_hid_cb";

static const uint8_t s_hid_report_descriptor[] = {
    TUD_HID_REPORT_DESC_KEYBOARD(HID_REPORT_ID(HID_ITF_PROTOCOL_KEYBOARD)),
};

static const uint8_t s_ctaphid_report_descriptor[] = {
    0x06, 0xD0, 0xF1,
    0x09, 0x01,
    0xA1, 0x01,
    0x09, 0x20,
    0x15, 0x00,
    0x26, 0xFF, 0x00,
    0x75, 0x08,
    0x95, 0x40,
    0x81, 0x02,
    0x09, 0x21,
    0x15, 0x00,
    0x26, 0xFF, 0x00,
    0x75, 0x08,
    0x95, 0x40,
    0x91, 0x02,
    0xC0,
};

uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance)
{
    ESP_LOGI(TAG, "HID report descriptor requested instance=%u", instance);
    if (instance == HID_ITF_NUM_CTAPHID) {
        return s_ctaphid_report_descriptor;
    }

    return s_hid_report_descriptor;
}

uint16_t tud_hid_get_report_cb(uint8_t instance,
                               uint8_t report_id,
                               hid_report_type_t report_type,
                               uint8_t *buffer,
                               uint16_t reqlen)
{
    (void)instance;
    (void)report_id;
    (void)report_type;
    (void)buffer;
    (void)reqlen;
    return 0;
}

void tud_hid_set_report_cb(uint8_t instance,
                           uint8_t report_id,
                           hid_report_type_t report_type,
                           uint8_t const *buffer,
                           uint16_t bufsize)
{
    ESP_LOGI(TAG, "HID SET_REPORT/OUT instance=%u report_id=%u type=%u len=%u",
             instance,
             report_id,
             report_type,
             bufsize);

    if (instance == HID_ITF_NUM_CTAPHID && report_type == HID_REPORT_TYPE_OUTPUT) {
        if (bufsize >= CTAPHID_EP_SIZE && buffer) {
            uint8_t cmd = buffer[4];
            uint16_t msg_len = (uint16_t)((buffer[5] << 8) | buffer[6]);
            if ((cmd & 0x80) != 0) {
                ESP_LOGI(TAG, "CTAPHID OUT raw=0x%02x cmd=0x%02x len=%u",
                         cmd,
                         cmd & 0x7F,
                         msg_len);
            } else {
                ESP_LOGD(TAG, "CTAPHID OUT cont seq=0x%02x", cmd);
            }
        } else {
            ESP_LOGW(TAG, "CTAPHID OUT short report: len=%u", bufsize);
            return;
        }
        canokey_esp32p4_ctaphid_queue_out_report(buffer, bufsize);
    }
}

void tud_hid_report_complete_cb(uint8_t instance, uint8_t const *report, uint16_t len)
{
    if (instance == HID_ITF_NUM_CTAPHID) {
        canokey_esp32p4_ctaphid_queue_data_in();
    }

    uint8_t report_id_or_cmd = (report && len > 4) ? report[4] : 0;
    ESP_LOGI(TAG,
             "HID report complete instance=%u len=%u byte4=0x%02x",
             instance,
             len,
             report_id_or_cmd);

    if (instance == HID_ITF_NUM_CTAPHID) {
        ESP_LOGI(TAG, "CTAPHID IN complete len=%u", len);
    }
}

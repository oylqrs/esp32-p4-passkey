#pragma once

#include <stdint.h>

#include "esp_err.h"
#include "tinyusb_msc.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t usb_msc_storage_init(void);
esp_err_t usb_hid_mount_usbstore_app(void);
esp_err_t usb_msc_factory_format(void);
esp_err_t usb_msc_mark_storage_initialized(void);
esp_err_t usb_hid_dump_storage_files(void);
esp_err_t usb_hid_enable_usbstore(void);
esp_err_t usb_hid_disable_usbstore(void);
esp_err_t usb_hid_toggle_usbstore(void);
bool usb_hid_usbstore_is_exposed_to_usb(void);
esp_err_t usb_hid_enable_ncm(void);
esp_err_t usb_hid_disable_ncm(void);
bool usb_hid_ncm_is_enabled(void);
esp_err_t usb_hid_enable_jade(void);
esp_err_t usb_hid_disable_jade(void);
bool usb_hid_jade_is_enabled(void);
uint16_t usb_hid_get_vid(void);
esp_err_t usb_hid_set_vid(uint16_t vid);

#ifdef __cplusplus
}
#endif

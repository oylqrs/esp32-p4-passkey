#pragma once

#include "esp_err.h"
#include "tinyusb_msc.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t usb_msc_storage_init(void);
esp_err_t usb_hid_mount_usbstore_app(void);
esp_err_t usb_hid_dump_storage_files(void);
esp_err_t usb_hid_enable_usbstore(void);
esp_err_t usb_hid_disable_usbstore(void);
esp_err_t usb_hid_toggle_usbstore(void);
bool usb_hid_usbstore_is_exposed_to_usb(void);

#ifdef __cplusplus
}
#endif

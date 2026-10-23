#pragma once

#include "esp_err.h"

#define USB_NCM_IP_ADDRESS_LEN 16

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t usb_ncm_init(void);
void usb_ncm_deinit(void);
void usb_ncm_get_ip_address(char address[USB_NCM_IP_ADDRESS_LEN]);
esp_err_t usb_ncm_set_ip_address(const char *address);

#ifdef __cplusplus
}
#endif

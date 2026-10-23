#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t usb_nrf_switch_init(void);
esp_err_t usb_nrf_switch_set(bool enabled);
bool usb_nrf_switch_is_enabled(void);

#ifdef __cplusplus
}
#endif

#pragma once

#include <stdbool.h>

#include "esp_err.h"

void jade_usb_protocol_ui_init(void);
esp_err_t jade_usb_protocol_start(void);
esp_err_t jade_usb_protocol_stop(void);
bool jade_usb_protocol_is_running(void);


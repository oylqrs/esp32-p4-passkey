#pragma once

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

size_t usb_ccid_process_message(const uint8_t *request, size_t request_len,
                                uint8_t *response, size_t response_size);
bool usb_ccid_start(void);
bool usb_ccid_activate_card(void);
bool usb_ccid_activate_openpgp_card(void);
void usb_ccid_deactivate_card(void);

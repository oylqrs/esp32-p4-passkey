#pragma once

#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    USB_SCRIPT_STATUS_IDLE,
    USB_SCRIPT_STATUS_RUNNING,
    USB_SCRIPT_STATUS_COMPLETED,
    USB_SCRIPT_STATUS_FAILED,
} usb_script_status_t;

typedef struct {
    usb_script_status_t status;
    size_t step;
    size_t total;
} usb_script_progress_t;

esp_err_t usb_script_send_file(const char *path);
void usb_script_get_progress(usb_script_progress_t *progress);

#ifdef __cplusplus
}
#endif

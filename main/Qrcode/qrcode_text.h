#pragma once

#include <stdint.h>

#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

#define QRCODE_TEXT_DEFAULT_SIZE       300
#define QRCODE_TEXT_DEFAULT_QUIET_ZONE 12

typedef struct {
    int32_t size;
    int32_t quiet_zone;
    lv_color_t dark_color;
    lv_color_t light_color;
} qrcode_text_config_t;

/**
 * Create a QR code from a null-terminated UTF-8 string.
 *
 * The returned object includes the quiet zone and can be aligned, hidden, or
 * deleted like any other LVGL object. Call this API while LVGL is locked.
 */
lv_obj_t *qrcode_text_create(lv_obj_t *parent, const char *text);

/** Create a QR code with a custom total size, quiet zone, and colors. */
lv_obj_t *qrcode_text_create_with_config(lv_obj_t *parent,
                                         const char *text,
                                         const qrcode_text_config_t *config);

/** Replace the text encoded by an existing QR code container. */
esp_err_t qrcode_text_update(lv_obj_t *qrcode_container, const char *text);

/** Return the internal LVGL QR code object, or NULL for an invalid container. */
lv_obj_t *qrcode_text_get_object(lv_obj_t *qrcode_container);

#ifdef __cplusplus
}
#endif

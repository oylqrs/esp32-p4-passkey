#include "qrcode_text.h"

#include <string.h>

#define QRCODE_TEXT_MIN_CANVAS_SIZE 21

static bool config_is_valid(const qrcode_text_config_t *config)
{
    return config != NULL &&
           config->size > 0 &&
           config->quiet_zone >= 0 &&
           config->size - config->quiet_zone * 2 >= QRCODE_TEXT_MIN_CANVAS_SIZE;
}

lv_obj_t *qrcode_text_get_object(lv_obj_t *qrcode_container)
{
    if (qrcode_container == NULL || !lv_obj_is_valid(qrcode_container) ||
        lv_obj_get_child_count(qrcode_container) != 1) {
        return NULL;
    }

    lv_obj_t *qrcode = lv_obj_get_child(qrcode_container, 0);
    if (!lv_obj_check_type(qrcode, &lv_qrcode_class)) {
        return NULL;
    }
    return qrcode;
}

esp_err_t qrcode_text_update(lv_obj_t *qrcode_container, const char *text)
{
    if (text == NULL || text[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    lv_obj_t *qrcode = qrcode_text_get_object(qrcode_container);
    if (qrcode == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    return lv_qrcode_update(qrcode, text, (uint32_t)strlen(text)) == LV_RESULT_OK
               ? ESP_OK
               : ESP_ERR_INVALID_SIZE;
}

lv_obj_t *qrcode_text_create_with_config(lv_obj_t *parent,
                                         const char *text,
                                         const qrcode_text_config_t *config)
{
    if (parent == NULL || text == NULL || text[0] == '\0' ||
        !config_is_valid(config)) {
        return NULL;
    }

    lv_obj_t *container = lv_obj_create(parent);
    if (container == NULL) {
        return NULL;
    }
    lv_obj_remove_style_all(container);
    lv_obj_set_size(container, config->size, config->size);
    lv_obj_set_style_bg_color(container, config->light_color, 0);
    lv_obj_set_style_bg_opa(container, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(container, 0, 0);
    lv_obj_set_style_pad_all(container, 0, 0);
    lv_obj_clear_flag(container, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *qrcode = lv_qrcode_create(container);
    if (qrcode == NULL) {
        lv_obj_delete(container);
        return NULL;
    }
    lv_qrcode_set_size(qrcode, config->size - config->quiet_zone * 2);
    lv_qrcode_set_dark_color(qrcode, config->dark_color);
    lv_qrcode_set_light_color(qrcode, config->light_color);
    lv_obj_center(qrcode);

    if (qrcode_text_update(container, text) != ESP_OK) {
        lv_obj_delete(container);
        return NULL;
    }
    return container;
}

lv_obj_t *qrcode_text_create(lv_obj_t *parent, const char *text)
{
    const qrcode_text_config_t config = {
        .size = QRCODE_TEXT_DEFAULT_SIZE,
        .quiet_zone = QRCODE_TEXT_DEFAULT_QUIET_ZONE,
        .dark_color = LV_COLOR_MAKE(0x00, 0x00, 0x00),
        .light_color = LV_COLOR_MAKE(0xFF, 0xFF, 0xFF),
    };
    return qrcode_text_create_with_config(parent, text, &config);
}

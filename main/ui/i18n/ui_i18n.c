#include "ui/i18n/ui_i18n.h"

#include <stdbool.h>
#include <stdint.h>

#include "nvs.h"
#include "ui/ttf/ui_fonts.h"

#define UI_I18N_NVS_NAMESPACE "ui_i18n"
#define UI_I18N_NVS_KEY "language"

extern const char *const ui_strings_en[UI_STR_COUNT];
extern const char *const ui_strings_zh[UI_STR_COUNT];

static ui_language_t s_language = UI_LANGUAGE_ENGLISH;
static bool s_initialized;

esp_err_t ui_i18n_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }
    s_initialized = true;

    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(UI_I18N_NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (ret != ESP_OK) {
        return ret;
    }

    uint8_t stored_language = UI_LANGUAGE_ENGLISH;
    ret = nvs_get_u8(nvs, UI_I18N_NVS_KEY, &stored_language);
    nvs_close(nvs);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (ret != ESP_OK) {
        return ret;
    }
    if (stored_language < UI_LANGUAGE_COUNT) {
        s_language = (ui_language_t)stored_language;
    }
    return ESP_OK;
}

esp_err_t ui_i18n_set_language(ui_language_t language)
{
    if (language < UI_LANGUAGE_ENGLISH || language >= UI_LANGUAGE_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }

    s_language = language;

    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(UI_I18N_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = nvs_set_u8(nvs, UI_I18N_NVS_KEY, (uint8_t)language);
    if (ret == ESP_OK) {
        ret = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return ret;
}

ui_language_t ui_i18n_get_language(void)
{
    return s_language;
}

const char *ui_i18n_get(ui_string_id_t id)
{
    if (id < 0 || id >= UI_STR_COUNT) {
        return "";
    }

    const char *const *strings = s_language == UI_LANGUAGE_CHINESE
                                     ? ui_strings_zh
                                     : ui_strings_en;
    return strings[id] ? strings[id] : ui_strings_en[id];
}

const lv_font_t *ui_i18n_font_16(void)
{
    return s_language == UI_LANGUAGE_CHINESE ? &font_cn_16 : &lv_font_montserrat_16;
}

const lv_font_t *ui_i18n_font_24(void)
{
    return s_language == UI_LANGUAGE_CHINESE ? &font_cn_24 : &lv_font_montserrat_24;
}

const lv_font_t *ui_i18n_font_28(void)
{
    return s_language == UI_LANGUAGE_CHINESE ? &font_cn_28 : &lv_font_montserrat_28;
}

const lv_font_t *ui_i18n_font_32(void)
{
    return s_language == UI_LANGUAGE_CHINESE ? &font_cn_32 : &lv_font_montserrat_32;
}

const lv_font_t *ui_i18n_font_35(void)
{
    return &font_cn_35;
}

const lv_font_t *ui_i18n_font_42(void)
{
    return s_language == UI_LANGUAGE_CHINESE ? &font_cn_42 : &lv_font_montserrat_42;
}

const lv_font_t *ui_i18n_font_64(void)
{
    return &font_cn_64;
}

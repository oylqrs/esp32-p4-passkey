#include "USB_NRF_SWITCH.h"

#include "driver/gpio.h"

#define USB_NRF_SWITCH_GPIO GPIO_NUM_29

static bool s_initialized;
static bool s_enabled;

esp_err_t usb_nrf_switch_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    const gpio_config_t config = {
        .pin_bit_mask = 1ULL << USB_NRF_SWITCH_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    esp_err_t ret = gpio_config(&config);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = gpio_set_level(USB_NRF_SWITCH_GPIO, 0);
    if (ret == ESP_OK) {
        s_enabled = false;
        s_initialized = true;
    }
    return ret;
}

esp_err_t usb_nrf_switch_set(bool enabled)
{
    esp_err_t ret = usb_nrf_switch_init();
    if (ret != ESP_OK) {
        return ret;
    }

    ret = gpio_set_level(USB_NRF_SWITCH_GPIO, enabled ? 1 : 0);
    if (ret == ESP_OK) {
        s_enabled = enabled;
    }
    return ret;
}

bool usb_nrf_switch_is_enabled(void)
{
    return s_enabled;
}

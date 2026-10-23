#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "lvgl.h"

#define DISPLAY_IDLE_DEFAULT_TIMEOUT_SECONDS 120
#define DISPLAY_IDLE_TIMEOUT_DISABLED_SECONDS 0

esp_err_t display_idle_init(lv_display_t *display);
uint16_t display_idle_get_timeout_seconds(void);
esp_err_t display_idle_set_timeout_seconds(uint16_t seconds);

/* Returns true when the current physical touch must not reach LVGL. */
bool display_idle_should_suppress_touch(bool pressed);

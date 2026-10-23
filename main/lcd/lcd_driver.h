#pragma once

#include <stdint.h>

#include "esp_err.h"
#include "esp_lcd_panel_ops.h"

#define LCD_H_RES            480
#define LCD_V_RES            640
#define LCD_BYTES_PER_PIXEL  2
#define LCD_BACKLIGHT_MIN_PERCENT 10
#define LCD_BACKLIGHT_MAX_PERCENT 100
#define LCD_BACKLIGHT_DEFAULT_PERCENT 80

esp_lcd_panel_handle_t lcd_driver_init(void);
void lcd_driver_set_backlight(uint8_t percent);
uint8_t lcd_driver_get_backlight_percent(void);
esp_err_t lcd_driver_save_backlight_percent(uint8_t percent);

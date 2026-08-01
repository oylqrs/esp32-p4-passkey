#pragma once

#include <stdint.h>

#include "esp_lcd_panel_ops.h"

#define LCD_H_RES            480
#define LCD_V_RES            640
#define LCD_BYTES_PER_PIXEL  2

esp_lcd_panel_handle_t lcd_driver_init(void);
void lcd_driver_set_backlight(uint8_t percent);
 
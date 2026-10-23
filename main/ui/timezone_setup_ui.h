#pragma once

#include <stdbool.h>
#include <stdint.h>

const char *timezone_setup_get_abbreviation(void);
int16_t timezone_setup_get_utc_offset_minutes(void);
bool timezone_setup_show_abbreviation(void);
void timezone_setup_ui_show(void);

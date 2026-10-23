#pragma once

#include "esp_err.h"

esp_err_t battery_monitor_start(void);
int battery_monitor_get_raw(void);
int battery_monitor_get_percent(void);
int battery_monitor_get_voltage_mv(void);

#pragma once

#include "esp_err.h"

// Temporary debug switch. GPIO15/16 cannot serve Chameleon while enabled.
#define DEBUG_MONITOR_UART_ENABLED 0

esp_err_t debug_monitor_uart_init(void);

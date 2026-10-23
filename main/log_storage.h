#pragma once

#include "esp_err.h"

esp_err_t log_storage_init(void);
void log_storage_refresh_time(void);
void log_storage_pause(void);
void log_storage_resume(void);

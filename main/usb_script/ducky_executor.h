#pragma once
#include <stddef.h>
#include "esp_err.h"
typedef void (*ducky_progress_cb_t)(size_t step, size_t total, void *context);
esp_err_t ducky_executor_run_file(const char *path, ducky_progress_cb_t progress_cb, void *context);

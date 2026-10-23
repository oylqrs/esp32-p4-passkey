#pragma once

#include <stddef.h>

#include "browser_autofill/browser_autofill_types.h"
#include "esp_err.h"

esp_err_t credential_matcher_find(const char *url,
                                  browser_credential_t *results,
                                  size_t capacity,
                                  size_t *count);


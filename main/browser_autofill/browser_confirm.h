#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    BROWSER_CONFIRM_PAIR,
    BROWSER_CONFIRM_CREDENTIAL_ACCESS,
} browser_confirm_kind_t;

void browser_confirm_init(void);
bool browser_confirm_request(browser_confirm_kind_t kind,
                             const char *detail,
                             uint32_t timeout_ms);


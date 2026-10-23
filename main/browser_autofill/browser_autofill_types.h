#pragma once

#include <stddef.h>

#define BROWSER_AUTOFILL_MAX_RESULTS 8
#define BROWSER_AUTOFILL_UUID_MAX 48
#define BROWSER_AUTOFILL_TITLE_MAX 96
#define BROWSER_AUTOFILL_USERNAME_MAX 96
#define BROWSER_AUTOFILL_PASSWORD_MAX 128

typedef struct {
    char uuid[BROWSER_AUTOFILL_UUID_MAX];
    char title[BROWSER_AUTOFILL_TITLE_MAX];
    char username[BROWSER_AUTOFILL_USERNAME_MAX];
    char password[BROWSER_AUTOFILL_PASSWORD_MAX];
} browser_credential_t;


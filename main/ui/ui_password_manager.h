#pragma once

#include <stdbool.h>

typedef enum {
    PASSWORD_DATABASE_MODE_NORMAL,
    PASSWORD_DATABASE_MODE_EMERGENCY,
    PASSWORD_DATABASE_MODE_DURESS,
} password_database_mode_t;

bool ui_password_manager_is_loading(void);
void ui_password_manager_notify_load_progress(void);
void ui_password_manager_reload(void);
void ui_password_manager_set_database_mode(password_database_mode_t mode);
void ui_password_manager_show(void);

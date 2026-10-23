#pragma once

#include <stdbool.h>

typedef enum {
    MAIN_MENU_ITEM_PASSWORD_MANAGER = 0,
    MAIN_MENU_ITEM_SEARCH,
    MAIN_MENU_ITEM_NEW_ENTRY,
    MAIN_MENU_ITEM_FIDO,
    MAIN_MENU_ITEM_SMARTCARD,
    MAIN_MENU_ITEM_CHAMELEON,
    MAIN_MENU_ITEM_SCRIPT,
    MAIN_MENU_ITEM_WALLET,
    MAIN_MENU_ITEM_COUNT,
} main_menu_item_id_t;

bool hide_menu_item_ui_is_visible(main_menu_item_id_t item_id);
void hide_menu_item_ui_show(void);

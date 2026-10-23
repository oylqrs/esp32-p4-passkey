/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Adapted from Blockstream Jade's dynamic dashboard menu model.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "wallet_service.h"

#define JADE_UI_MAX_MENU_ITEMS 3

typedef enum {
    JADE_UI_TEXT_SET_UP = 0,
    JADE_UI_TEXT_SCAN_SEED_QR,
    JADE_UI_TEXT_READY,
    JADE_UI_TEXT_QR_MODE,
    JADE_UI_TEXT_SESSION,
    JADE_UI_TEXT_SCAN_QR,
    JADE_UI_TEXT_OPTIONS,
} jade_ui_text_t;

typedef enum {
    JADE_UI_STATE_UNAVAILABLE = 0,
    JADE_UI_STATE_UNINITIALIZED,
    JADE_UI_STATE_INITIALIZED,
    JADE_UI_STATE_ACTIVE,
} jade_ui_state_t;

typedef struct {
    const char *symbol;
    jade_ui_text_t text;
    wallet_action_t action;
    uint32_t color;
} jade_ui_menu_item_t;

typedef struct {
    jade_ui_state_t state;
    bool operational;
    size_t item_count;
    jade_ui_menu_item_t items[JADE_UI_MAX_MENU_ITEMS];
} jade_ui_home_model_t;

jade_ui_home_model_t jade_ui_build_home_model(wallet_status_t status,
                                               wallet_capabilities_t capabilities);

/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Adapted from Blockstream Jade main/process/dashboard.c.
 */
#include "jade_ui_model.h"

#define JADE_COLOR_HIGHLIGHT 0x00A688U
#define JADE_COLOR_SECONDARY 0x3272B8U
#define JADE_COLOR_NEUTRAL   0x4B4E57U

static void add_item(jade_ui_home_model_t *model, const char *symbol,
                     jade_ui_text_t text, wallet_action_t action, uint32_t color)
{
    if (model->item_count >= JADE_UI_MAX_MENU_ITEMS) {
        return;
    }

    model->items[model->item_count++] = (jade_ui_menu_item_t){
        .symbol = symbol,
        .text = text,
        .action = action,
        .color = color,
    };
}

jade_ui_home_model_t jade_ui_build_home_model(wallet_status_t status,
                                               wallet_capabilities_t capabilities)
{
    jade_ui_home_model_t model = {
        .state = JADE_UI_STATE_UNAVAILABLE,
        .operational = status != WALLET_STATUS_UNAVAILABLE,
    };

    if (status == WALLET_STATUS_READY) {
        model.state = JADE_UI_STATE_ACTIVE;
        add_item(&model, "4", JADE_UI_TEXT_SESSION, WALLET_ACTION_SESSION,
                 JADE_COLOR_HIGHLIGHT);
        if (capabilities.qr_scan) {
            add_item(&model, "2", JADE_UI_TEXT_SCAN_QR, WALLET_ACTION_SCAN_QR,
                     JADE_COLOR_SECONDARY);
        }
    } else if (status == WALLET_STATUS_LOCKED) {
        model.state = JADE_UI_STATE_INITIALIZED;
        add_item(&model, "5", JADE_UI_TEXT_READY, WALLET_ACTION_CONNECT,
                 JADE_COLOR_HIGHLIGHT);
        if (capabilities.qr_scan) {
            add_item(&model, "2", JADE_UI_TEXT_QR_MODE, WALLET_ACTION_QR_MODE,
                     JADE_COLOR_SECONDARY);
        }
    } else {
        model.state = status == WALLET_STATUS_UNINITIALIZED
                          ? JADE_UI_STATE_UNINITIALIZED
                          : JADE_UI_STATE_UNAVAILABLE;
        add_item(&model, "1", JADE_UI_TEXT_SET_UP, WALLET_ACTION_INITIALIZE,
                 JADE_COLOR_HIGHLIGHT);
        if (capabilities.qr_scan) {
            add_item(&model, "2", JADE_UI_TEXT_SCAN_SEED_QR, WALLET_ACTION_SCAN_QR,
                     JADE_COLOR_SECONDARY);
        }
    }

    add_item(&model, "3", JADE_UI_TEXT_OPTIONS, WALLET_ACTION_SETTINGS,
             JADE_COLOR_NEUTRAL);
    return model;
}

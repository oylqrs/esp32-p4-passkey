#pragma once

#include <stdint.h>

#define CANOKEY_PIV_SLOT_COUNT 5u

int canokey_piv_select_slot(uint8_t slot);
uint8_t canokey_piv_get_slot(void);

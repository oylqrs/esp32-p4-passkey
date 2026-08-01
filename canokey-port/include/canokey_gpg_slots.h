#pragma once

#include <stdint.h>

#define CANOKEY_GPG_SLOT_COUNT 5u

int canokey_gpg_select_slot(uint8_t slot);
uint8_t canokey_gpg_get_slot(void);


#pragma once

#include <stdbool.h>
#include <stdint.h>

bool canokey_port_core_lock(uint32_t timeout_ms);
void canokey_port_core_unlock(void);
bool canokey_port_ccid_session_active(void);

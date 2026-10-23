#pragma once
#include <stdint.h>
enum {PKE_BUFFER_OWNER_NONE, PKE_BUFFER_OWNER_OPENPGP, PKE_BUFFER_OWNER_PIV, PKE_BUFFER_OWNER_CTAP};
int pke_buffer_acquire(uint8_t owner);
int pke_buffer_release(uint8_t owner);

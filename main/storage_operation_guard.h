#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    STORAGE_OPERATION_NONE = 0,
    STORAGE_OPERATION_KDBX_LOAD,
    STORAGE_OPERATION_FIRMWARE_UPDATE,
} storage_operation_t;

bool storage_operation_try_acquire(storage_operation_t operation);
void storage_operation_release(storage_operation_t operation);
storage_operation_t storage_operation_get(void);
const char *storage_operation_name(storage_operation_t operation);

#ifdef __cplusplus
}
#endif

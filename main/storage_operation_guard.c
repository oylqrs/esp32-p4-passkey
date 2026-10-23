#include "storage_operation_guard.h"

#include <stdatomic.h>

#include "esp_log.h"

static const char *TAG = "storage_guard";
static atomic_int s_operation = ATOMIC_VAR_INIT(STORAGE_OPERATION_NONE);

bool storage_operation_try_acquire(storage_operation_t operation)
{
    if (operation == STORAGE_OPERATION_NONE) {
        return false;
    }

    int expected = STORAGE_OPERATION_NONE;
    return atomic_compare_exchange_strong_explicit(&s_operation,
                                                    &expected,
                                                    operation,
                                                    memory_order_acq_rel,
                                                    memory_order_acquire);
}

void storage_operation_release(storage_operation_t operation)
{
    int expected = operation;
    if (!atomic_compare_exchange_strong_explicit(&s_operation,
                                                  &expected,
                                                  STORAGE_OPERATION_NONE,
                                                  memory_order_release,
                                                  memory_order_acquire)) {
        ESP_LOGE(TAG, "Release mismatch: requested=%s current=%s",
                 storage_operation_name(operation),
                 storage_operation_name((storage_operation_t)expected));
    }
}

storage_operation_t storage_operation_get(void)
{
    return (storage_operation_t)atomic_load_explicit(&s_operation, memory_order_acquire);
}

const char *storage_operation_name(storage_operation_t operation)
{
    switch (operation) {
    case STORAGE_OPERATION_NONE:
        return "none";
    case STORAGE_OPERATION_KDBX_LOAD:
        return "KDBX load";
    case STORAGE_OPERATION_FIRMWARE_UPDATE:
        return "firmware update";
    default:
        return "unknown";
    }
}

#pragma once

#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FIRMWARE_UPDATE_DIRECTORY "/data/update"
#define FIRMWARE_UPDATE_IMAGE_PATTERN FIRMWARE_UPDATE_DIRECTORY "/*.bin"

typedef enum {
    FIRMWARE_UPDATE_IDLE,
    FIRMWARE_UPDATE_MOUNTING,
    FIRMWARE_UPDATE_VERIFYING,
    FIRMWARE_UPDATE_REJECTED_SAME_VERSION,
    FIRMWARE_UPDATE_VERIFIED,
    FIRMWARE_UPDATE_WRITING,
    FIRMWARE_UPDATE_COMPLETED,
    FIRMWARE_UPDATE_FAILED,
} firmware_update_state_t;

typedef struct {
    firmware_update_state_t state;
    size_t bytes_written;
    size_t image_size;
    esp_err_t error;
    char version[32];
    char sha256[65];
    char running_partition[8];
    char target_partition[8];
} firmware_update_status_t;

esp_err_t firmware_update_init(void);
esp_err_t firmware_update_verify(void);
esp_err_t firmware_update_install(void);
void firmware_update_get_status(firmware_update_status_t *status);

#ifdef __cplusplus
}
#endif

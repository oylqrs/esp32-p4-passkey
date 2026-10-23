#pragma once

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CHAMELEON_COMMAND_TIMEOUT_MS 1000
#define CHAMELEON_SLOT_COUNT 8
#define CHAMELEON_HF_UID_MAX_LENGTH 10
#define CHAMELEON_LF_UID_MAX_LENGTH 16
#define CHAMELEON_NICKNAME_MAX_LENGTH 32
#define CHAMELEON_BLE_PAIRING_KEY_LENGTH 6

typedef enum {
    CHAMELEON_MODE_TAG = 0,
    CHAMELEON_MODE_READER = 1,
} chameleon_mode_t;

typedef struct {
    uint8_t major;
    uint8_t minor;
} chameleon_firmware_version_t;

typedef struct {
    uint16_t voltage_mv;
    uint8_t percent;
} chameleon_battery_info_t;

typedef struct {
    uint16_t hf_tag_type;
    uint16_t lf_tag_type;
} chameleon_slot_info_t;

typedef struct {
    chameleon_slot_info_t configured;
    char hf_nickname[CHAMELEON_NICKNAME_MAX_LENGTH + 1];
    char lf_nickname[CHAMELEON_NICKNAME_MAX_LENGTH + 1];
    uint16_t actual_hf_tag_type;
    uint16_t actual_lf_tag_type;
    uint8_t hf_uid[CHAMELEON_HF_UID_MAX_LENGTH];
    uint8_t hf_uid_length;
    uint8_t lf_uid[CHAMELEON_LF_UID_MAX_LENGTH];
    uint8_t lf_uid_length;
} chameleon_slot_details_t;

esp_err_t chameleon_get_firmware_version(chameleon_firmware_version_t *version,
                                         uint16_t *device_status);
esp_err_t chameleon_get_battery_info(chameleon_battery_info_t *info,
                                     uint16_t *device_status);
esp_err_t chameleon_get_temperature(int16_t *quarter_degrees,
                                    uint16_t *device_status);
esp_err_t chameleon_get_ble_pairing_key(
    char key[CHAMELEON_BLE_PAIRING_KEY_LENGTH + 1], uint16_t *device_status);
esp_err_t chameleon_change_mode(chameleon_mode_t mode, uint16_t *device_status);
esp_err_t chameleon_get_mode(chameleon_mode_t *mode, uint16_t *device_status);
esp_err_t chameleon_get_active_slot(uint8_t *slot, uint16_t *device_status);
esp_err_t chameleon_set_active_slot(uint8_t slot, uint16_t *device_status);
esp_err_t chameleon_get_slot_info(uint8_t slot,
                                  chameleon_slot_info_t *info,
                                  uint16_t *device_status);
esp_err_t chameleon_get_slot_details(uint8_t slot,
                                     chameleon_slot_details_t *details,
                                     uint16_t *device_status);

#ifdef __cplusplus
}
#endif

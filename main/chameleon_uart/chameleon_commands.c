#include "chameleon_commands.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "chameleon_uart.h"

#define CHAMELEON_CMD_GET_FIRMWARE_VERSION 1000
#define CHAMELEON_CMD_GET_BATTERY_INFO 1025
#define CHAMELEON_CMD_GET_TEMPERATURE 1041
#define CHAMELEON_CMD_GET_BLE_PAIRING_KEY 1031
#define CHAMELEON_CMD_CHANGE_MODE 1001
#define CHAMELEON_CMD_GET_MODE 1002
#define CHAMELEON_CMD_SET_ACTIVE_SLOT 1003
#define CHAMELEON_CMD_GET_SLOT_NICKNAME 1008
#define CHAMELEON_CMD_GET_ACTIVE_SLOT 1018
#define CHAMELEON_CMD_GET_SLOT_INFO 1019
#define CHAMELEON_CMD_GET_HF_ANTI_COLLISION 4018
#define CHAMELEON_CMD_GET_EM410X_ID 5001
#define CHAMELEON_CMD_GET_HID_PROX_ID 5003
#define CHAMELEON_CMD_GET_VIKING_ID 5005
#define CHAMELEON_CMD_GET_PAC_ID 5007
#define CHAMELEON_CMD_GET_IOPROX_ID 5009
#define CHAMELEON_CMD_GET_IDTECK_ID 5013
#define CHAMELEON_SLOT_INFO_SIZE 4
#define CHAMELEON_SENSE_LF 1
#define CHAMELEON_SENSE_HF 2

static esp_err_t check_response(esp_err_t ret,
                                const chameleon_uart_response_t *response,
                                uint16_t expected_length,
                                uint16_t *device_status)
{
    if (ret != ESP_OK) {
        return ret;
    }

    if (device_status != NULL) {
        *device_status = response->status;
    }
    if (response->status != CHAMELEON_STATUS_SUCCESS) {
        return ESP_ERR_INVALID_STATE;
    }
    if (response->length != expected_length) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}

esp_err_t chameleon_get_firmware_version(chameleon_firmware_version_t *version,
                                         uint16_t *device_status)
{
    if (version == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t data[2];
    chameleon_uart_response_t response = {
        .data = data,
        .capacity = sizeof(data),
    };
    esp_err_t ret = chameleon_uart_transceive(CHAMELEON_CMD_GET_FIRMWARE_VERSION,
                                              NULL,
                                              0,
                                              &response,
                                              CHAMELEON_COMMAND_TIMEOUT_MS);
    ret = check_response(ret, &response, sizeof(data), device_status);
    if (ret == ESP_OK) {
        version->major = data[0];
        version->minor = data[1];
    }
    return ret;
}

esp_err_t chameleon_get_battery_info(chameleon_battery_info_t *info,
                                     uint16_t *device_status)
{
    if (info == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t data[3];
    chameleon_uart_response_t response = {
        .data = data,
        .capacity = sizeof(data),
    };
    esp_err_t ret = chameleon_uart_transceive(CHAMELEON_CMD_GET_BATTERY_INFO,
                                              NULL,
                                              0,
                                              &response,
                                              CHAMELEON_COMMAND_TIMEOUT_MS);
    ret = check_response(ret, &response, sizeof(data), device_status);
    if (ret == ESP_OK) {
        uint8_t percent = data[2];
        if (percent > 100) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        info->voltage_mv = (uint16_t)(((uint16_t)data[0] << 8) | data[1]);
        info->percent = percent;
    }
    return ret;
}

esp_err_t chameleon_get_temperature(int16_t *quarter_degrees,
                                    uint16_t *device_status)
{
    if (quarter_degrees == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t data[2];
    chameleon_uart_response_t response = {
        .data = data,
        .capacity = sizeof(data),
    };
    esp_err_t ret = chameleon_uart_transceive(CHAMELEON_CMD_GET_TEMPERATURE,
                                              NULL,
                                              0,
                                              &response,
                                              CHAMELEON_COMMAND_TIMEOUT_MS);
    ret = check_response(ret, &response, sizeof(data), device_status);
    if (ret == ESP_OK) {
        uint16_t encoded = (uint16_t)(((uint16_t)data[0] << 8) | data[1]);
        *quarter_degrees = (int16_t)encoded;
    }
    return ret;
}

esp_err_t chameleon_get_ble_pairing_key(
    char key[CHAMELEON_BLE_PAIRING_KEY_LENGTH + 1], uint16_t *device_status)
{
    if (key == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t data[CHAMELEON_BLE_PAIRING_KEY_LENGTH];
    chameleon_uart_response_t response = {
        .data = data,
        .capacity = sizeof(data),
    };
    esp_err_t ret = chameleon_uart_transceive(
        CHAMELEON_CMD_GET_BLE_PAIRING_KEY, NULL, 0, &response,
        CHAMELEON_COMMAND_TIMEOUT_MS);
    ret = check_response(ret, &response, sizeof(data), device_status);
    if (ret != ESP_OK) {
        return ret;
    }
    for (size_t i = 0; i < sizeof(data); i++) {
        if (data[i] < '0' || data[i] > '9') {
            return ESP_ERR_INVALID_RESPONSE;
        }
    }
    memcpy(key, data, sizeof(data));
    key[sizeof(data)] = '\0';
    return ESP_OK;
}

esp_err_t chameleon_change_mode(chameleon_mode_t mode, uint16_t *device_status)
{
    if (mode != CHAMELEON_MODE_TAG && mode != CHAMELEON_MODE_READER) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t request = (uint8_t)mode;
    chameleon_uart_response_t response = {0};
    esp_err_t ret = chameleon_uart_transceive(CHAMELEON_CMD_CHANGE_MODE,
                                              &request,
                                              sizeof(request),
                                              &response,
                                              CHAMELEON_COMMAND_TIMEOUT_MS);
    return check_response(ret, &response, 0, device_status);
}

esp_err_t chameleon_get_mode(chameleon_mode_t *mode, uint16_t *device_status)
{
    if (mode == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t data;
    chameleon_uart_response_t response = {
        .data = &data,
        .capacity = sizeof(data),
    };
    esp_err_t ret = chameleon_uart_transceive(CHAMELEON_CMD_GET_MODE,
                                              NULL,
                                              0,
                                              &response,
                                              CHAMELEON_COMMAND_TIMEOUT_MS);
    ret = check_response(ret, &response, sizeof(data), device_status);
    if (ret == ESP_OK) {
        if (data > CHAMELEON_MODE_READER) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        *mode = (chameleon_mode_t)data;
    }
    return ret;
}

esp_err_t chameleon_get_active_slot(uint8_t *slot, uint16_t *device_status)
{
    if (slot == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t data;
    chameleon_uart_response_t response = {
        .data = &data,
        .capacity = sizeof(data),
    };
    esp_err_t ret = chameleon_uart_transceive(CHAMELEON_CMD_GET_ACTIVE_SLOT,
                                              NULL,
                                              0,
                                              &response,
                                              CHAMELEON_COMMAND_TIMEOUT_MS);
    ret = check_response(ret, &response, sizeof(data), device_status);
    if (ret == ESP_OK) {
        if (data >= CHAMELEON_SLOT_COUNT) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        *slot = data;
    }
    return ret;
}

esp_err_t chameleon_set_active_slot(uint8_t slot, uint16_t *device_status)
{
    if (slot >= CHAMELEON_SLOT_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }

    chameleon_uart_response_t response = {0};
    esp_err_t ret = chameleon_uart_transceive(CHAMELEON_CMD_SET_ACTIVE_SLOT,
                                              &slot,
                                              sizeof(slot),
                                              &response,
                                              CHAMELEON_COMMAND_TIMEOUT_MS);
    return check_response(ret, &response, 0, device_status);
}

esp_err_t chameleon_get_slot_info(uint8_t slot,
                                  chameleon_slot_info_t *info,
                                  uint16_t *device_status)
{
    if (slot >= CHAMELEON_SLOT_COUNT || info == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t data[CHAMELEON_SLOT_COUNT * CHAMELEON_SLOT_INFO_SIZE];
    chameleon_uart_response_t response = {
        .data = data,
        .capacity = sizeof(data),
    };
    esp_err_t ret = chameleon_uart_transceive(CHAMELEON_CMD_GET_SLOT_INFO,
                                              NULL,
                                              0,
                                              &response,
                                              CHAMELEON_COMMAND_TIMEOUT_MS);
    ret = check_response(ret, &response, sizeof(data), device_status);
    if (ret == ESP_OK) {
        size_t offset = slot * CHAMELEON_SLOT_INFO_SIZE;
        info->hf_tag_type = (uint16_t)(((uint16_t)data[offset] << 8) | data[offset + 1]);
        info->lf_tag_type = (uint16_t)(((uint16_t)data[offset + 2] << 8) | data[offset + 3]);
    }
    return ret;
}

static uint16_t lf_identity_command(uint16_t type)
{
    if (type >= 100 && type <= 104) {
        return CHAMELEON_CMD_GET_EM410X_ID;
    }
    switch (type) {
    case 150: return CHAMELEON_CMD_GET_PAC_ID;
    case 170: return CHAMELEON_CMD_GET_VIKING_ID;
    case 200: return CHAMELEON_CMD_GET_HID_PROX_ID;
    case 201: return CHAMELEON_CMD_GET_IOPROX_ID;
    case 310: return CHAMELEON_CMD_GET_IDTECK_ID;
    default: return 0;
    }
}

static esp_err_t receive_identity(uint16_t command,
                                  uint8_t *data,
                                  size_t capacity,
                                  uint16_t *length,
                                  uint16_t *device_status)
{
    chameleon_uart_response_t response = {
        .data = data,
        .capacity = capacity,
    };
    esp_err_t ret = chameleon_uart_transceive(command,
                                              NULL,
                                              0,
                                              &response,
                                              CHAMELEON_COMMAND_TIMEOUT_MS);
    if (ret != ESP_OK) {
        return ret;
    }
    if (device_status != NULL) {
        *device_status = response.status;
    }
    if (response.status != CHAMELEON_STATUS_SUCCESS) {
        return ESP_ERR_INVALID_STATE;
    }
    *length = response.length;
    return ESP_OK;
}

static esp_err_t get_slot_nickname(uint8_t slot,
                                   uint8_t sense_type,
                                   char *nickname,
                                   uint16_t *device_status)
{
    uint8_t request[2] = {slot, sense_type};
    chameleon_uart_response_t response = {
        .data = (uint8_t *)nickname,
        .capacity = CHAMELEON_NICKNAME_MAX_LENGTH,
    };
    esp_err_t ret = chameleon_uart_transceive(CHAMELEON_CMD_GET_SLOT_NICKNAME,
                                              request,
                                              sizeof(request),
                                              &response,
                                              CHAMELEON_COMMAND_TIMEOUT_MS);
    if (ret != ESP_OK) {
        return ret;
    }
    if (device_status != NULL) {
        *device_status = response.status;
    }
    if (response.status != CHAMELEON_STATUS_SUCCESS) {
        return ESP_ERR_INVALID_STATE;
    }
    nickname[response.length] = '\0';
    return ESP_OK;
}

esp_err_t chameleon_get_slot_details(uint8_t slot,
                                     chameleon_slot_details_t *details,
                                     uint16_t *device_status)
{
    if (slot >= CHAMELEON_SLOT_COUNT || details == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(details, 0, sizeof(*details));
    esp_err_t ret = chameleon_get_slot_info(slot, &details->configured, device_status);
    if (ret != ESP_OK) {
        return ret;
    }
    details->actual_hf_tag_type = details->configured.hf_tag_type;
    details->actual_lf_tag_type = details->configured.lf_tag_type;
    if (get_slot_nickname(slot, CHAMELEON_SENSE_HF,
                          details->hf_nickname, device_status) != ESP_OK) {
        details->hf_nickname[0] = '\0';
    }
    if (get_slot_nickname(slot, CHAMELEON_SENSE_LF,
                          details->lf_nickname, device_status) != ESP_OK) {
        details->lf_nickname[0] = '\0';
    }

    uint8_t original_slot;
    ret = chameleon_get_active_slot(&original_slot, device_status);
    if (ret != ESP_OK) {
        return ret;
    }
    bool restore_slot = original_slot != slot;
    if (restore_slot) {
        ret = chameleon_set_active_slot(slot, device_status);
        if (ret != ESP_OK) {
            return ret;
        }
    }

    esp_err_t result = ESP_OK;
    if (details->configured.hf_tag_type != 0) {
        uint8_t hf_data[1 + CHAMELEON_HF_UID_MAX_LENGTH + 2 + 1 + 1 + 254];
        uint16_t hf_length = 0;
        ret = receive_identity(CHAMELEON_CMD_GET_HF_ANTI_COLLISION,
                               hf_data, sizeof(hf_data), &hf_length, device_status);
        if (ret == ESP_OK && hf_length > 0 && hf_data[0] <= CHAMELEON_HF_UID_MAX_LENGTH &&
            hf_length >= (uint16_t)(1 + hf_data[0])) {
            details->hf_uid_length = hf_data[0];
            memcpy(details->hf_uid, &hf_data[1], details->hf_uid_length);
        } else if (ret != ESP_OK) {
            result = ret;
        }
    }

    uint16_t lf_command = lf_identity_command(details->configured.lf_tag_type);
    if (lf_command != 0) {
        uint8_t lf_data[2 + CHAMELEON_LF_UID_MAX_LENGTH];
        uint16_t lf_length = 0;
        ret = receive_identity(lf_command, lf_data, sizeof(lf_data), &lf_length, device_status);
        if (ret == ESP_OK) {
            size_t offset = 0;
            if (lf_command == CHAMELEON_CMD_GET_EM410X_ID && lf_length >= 2) {
                details->actual_lf_tag_type = (uint16_t)(((uint16_t)lf_data[0] << 8) | lf_data[1]);
                offset = 2;
            }
            size_t uid_length = lf_length - offset;
            if (uid_length <= sizeof(details->lf_uid)) {
                details->lf_uid_length = uid_length;
                memcpy(details->lf_uid, &lf_data[offset], uid_length);
            }
        } else if (result == ESP_OK) {
            result = ret;
        }
    }

    if (restore_slot) {
        ret = chameleon_set_active_slot(original_slot, device_status);
        if (result == ESP_OK && ret != ESP_OK) {
            result = ret;
        }
    }
    return result;
}

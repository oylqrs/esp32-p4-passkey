#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CHAMELEON_UART_BAUD_RATE 115200
#define CHAMELEON_UART_MAX_DATA_LENGTH 4096
#define CHAMELEON_STATUS_SUCCESS 0x0068

typedef struct {
    uint16_t command;
    uint16_t status;
    uint16_t length;
    uint8_t *data;
    size_t capacity;
} chameleon_uart_response_t;

esp_err_t chameleon_uart_init(void);
esp_err_t chameleon_uart_nfc_transceive(uint16_t command,
                                       const uint8_t *request, uint16_t request_length,
                                       uint8_t *response, uint16_t response_capacity,
                                       uint16_t *response_length, uint16_t *status,
                                       uint32_t timeout_ms);
int chameleon_uart_write(const void *data, size_t length);
int chameleon_uart_read(void *data, size_t length, uint32_t timeout_ms);
esp_err_t chameleon_uart_transceive(uint16_t command,
                                    const void *request_data,
                                    uint16_t request_length,
                                    chameleon_uart_response_t *response,
                                    uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif

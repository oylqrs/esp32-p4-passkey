#include "chameleon_uart.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define CHAMELEON_UART_PORT UART_NUM_1
#define CHAMELEON_UART_TX_PIN GPIO_NUM_15
#define CHAMELEON_UART_RX_PIN GPIO_NUM_16
#define CHAMELEON_UART_RX_BUFFER_SIZE (CHAMELEON_UART_MAX_DATA_LENGTH + CHAMELEON_FRAME_OVERHEAD)
#define CHAMELEON_FRAME_SOF 0x11
#define CHAMELEON_FRAME_HEADER_SIZE 9
#define CHAMELEON_FRAME_OVERHEAD 10
#define CHAMELEON_UART_TRACE_RX 0

static const char *TAG = "chameleon_uart";
static bool s_initialized;
static SemaphoreHandle_t s_transaction_mutex;

static void trace_rx_bytes(const char *part, const uint8_t *data, size_t length)
{
#if CHAMELEON_UART_TRACE_RX
    ESP_LOGI(TAG, "RX %s: len=%u", part, (unsigned int)length);
    if (length > 0) ESP_LOG_BUFFER_HEX_LEVEL(TAG, data, length, ESP_LOG_INFO);
#else
    (void)part;
    (void)data;
    (void)length;
#endif
}

static uint8_t compute_lrc(const uint8_t *data, size_t length)
{
    uint8_t sum = 0;
    for (size_t index = 0; index < length; index++) {
        sum = (uint8_t)(sum + data[index]);
    }
    return (uint8_t)(0U - sum);
}

static uint16_t read_be16(const uint8_t *data)
{
    return (uint16_t)(((uint16_t)data[0] << 8) | data[1]);
}

static void write_be16(uint8_t *data, uint16_t value)
{
    data[0] = (uint8_t)(value >> 8);
    data[1] = (uint8_t)value;
}

static esp_err_t read_exact(uint8_t *data, size_t length, TickType_t deadline)
{
    size_t position = 0;
    while (position < length) {
        TickType_t now = xTaskGetTickCount();
        if ((int32_t)(deadline - now) <= 0) {
            return ESP_ERR_TIMEOUT;
        }

        int received = uart_read_bytes(CHAMELEON_UART_PORT,
                                       data + position,
                                       length - position,
                                       deadline - now);
        if (received < 0) {
            return ESP_FAIL;
        }
        if (received == 0) {
            return ESP_ERR_TIMEOUT;
        }
        position += (size_t)received;
    }
    return ESP_OK;
}

esp_err_t chameleon_uart_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    const uart_config_t config = {
        .baud_rate = CHAMELEON_UART_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    esp_err_t ret = uart_param_config(CHAMELEON_UART_PORT, &config);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = uart_set_pin(CHAMELEON_UART_PORT,
                       CHAMELEON_UART_TX_PIN,
                       CHAMELEON_UART_RX_PIN,
                       UART_PIN_NO_CHANGE,
                       UART_PIN_NO_CHANGE);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = uart_driver_install(CHAMELEON_UART_PORT,
                              CHAMELEON_UART_RX_BUFFER_SIZE,
                              0,
                              0,
                              NULL,
                              0);
    if (ret != ESP_OK) {
        return ret;
    }

    s_transaction_mutex = xSemaphoreCreateMutex();
    if (s_transaction_mutex == NULL) {
        uart_driver_delete(CHAMELEON_UART_PORT);
        return ESP_ERR_NO_MEM;
    }

    s_initialized = true;
    return ESP_OK;
}

int chameleon_uart_write(const void *data, size_t length)
{
    if (!s_initialized || data == NULL || length == 0) {
        return -1;
    }

    return uart_write_bytes(CHAMELEON_UART_PORT, data, length);
}

int chameleon_uart_read(void *data, size_t length, uint32_t timeout_ms)
{
    if (!s_initialized || data == NULL || length == 0) {
        return -1;
    }

    return uart_read_bytes(CHAMELEON_UART_PORT,
                           data,
                           length,
                           pdMS_TO_TICKS(timeout_ms));
}

esp_err_t chameleon_uart_transceive(uint16_t command,
                                    const void *request_data,
                                    uint16_t request_length,
                                    chameleon_uart_response_t *response,
                                    uint32_t timeout_ms)
{
    if (!s_initialized || response == NULL || timeout_ms == 0 ||
        request_length > CHAMELEON_UART_MAX_DATA_LENGTH ||
        (request_length > 0 && request_data == NULL) ||
        (response->capacity > 0 && response->data == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }

    if (xSemaphoreTake(s_transaction_mutex, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    response->command = 0;
    response->status = 0;
    response->length = 0;

    esp_err_t ret = ESP_OK;
    size_t frame_length = CHAMELEON_FRAME_OVERHEAD + request_length;
    uint8_t *frame = malloc(frame_length);
    if (frame == NULL) {
        ret = ESP_ERR_NO_MEM;
        goto done;
    }

    frame[0] = CHAMELEON_FRAME_SOF;
    frame[1] = compute_lrc(frame, 1);
    write_be16(&frame[2], command);
    write_be16(&frame[4], 0);
    write_be16(&frame[6], request_length);
    frame[8] = compute_lrc(frame, 8);
    if (request_length > 0) {
        memcpy(&frame[9], request_data, request_length);
    }
    frame[9 + request_length] = compute_lrc(&frame[9], request_length);

    uart_flush_input(CHAMELEON_UART_PORT);
    int written = uart_write_bytes(CHAMELEON_UART_PORT, frame, frame_length);
    free(frame);
    if (written != (int)frame_length) {
        ret = ESP_FAIL;
        goto done;
    }

    TickType_t timeout_ticks = pdMS_TO_TICKS(timeout_ms);
    if (timeout_ticks == 0) {
        timeout_ticks = 1;
    }
    TickType_t deadline = xTaskGetTickCount() + timeout_ticks;

    uint8_t header[CHAMELEON_FRAME_HEADER_SIZE];
    do {
        ret = read_exact(&header[0], 1, deadline);
        if (ret != ESP_OK) {
            goto done;
        }
        if (header[0] != CHAMELEON_FRAME_SOF) {
            trace_rx_bytes("discarded before SOF", &header[0], 1);
        }
    } while (header[0] != CHAMELEON_FRAME_SOF);

    ret = read_exact(&header[1], CHAMELEON_FRAME_HEADER_SIZE - 1, deadline);
    if (ret != ESP_OK) {
        goto done;
    }
    trace_rx_bytes("header", header, sizeof(header));
    if (compute_lrc(header, 1) != header[1] || compute_lrc(header, 8) != header[8]) {
        ret = ESP_ERR_INVALID_CRC;
        goto done;
    }

    response->command = read_be16(&header[2]);
    response->status = read_be16(&header[4]);
    response->length = read_be16(&header[6]);
    ESP_LOGD(TAG, "RX frame: request_cmd=0x%04x response_cmd=0x%04x status=0x%04x data_len=%u",
             command, response->command, response->status, response->length);
    if (response->command != command || response->length > CHAMELEON_UART_MAX_DATA_LENGTH) {
        ret = ESP_ERR_INVALID_RESPONSE;
        goto done;
    }

    bool response_fits = response->length <= response->capacity;
    uint8_t scratch[64];
    uint8_t data_sum = 0;
    size_t remaining = response->length;
    size_t position = 0;
    while (remaining > 0) {
        size_t chunk_length = remaining > sizeof(scratch) ? sizeof(scratch) : remaining;
        uint8_t *destination = response_fits ? response->data + position : scratch;
        ret = read_exact(destination, chunk_length, deadline);
        if (ret != ESP_OK) {
            goto done;
        }
        ESP_LOGD(TAG, "RX data offset=%u", (unsigned int)position);
        trace_rx_bytes("data", destination, chunk_length);
        for (size_t index = 0; index < chunk_length; index++) {
            data_sum = (uint8_t)(data_sum + destination[index]);
        }
        position += chunk_length;
        remaining -= chunk_length;
    }

    uint8_t received_lrc;
    ret = read_exact(&received_lrc, 1, deadline);
    if (ret != ESP_OK) {
        goto done;
    }
    trace_rx_bytes("data LRC", &received_lrc, 1);
    if ((uint8_t)(data_sum + received_lrc) != 0) {
        ret = ESP_ERR_INVALID_CRC;
        goto done;
    }
    if (!response_fits) {
        ret = ESP_ERR_INVALID_SIZE;
    }

done:
    xSemaphoreGive(s_transaction_mutex);
    return ret;
}

esp_err_t chameleon_uart_nfc_transceive(uint16_t command,
                                       const uint8_t *request, uint16_t request_length,
                                       uint8_t *response, uint16_t response_capacity,
                                       uint16_t *response_length, uint16_t *status,
                                       uint32_t timeout_ms)
{
    if (!response_length || !status) return ESP_ERR_INVALID_ARG;
    chameleon_uart_response_t packet = {.data = response, .capacity = response_capacity};
    esp_err_t ret = chameleon_uart_transceive(command, request, request_length, &packet, timeout_ms);
    *response_length = packet.length;
    *status = packet.status;
    return ret;
}

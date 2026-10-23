#include "debug_monitor_uart.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define DEBUG_MONITOR_PORT UART_NUM_1
#define DEBUG_MONITOR_TX GPIO_NUM_15
#define DEBUG_MONITOR_RX GPIO_NUM_16
#define DEBUG_MONITOR_BAUD 115200
#define DEBUG_MONITOR_LINE_SIZE 256
#define DEBUG_MONITOR_LINE_COUNT 16

static const char *TAG = "debug_uart";
static vprintf_like_t s_previous_vprintf;
static portMUX_TYPE s_lines_mux = portMUX_INITIALIZER_UNLOCKED;
static char s_lines[DEBUG_MONITOR_LINE_COUNT][DEBUG_MONITOR_LINE_SIZE];
static size_t s_head;
static size_t s_count;

static void debug_monitor_task(void *arg)
{
    (void)arg;
    int64_t last_heartbeat_us = esp_timer_get_time();
    while (true) {
        char line[DEBUG_MONITOR_LINE_SIZE];
        bool pending = false;
        portENTER_CRITICAL(&s_lines_mux);
        if (s_count > 0) {
            memcpy(line, s_lines[s_head], sizeof(line));
            s_head = (s_head + 1) % DEBUG_MONITOR_LINE_COUNT;
            s_count--;
            pending = true;
        }
        portEXIT_CRITICAL(&s_lines_mux);

        if (pending) {
            uart_write_bytes(DEBUG_MONITOR_PORT, line, strlen(line));
        } else {
            vTaskDelay(pdMS_TO_TICKS(10));
        }

        int64_t now_us = esp_timer_get_time();
        if (now_us - last_heartbeat_us >= 10000000) {
            last_heartbeat_us = now_us;
            ESP_LOGI(TAG, "Heartbeat uptime=%llds, free_heap=%lu, min_free_heap=%lu",
                     (long long)(now_us / 1000000),
                     (unsigned long)esp_get_free_heap_size(),
                     (unsigned long)esp_get_minimum_free_heap_size());
        }
    }
}

static int debug_monitor_vprintf(const char *format, va_list args)
{
    char line[DEBUG_MONITOR_LINE_SIZE] = {0};
    va_list copy;
    va_copy(copy, args);
    int length = vsnprintf(line, sizeof(line), format, copy);
    va_end(copy);

    int result = s_previous_vprintf(format, args);
    if (length > 0) {
        if (length >= sizeof(line)) {
            line[sizeof(line) - 2] = '\n';
            line[sizeof(line) - 1] = '\0';
        }
        portENTER_CRITICAL(&s_lines_mux);
        if (s_count < DEBUG_MONITOR_LINE_COUNT) {
            size_t tail = (s_head + s_count) % DEBUG_MONITOR_LINE_COUNT;
            memcpy(s_lines[tail], line, sizeof(line));
            s_count++;
        }
        portEXIT_CRITICAL(&s_lines_mux);
    }
    return result;
}

esp_err_t debug_monitor_uart_init(void)
{
    const uart_config_t config = {
        .baud_rate = DEBUG_MONITOR_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    esp_err_t ret = uart_param_config(DEBUG_MONITOR_PORT, &config);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = uart_set_pin(DEBUG_MONITOR_PORT, DEBUG_MONITOR_TX, DEBUG_MONITOR_RX,
                       UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = uart_driver_install(DEBUG_MONITOR_PORT, 1024, 2048, 0, NULL, 0);
    if (ret != ESP_OK) {
        return ret;
    }

    if (xTaskCreate(debug_monitor_task, "debug_monitor", 3072,
                    NULL, 1, NULL) != pdPASS) {
        uart_driver_delete(DEBUG_MONITOR_PORT);
        return ESP_ERR_NO_MEM;
    }
    s_previous_vprintf = esp_log_set_vprintf(debug_monitor_vprintf);
    ESP_LOGI(TAG, "GPIO15 TX / GPIO16 RX, 115200 8N1, reset reason=%d",
             esp_reset_reason());
    return ESP_OK;
}

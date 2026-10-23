#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint16_t year;
    uint8_t month;
    uint8_t day;
    uint8_t weekday;
    uint8_t hour;
    uint8_t minute;
    uint8_t second;
    bool voltage_low;
} pcf85063_time_t;

esp_err_t pcf85063_init(void);
esp_err_t pcf85063_get_time(pcf85063_time_t *time);
esp_err_t pcf85063_set_time(const pcf85063_time_t *time);

#ifdef __cplusplus
}
#endif

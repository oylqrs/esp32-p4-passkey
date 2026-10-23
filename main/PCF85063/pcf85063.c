#include "pcf85063.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/i2c.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

#define PCF85063_I2C_PORT I2C_NUM_1
#define PCF85063_I2C_ADDRESS 0x51
#define PCF85063_I2C_SDA_GPIO GPIO_NUM_21
#define PCF85063_I2C_SCL_GPIO GPIO_NUM_22
#define PCF85063_I2C_FREQUENCY_HZ 100000
#define PCF85063_REG_CONTROL_1 0x00
#define PCF85063_REG_SECONDS 0x04
#define PCF85063_TIME_REGISTER_COUNT 7

static const char *TAG = "pcf85063";
static bool s_initialized;

static esp_err_t set_build_time(void)
{
    static const char *months = "JanFebMarAprMayJunJulAugSepOctNovDec";
    char month_text[4] = {0};
    unsigned int day;
    unsigned int year;
    unsigned int hour;
    unsigned int minute;
    unsigned int second;
    if (sscanf(__DATE__, "%3s %u %u", month_text, &day, &year) != 3 ||
        sscanf(__TIME__, "%u:%u:%u", &hour, &minute, &second) != 3) {
        return ESP_FAIL;
    }

    const char *month_position = strstr(months, month_text);
    if (month_position == NULL) {
        return ESP_FAIL;
    }

    pcf85063_time_t time = {
        .year = (uint16_t)year,
        .month = (uint8_t)(((month_position - months) / 3) + 1),
        .day = (uint8_t)day,
        .weekday = 0,
        .hour = (uint8_t)hour,
        .minute = (uint8_t)minute,
        .second = (uint8_t)second,
    };
    ESP_LOGW(TAG, "RTC time invalid; initializing from build time %04u-%02u-%02u %02u:%02u:%02u",
             time.year, time.month, time.day, time.hour, time.minute, time.second);
    return pcf85063_set_time(&time);
}

static uint8_t bcd_to_binary(uint8_t value)
{
    return (uint8_t)(((value >> 4) * 10) + (value & 0x0F));
}

static uint8_t binary_to_bcd(uint8_t value)
{
    return (uint8_t)(((value / 10) << 4) | (value % 10));
}

static bool time_is_valid(const pcf85063_time_t *time)
{
    return time != NULL && time->year >= 2000 && time->year <= 2099 &&
           time->month >= 1 && time->month <= 12 &&
           time->day >= 1 && time->day <= 31 && time->weekday <= 6 &&
           time->hour <= 23 && time->minute <= 59 && time->second <= 59;
}

esp_err_t pcf85063_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    const i2c_config_t config = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = PCF85063_I2C_SDA_GPIO,
        .scl_io_num = PCF85063_I2C_SCL_GPIO,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = PCF85063_I2C_FREQUENCY_HZ,
        .clk_flags = 0,
    };

    ESP_RETURN_ON_ERROR(i2c_param_config(PCF85063_I2C_PORT, &config),
                        TAG, "configure I2C failed");
    esp_err_t ret = i2c_driver_install(PCF85063_I2C_PORT, I2C_MODE_MASTER, 0, 0, 0);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        return ret;
    }

    uint8_t control_1;
    uint8_t reg = PCF85063_REG_CONTROL_1;
    ESP_RETURN_ON_ERROR(i2c_master_write_read_device(PCF85063_I2C_PORT,
                                                     PCF85063_I2C_ADDRESS,
                                                     &reg, 1,
                                                     &control_1, 1,
                                                     pdMS_TO_TICKS(100)),
                        TAG, "RTC not responding");
    s_initialized = true;
    ESP_LOGI(TAG, "PCF85063 detected at 0x%02X, SDA=%d SCL=%d",
             PCF85063_I2C_ADDRESS, PCF85063_I2C_SDA_GPIO, PCF85063_I2C_SCL_GPIO);

    pcf85063_time_t time;
    ret = pcf85063_get_time(&time);
    if (ret != ESP_OK || time.voltage_low) {
        ret = set_build_time();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to initialize RTC time: %s", esp_err_to_name(ret));
            return ret;
        }
    }
    return ESP_OK;
}

esp_err_t pcf85063_get_time(pcf85063_time_t *time)
{
    if (time == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(pcf85063_init(), TAG, "RTC init failed");

    uint8_t reg = PCF85063_REG_SECONDS;
    uint8_t data[PCF85063_TIME_REGISTER_COUNT];
    ESP_RETURN_ON_ERROR(i2c_master_write_read_device(PCF85063_I2C_PORT,
                                                     PCF85063_I2C_ADDRESS,
                                                     &reg, 1,
                                                     data, sizeof(data),
                                                     pdMS_TO_TICKS(100)),
                        TAG, "read time failed");

    time->voltage_low = (data[0] & 0x80) != 0;
    time->second = bcd_to_binary(data[0] & 0x7F);
    time->minute = bcd_to_binary(data[1] & 0x7F);
    time->hour = bcd_to_binary(data[2] & 0x3F);
    time->day = bcd_to_binary(data[3] & 0x3F);
    time->weekday = data[4] & 0x07;
    time->month = bcd_to_binary(data[5] & 0x1F);
    time->year = (uint16_t)(2000 + bcd_to_binary(data[6]));
    if (!time_is_valid(time)) {
        ESP_LOGW(TAG, "Invalid RTC registers: %02X %02X %02X %02X %02X %02X %02X",
                 data[0], data[1], data[2], data[3], data[4], data[5], data[6]);
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}

esp_err_t pcf85063_set_time(const pcf85063_time_t *time)
{
    if (!time_is_valid(time)) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(pcf85063_init(), TAG, "RTC init failed");

    uint8_t data[1 + PCF85063_TIME_REGISTER_COUNT] = {
        PCF85063_REG_SECONDS,
        binary_to_bcd(time->second),
        binary_to_bcd(time->minute),
        binary_to_bcd(time->hour),
        binary_to_bcd(time->day),
        time->weekday,
        binary_to_bcd(time->month),
        binary_to_bcd((uint8_t)(time->year - 2000)),
    };
    return i2c_master_write_to_device(PCF85063_I2C_PORT,
                                      PCF85063_I2C_ADDRESS,
                                      data, sizeof(data),
                                      pdMS_TO_TICKS(100));
}

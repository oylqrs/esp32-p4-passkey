#include "ADC/battery_monitor.h"

#include <stdbool.h>
#include <stdint.h>

#include "chameleon_uart/chameleon_commands.h"
#include "driver/gpio.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define BATTERY_MONITOR_USE_CHAMELEON 1
#define BATTERY_MONITOR_GPIO GPIO_NUM_18
#define BATTERY_MONITOR_INTERVAL_MS 5000
#define BATTERY_MONITOR_SAMPLE_COUNT 8
#define BATTERY_MONITOR_TASK_STACK 3072
#define BATTERY_MONITOR_TASK_PRIORITY 2
#define BATTERY_MONITOR_ATTEN ADC_ATTEN_DB_12
#define BATTERY_MONITOR_BITWIDTH ADC_BITWIDTH_DEFAULT
#define BATTERY_MONITOR_VOLTAGE_DIVIDER_RATIO 2.0f
#define BATTERY_MONITOR_ADC_RAW_MAX 4095
#define BATTERY_MONITOR_ADC_REF_MV  3300
#define BATTERY_MONITOR_EMPTY_MV 3300
#define BATTERY_MONITOR_FULL_MV  4200

static const char *TAG = "battery";
static TaskHandle_t s_battery_task_handle;
static volatile int s_battery_raw = -1;
static volatile int s_battery_voltage_mv = -1;
static volatile int s_battery_percent = -1;

#if !BATTERY_MONITOR_USE_CHAMELEON
typedef struct {
    adc_oneshot_unit_handle_t adc_handle;
    adc_cali_handle_t cali_handle;
    bool calibrated;
    adc_channel_t channel;
} battery_monitor_ctx_t;

static int battery_monitor_voltage_to_percent(int battery_mv)
{
    if (battery_mv <= BATTERY_MONITOR_EMPTY_MV) {
        return 0;
    }
    if (battery_mv >= BATTERY_MONITOR_FULL_MV) {
        return 100;
    }

    return (battery_mv - BATTERY_MONITOR_EMPTY_MV) * 100 /
           (BATTERY_MONITOR_FULL_MV - BATTERY_MONITOR_EMPTY_MV);
}

static int battery_monitor_raw_to_voltage_mv(int raw)
{
    if (raw < 0) {
        return -1;
    }

    int adc_mv = raw * BATTERY_MONITOR_ADC_REF_MV / BATTERY_MONITOR_ADC_RAW_MAX;
    return (int)((float)adc_mv * BATTERY_MONITOR_VOLTAGE_DIVIDER_RATIO);
}

static bool battery_monitor_init_calibration(adc_unit_t unit_id,
                                             adc_channel_t channel,
                                             adc_cali_handle_t *out_handle)
{
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_curve_fitting_config_t cali_config = {
        .unit_id = unit_id,
        .chan = channel,
        .atten = BATTERY_MONITOR_ATTEN,
        .bitwidth = BATTERY_MONITOR_BITWIDTH,
    };
    esp_err_t ret = adc_cali_create_scheme_curve_fitting(&cali_config, out_handle);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "ADC calibration enabled: curve fitting");
        return true;
    }
    ESP_LOGW(TAG, "ADC curve calibration unavailable: %s", esp_err_to_name(ret));
#elif ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    adc_cali_line_fitting_config_t cali_config = {
        .unit_id = unit_id,
        .atten = BATTERY_MONITOR_ATTEN,
        .bitwidth = BATTERY_MONITOR_BITWIDTH,
    };
    esp_err_t ret = adc_cali_create_scheme_line_fitting(&cali_config, out_handle);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "ADC calibration enabled: line fitting");
        return true;
    }
    ESP_LOGW(TAG, "ADC line calibration unavailable: %s", esp_err_to_name(ret));
#else
    (void)unit_id;
    (void)channel;
    (void)out_handle;
    ESP_LOGW(TAG, "ADC calibration scheme is not supported");
#endif
    return false;
}

static esp_err_t battery_monitor_init(battery_monitor_ctx_t *ctx)
{
    adc_unit_t unit_id = ADC_UNIT_1;
    adc_channel_t channel = ADC_CHANNEL_0;
    esp_err_t ret = adc_oneshot_io_to_channel(BATTERY_MONITOR_GPIO, &unit_id, &channel);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG,
                 "GPIO%d is not mapped to an ADC channel on this target: %s",
                 BATTERY_MONITOR_GPIO,
                 esp_err_to_name(ret));
        return ret;
    }

    adc_oneshot_unit_init_cfg_t unit_config = {
        .unit_id = unit_id,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    ret = adc_oneshot_new_unit(&unit_config, &ctx->adc_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ADC unit init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    adc_oneshot_chan_cfg_t channel_config = {
        .atten = BATTERY_MONITOR_ATTEN,
        .bitwidth = BATTERY_MONITOR_BITWIDTH,
    };
    ret = adc_oneshot_config_channel(ctx->adc_handle, channel, &channel_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ADC channel config failed: %s", esp_err_to_name(ret));
        adc_oneshot_del_unit(ctx->adc_handle);
        ctx->adc_handle = NULL;
        return ret;
    }

    ctx->channel = channel;
    ctx->calibrated = battery_monitor_init_calibration(unit_id, channel, &ctx->cali_handle);
    ESP_LOGI(TAG,
             "Battery ADC monitor ready: gpio=%d unit=%d channel=%d atten=%d divider=%.2f",
             BATTERY_MONITOR_GPIO,
             unit_id,
             channel,
             BATTERY_MONITOR_ATTEN,
             BATTERY_MONITOR_VOLTAGE_DIVIDER_RATIO);
    return ESP_OK;
}

static void battery_monitor_task(void *arg)
{
    (void)arg;

    battery_monitor_ctx_t ctx = {0};
    if (battery_monitor_init(&ctx) != ESP_OK) {
        s_battery_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    while (true) {
        int raw_sum = 0;
        int valid_samples = 0;
        for (int i = 0; i < BATTERY_MONITOR_SAMPLE_COUNT; i++) {
            int raw = 0;
            esp_err_t ret = adc_oneshot_read(ctx.adc_handle, ctx.channel, &raw);
            if (ret == ESP_OK) {
                raw_sum += raw;
                valid_samples++;
            } else {
                ESP_LOGW(TAG, "ADC read failed: %s", esp_err_to_name(ret));
            }
            vTaskDelay(pdMS_TO_TICKS(5));
        }

        if (valid_samples > 0) {
            int raw_avg = raw_sum / valid_samples;
            s_battery_raw = raw_avg;
            int adc_mv = -1;
            if (ctx.calibrated) {
                esp_err_t ret = adc_cali_raw_to_voltage(ctx.cali_handle, raw_avg, &adc_mv);
                if (ret != ESP_OK) {
                    ESP_LOGW(TAG, "ADC calibration convert failed: %s", esp_err_to_name(ret));
                    adc_mv = -1;
                }
            }

            if (adc_mv >= 0) {
                int battery_mv = (int)((float)adc_mv * BATTERY_MONITOR_VOLTAGE_DIVIDER_RATIO);
                s_battery_voltage_mv = battery_mv;
                s_battery_percent = battery_monitor_voltage_to_percent(battery_mv);
                ESP_LOGI(TAG,
                         "Battery voltage: raw=%d adc=%d mV battery=%d mV percent=%d samples=%d",
                         raw_avg,
                         adc_mv,
                         battery_mv,
                         s_battery_percent,
                         valid_samples);
            } else {
                int battery_mv = battery_monitor_raw_to_voltage_mv(raw_avg);
                s_battery_voltage_mv = battery_mv;
                s_battery_percent = battery_monitor_voltage_to_percent(battery_mv);
                ESP_LOGI(TAG,
                         "Battery voltage: raw=%d adc=uncalibrated battery=%d mV percent=%d samples=%d",
                         raw_avg,
                         battery_mv,
                         s_battery_percent,
                         valid_samples);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(BATTERY_MONITOR_INTERVAL_MS));
    }
}
#else
static void battery_monitor_task(void *arg)
{
    (void)arg;

    bool was_available = false;
    while (true) {
        chameleon_battery_info_t info;
        uint16_t device_status = 0;
        esp_err_t ret = chameleon_get_battery_info(&info, &device_status);
        if (ret == ESP_OK) {
            s_battery_raw = -1;
            s_battery_voltage_mv = info.voltage_mv;
            s_battery_percent = info.percent;
            if (!was_available) {
                ESP_LOGI(TAG, "Chameleon battery monitor ready");
            }
            ESP_LOGI(TAG,
                     "Chameleon battery: %u mV, %u%%",
                     (unsigned)info.voltage_mv,
                     (unsigned)info.percent);
            was_available = true;
        } else {
            s_battery_voltage_mv = -1;
            s_battery_percent = -1;
            if (was_available) {
                ESP_LOGW(TAG,
                         "Chameleon battery unavailable: %s, status=0x%04x",
                         esp_err_to_name(ret),
                         device_status);
            }
            was_available = false;
        }

        vTaskDelay(pdMS_TO_TICKS(BATTERY_MONITOR_INTERVAL_MS));
    }
}
#endif

esp_err_t battery_monitor_start(void)
{
    if (s_battery_task_handle) {
        return ESP_OK;
    }

    BaseType_t ok = xTaskCreate(battery_monitor_task,
                                "battery_mon",
                                BATTERY_MONITOR_TASK_STACK,
                                NULL,
                                BATTERY_MONITOR_TASK_PRIORITY,
                                &s_battery_task_handle);
    if (ok != pdPASS) {
        s_battery_task_handle = NULL;
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

int battery_monitor_get_percent(void)
{
    return s_battery_percent;
}

int battery_monitor_get_raw(void)
{
    return s_battery_raw;
}

int battery_monitor_get_voltage_mv(void)
{
    return s_battery_voltage_mv;
}

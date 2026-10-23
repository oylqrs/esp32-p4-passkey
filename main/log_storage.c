#include "log_storage.h"

#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "PCF85063/pcf85063.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define LOG_DIR             "/data/log"
#define LOG_FILE_MAX_BYTES  (5 * 1024 * 1024)
#define LOG_FREE_RESERVE    (1024 * 1024)
#define LOG_RECORD_SIZE     256
#define LOG_RECORD_COUNT    32
#define LOG_BATCH_COUNT     8
#define LOG_PATH_SIZE       64

static portMUX_TYPE s_buffer_mux = portMUX_INITIALIZER_UNLOCKED;
static char s_records[LOG_RECORD_COUNT][LOG_RECORD_SIZE];
static size_t s_head;
static size_t s_count;
static SemaphoreHandle_t s_storage_mutex;
static bool s_app_mounted;
static vprintf_like_t s_uart_vprintf = vprintf;
static time_t s_rtc_epoch;
static int64_t s_rtc_sync_us;
static bool s_rtc_valid;
static char s_log_path[LOG_PATH_SIZE];

static void log_storage_update_rtc(void)
{
    pcf85063_time_t rtc;
    if (pcf85063_get_time(&rtc) != ESP_OK || rtc.voltage_low) {
        return;
    }

    struct tm calendar = {
        .tm_year = rtc.year - 1900,
        .tm_mon = rtc.month - 1,
        .tm_mday = rtc.day,
        .tm_hour = rtc.hour,
        .tm_min = rtc.minute,
        .tm_sec = rtc.second,
        .tm_isdst = -1,
    };
    time_t epoch = mktime(&calendar);
    if (epoch == (time_t)-1) {
        return;
    }

    int64_t now_us = esp_timer_get_time();
    portENTER_CRITICAL(&s_buffer_mux);
    s_rtc_epoch = epoch;
    s_rtc_sync_us = now_us;
    s_rtc_valid = true;
    portEXIT_CRITICAL(&s_buffer_mux);
}

static bool log_storage_wall_time(int64_t event_us, struct tm *calendar)
{
    time_t epoch;
    int64_t sync_us;
    bool rtc_valid;
    portENTER_CRITICAL(&s_buffer_mux);
    epoch = s_rtc_epoch;
    sync_us = s_rtc_sync_us;
    rtc_valid = s_rtc_valid;
    portEXIT_CRITICAL(&s_buffer_mux);
    if (!rtc_valid) {
        return false;
    }
    epoch += (event_us - sync_us) / 1000000;
    return localtime_r(&epoch, calendar) != NULL;
}

static int log_capture_vprintf(const char *format, va_list args)
{
    int64_t event_us = esp_timer_get_time();
    va_list copy;
    va_copy(copy, args);
    int result = s_uart_vprintf(format, args);

    char record[LOG_RECORD_SIZE] = {0};

    size_t prefix_len = 0;
    struct tm calendar;
    if (log_storage_wall_time(event_us, &calendar)) {
        prefix_len = strftime(record, sizeof(record), "[%Y-%m-%d %H:%M:%S] ", &calendar);
    }
    if (prefix_len == 0) {
        prefix_len = snprintf(record, sizeof(record), "[uptime %llds] ",
                              (long long)(event_us / 1000000));
    }

    int length = vsnprintf(record + prefix_len, sizeof(record) - prefix_len,
                           format, copy);
    va_end(copy);
    if (length <= 0) {
        return result;
    }
    if (length >= (int)(sizeof(record) - prefix_len)) {
        record[sizeof(record) - 2] = '\n';
        record[sizeof(record) - 1] = '\0';
    }

    portENTER_CRITICAL(&s_buffer_mux);
    if (s_count == LOG_RECORD_COUNT) {
        s_head = (s_head + 1) % LOG_RECORD_COUNT;
        s_count--;
    }
    size_t tail = (s_head + s_count) % LOG_RECORD_COUNT;
    memcpy(s_records[tail], record, sizeof(record));
    s_count++;
    portEXIT_CRITICAL(&s_buffer_mux);
    return result;
}

static FILE *log_storage_create_file(void)
{
    // FAT long filenames are disabled: YYYYMMDD/HHMMSSnn.LOG stays within 8.3.
    struct tm calendar;
    bool dated = log_storage_wall_time(esp_timer_get_time(), &calendar);
    char directory[LOG_PATH_SIZE];
    int length = dated
                     ? snprintf(directory, sizeof(directory), LOG_DIR "/%04d%02d%02d",
                                calendar.tm_year + 1900, calendar.tm_mon + 1, calendar.tm_mday)
                     : snprintf(directory, sizeof(directory), LOG_DIR "/UNKNOWN");
    if (length < 0 || length >= sizeof(directory) ||
        (mkdir(directory, 0775) != 0 && errno != EEXIST)) {
        return NULL;
    }

    unsigned limit = dated ? 100 : 10000;
    for (unsigned index = 0; index < limit; index++) {
        length = dated
                     ? snprintf(s_log_path, sizeof(s_log_path), "%s/%02d%02d%02d%02u.LOG",
                                directory, calendar.tm_hour, calendar.tm_min,
                                calendar.tm_sec, index)
                     : snprintf(s_log_path, sizeof(s_log_path), "%s/UP%06u.LOG",
                                directory, index);
        if (length < 0 || length >= sizeof(s_log_path)) {
            break;
        }
        struct stat st;
        if (stat(s_log_path, &st) == 0) {
            continue;
        }
        if (errno != ENOENT) {
            break;
        }
        FILE *file = fopen(s_log_path, "wb");
        if (file) {
            return file;
        }
        break;
    }
    s_log_path[0] = '\0';
    return NULL;
}

static void log_storage_write_batch(void)
{
    portENTER_CRITICAL(&s_buffer_mux);
    bool pending = s_count != 0;
    portEXIT_CRITICAL(&s_buffer_mux);
    if (!pending) {
        return;
    }

    uint64_t total_bytes, free_bytes;
    if (esp_vfs_fat_info("/data", &total_bytes, &free_bytes) != ESP_OK ||
        free_bytes < LOG_FREE_RESERVE + LOG_BATCH_COUNT * LOG_RECORD_SIZE ||
        (mkdir(LOG_DIR, 0775) != 0 && errno != EEXIST)) {
        return;
    }

    FILE *file = s_log_path[0] ? fopen(s_log_path, "ab+") : log_storage_create_file();
    if (!file) {
        return;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return;
    }
    long file_size = ftell(file);
    if (file_size < 0) {
        fclose(file);
        return;
    }
    if (file_size >= LOG_FILE_MAX_BYTES - LOG_BATCH_COUNT * LOG_RECORD_SIZE) {
        fclose(file);
        s_log_path[0] = '\0';
        file = log_storage_create_file();
        if (!file) {
            return;
        }
    }

    char batch[LOG_BATCH_COUNT][LOG_RECORD_SIZE];
    size_t count = 0;
    portENTER_CRITICAL(&s_buffer_mux);
    while (count < LOG_BATCH_COUNT && s_count != 0) {
        memcpy(batch[count++], s_records[s_head], LOG_RECORD_SIZE);
        s_head = (s_head + 1) % LOG_RECORD_COUNT;
        s_count--;
    }
    portEXIT_CRITICAL(&s_buffer_mux);

    for (size_t i = 0; i < count; i++) {
        size_t length = strlen(batch[i]);
        if (fwrite(batch[i], 1, length, file) != length) {
            break;
        }
    }
    fclose(file);
}

static void log_storage_task(void *arg)
{
    (void)arg;
    unsigned rtc_refresh_ticks = 0;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (++rtc_refresh_ticks >= 10) {
            log_storage_update_rtc();
            rtc_refresh_ticks = 0;
        }
        xSemaphoreTake(s_storage_mutex, portMAX_DELAY);
        if (s_app_mounted) {
            log_storage_write_batch();
        }
        xSemaphoreGive(s_storage_mutex);
    }
}

esp_err_t log_storage_init(void)
{
    if (s_storage_mutex) {
        return ESP_OK;
    }
    SemaphoreHandle_t mutex = xSemaphoreCreateMutex();
    if (!mutex) {
        return ESP_ERR_NO_MEM;
    }
    s_storage_mutex = mutex;
    s_app_mounted = true;
    log_storage_update_rtc();
    if (xTaskCreate(log_storage_task, "log_storage", 6144, NULL, 1, NULL) != pdPASS) {
        s_storage_mutex = NULL;
        vSemaphoreDelete(mutex);
        return ESP_ERR_NO_MEM;
    }
    s_uart_vprintf = esp_log_set_vprintf(log_capture_vprintf);
    return ESP_OK;
}

void log_storage_refresh_time(void)
{
    log_storage_update_rtc();
}

void log_storage_pause(void)
{
    if (s_storage_mutex) {
        xSemaphoreTake(s_storage_mutex, portMAX_DELAY);
        if (s_app_mounted) {
            for (size_t i = 0; i < LOG_RECORD_COUNT / LOG_BATCH_COUNT; i++) {
                portENTER_CRITICAL(&s_buffer_mux);
                size_t pending = s_count;
                portEXIT_CRITICAL(&s_buffer_mux);
                if (pending == 0) {
                    break;
                }
                log_storage_write_batch();
                portENTER_CRITICAL(&s_buffer_mux);
                bool progressed = s_count < pending;
                portEXIT_CRITICAL(&s_buffer_mux);
                if (!progressed) {
                    break;
                }
            }
        }
        s_app_mounted = false;
        xSemaphoreGive(s_storage_mutex);
    }
}

void log_storage_resume(void)
{
    if (s_storage_mutex) {
        xSemaphoreTake(s_storage_mutex, portMAX_DELAY);
        if (!s_app_mounted) {
            s_log_path[0] = '\0';
        }
        s_app_mounted = true;
        xSemaphoreGive(s_storage_mutex);
    }
}

#include "runtime_monitor.h"

#define RUNTIME_MONITOR_ENABLED 0

#if RUNTIME_MONITOR_ENABLED

#include <stdarg.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"

#define RUNTIME_MONITOR_INTERVAL_MS       30000
#define RUNTIME_MONITOR_UI_REFRESH_MS     1000
#define RUNTIME_MONITOR_STACK_SIZE        4096
#define RUNTIME_MONITOR_TASK_CAPACITY     48
#define RUNTIME_MONITOR_STACK_WARNING     1024
#define RUNTIME_MONITOR_TEXT_SIZE         2048

static const char *TAG = "runtime_monitor";
static TaskHandle_t s_monitor_task;
static TaskStatus_t s_task_status[RUNTIME_MONITOR_TASK_CAPACITY];
static SemaphoreHandle_t s_text_mutex;
static lv_obj_t *s_overlay_label;
static char s_screen_text[RUNTIME_MONITOR_TEXT_SIZE] = "Collecting runtime metrics...";

static char task_state_char(eTaskState state)
{
    switch (state) {
    case eRunning:
        return 'R';
    case eReady:
        return 'Y';
    case eBlocked:
        return 'B';
    case eSuspended:
        return 'S';
    case eDeleted:
        return 'D';
    default:
        return '?';
    }
}

static size_t screen_text_append(size_t offset, const char *format, ...)
{
    if (offset >= sizeof(s_screen_text) - 1) {
        return sizeof(s_screen_text) - 1;
    }

    va_list args;
    va_start(args, format);
    int written = vsnprintf(s_screen_text + offset,
                            sizeof(s_screen_text) - offset,
                            format,
                            args);
    va_end(args);

    if (written < 0) {
        return offset;
    }
    size_t available = sizeof(s_screen_text) - offset;
    if ((size_t)written >= available) {
        return sizeof(s_screen_text) - 1;
    }
    return offset + (size_t)written;
}

static size_t log_heap_status(size_t text_offset)
{
    const uint32_t internal_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    const uint32_t psram_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    size_t internal_free = heap_caps_get_free_size(internal_caps);
    size_t internal_min = heap_caps_get_minimum_free_size(internal_caps);
    size_t internal_largest = heap_caps_get_largest_free_block(internal_caps);
    size_t psram_free = heap_caps_get_free_size(psram_caps);
    size_t psram_min = heap_caps_get_minimum_free_size(psram_caps);
    size_t psram_largest = heap_caps_get_largest_free_block(psram_caps);

    ESP_LOGI(TAG,
             "Heap internal: free=%u min=%u largest=%u; PSRAM: free=%u min=%u largest=%u",
             (unsigned int)internal_free,
             (unsigned int)internal_min,
             (unsigned int)internal_largest,
             (unsigned int)psram_free,
             (unsigned int)psram_min,
             (unsigned int)psram_largest);

    text_offset = screen_text_append(text_offset,
                                     "INT free:%u min:%u block:%u\n",
                                     (unsigned int)internal_free,
                                     (unsigned int)internal_min,
                                     (unsigned int)internal_largest);
    return screen_text_append(text_offset,
                              "PSRAM free:%u min:%u block:%u\n",
                              (unsigned int)psram_free,
                              (unsigned int)psram_min,
                              (unsigned int)psram_largest);
}

static size_t log_task_stacks(size_t text_offset)
{
    UBaseType_t task_count = uxTaskGetNumberOfTasks();
    if (task_count > RUNTIME_MONITOR_TASK_CAPACITY) {
        ESP_LOGW(TAG, "Task snapshot capacity exceeded: active=%u capacity=%u",
                 (unsigned int)task_count,
                 (unsigned int)RUNTIME_MONITOR_TASK_CAPACITY);
        return screen_text_append(text_offset,
                                  "Tasks:%u exceeds capacity:%u",
                                  (unsigned int)task_count,
                                  (unsigned int)RUNTIME_MONITOR_TASK_CAPACITY);
    }

    task_count = uxTaskGetSystemState(s_task_status,
                                      RUNTIME_MONITOR_TASK_CAPACITY,
                                      NULL);
    if (task_count == 0) {
        ESP_LOGW(TAG, "Failed to capture task stack snapshot");
        return screen_text_append(text_offset, "Task snapshot failed");
    }

    ESP_LOGI(TAG, "Task stack snapshot: count=%u, high-water marks are bytes",
             (unsigned int)task_count);
    text_offset = screen_text_append(text_offset,
                                     "Tasks:%u   S P HWM(bytes)\n",
                                     (unsigned int)task_count);
    for (UBaseType_t i = 0; i < task_count; i++) {
        const TaskStatus_t *status = &s_task_status[i];
        char task_name[configMAX_TASK_NAME_LEN];
        strlcpy(task_name, status->pcTaskName, sizeof(task_name));

        if (status->usStackHighWaterMark < RUNTIME_MONITOR_STACK_WARNING) {
            ESP_LOGW(TAG, "Task %-16s state=%c priority=%u stack_hwm=%u",
                     task_name,
                     task_state_char(status->eCurrentState),
                     (unsigned int)status->uxCurrentPriority,
                     (unsigned int)status->usStackHighWaterMark);
        } else {
            ESP_LOGI(TAG, "Task %-16s state=%c priority=%u stack_hwm=%u",
                     task_name,
                     task_state_char(status->eCurrentState),
                     (unsigned int)status->uxCurrentPriority,
                     (unsigned int)status->usStackHighWaterMark);
        }
        text_offset = screen_text_append(text_offset,
                                         "%c%-16s %c %u %u\n",
                                         status->usStackHighWaterMark < RUNTIME_MONITOR_STACK_WARNING ? '!' : ' ',
                                         task_name,
                                         task_state_char(status->eCurrentState),
                                         (unsigned int)status->uxCurrentPriority,
                                         (unsigned int)status->usStackHighWaterMark);
    }
    return text_offset;
}

static void collect_runtime_status(void)
{
    if (xSemaphoreTake(s_text_mutex, portMAX_DELAY) != pdTRUE) {
        return;
    }

    s_screen_text[0] = '\0';
    size_t text_offset = log_heap_status(0);
    log_task_stacks(text_offset);
    xSemaphoreGive(s_text_mutex);
}

static void runtime_monitor_ui_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (s_overlay_label && xSemaphoreTake(s_text_mutex, 0) == pdTRUE) {
        lv_label_set_text(s_overlay_label, s_screen_text);
        xSemaphoreGive(s_text_mutex);
    }
}

static void runtime_monitor_task(void *arg)
{
    (void)arg;
    while (true) {
        collect_runtime_status();
        vTaskDelay(pdMS_TO_TICKS(RUNTIME_MONITOR_INTERVAL_MS));
    }
}

static esp_err_t ensure_text_mutex(void)
{
    if (!s_text_mutex) {
        s_text_mutex = xSemaphoreCreateMutex();
    }
    return s_text_mutex ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t runtime_monitor_ui_init(void)
{
    esp_err_t ret = ensure_text_mutex();
    if (ret != ESP_OK) {
        return ret;
    }
    if (s_overlay_label) {
        return ESP_ERR_INVALID_STATE;
    }

    s_overlay_label = lv_label_create(lv_layer_top());
    lv_obj_set_size(s_overlay_label, lv_pct(100), lv_pct(100));
    lv_obj_set_pos(s_overlay_label, 0, 0);
    lv_label_set_long_mode(s_overlay_label, LV_LABEL_LONG_WRAP);
    lv_label_set_text(s_overlay_label, s_screen_text);
    lv_obj_set_style_pad_all(s_overlay_label, 4, 0);
    lv_obj_set_style_text_color(s_overlay_label, lv_color_hex(0x69ff8f), 0);
    lv_obj_set_style_bg_color(s_overlay_label, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_overlay_label, LV_OPA_70, 0);
    lv_obj_clear_flag(s_overlay_label, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_overlay_label, LV_OBJ_FLAG_FLOATING);
    lv_obj_move_foreground(s_overlay_label);
    lv_timer_create(runtime_monitor_ui_timer_cb, RUNTIME_MONITOR_UI_REFRESH_MS, NULL);
    return ESP_OK;
}

esp_err_t runtime_monitor_start(void)
{
    if (s_monitor_task) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = ensure_text_mutex();
    if (ret != ESP_OK) {
        return ret;
    }

    if (xTaskCreate(runtime_monitor_task,
                    "runtime_monitor",
                    RUNTIME_MONITOR_STACK_SIZE,
                    NULL,
                    1,
                    &s_monitor_task) != pdPASS) {
        s_monitor_task = NULL;
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

#else

esp_err_t runtime_monitor_ui_init(void)
{
    return ESP_OK;
}

esp_err_t runtime_monitor_start(void)
{
    return ESP_OK;
}

#endif

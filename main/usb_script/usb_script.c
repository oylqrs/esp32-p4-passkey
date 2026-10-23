#include "usb_script.h"

#include <stdbool.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "ducky_executor.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hid_keyboard.h"

#define USB_SCRIPT_PATH_MAX_LEN 512
#define USB_SCRIPT_TASK_STACK_SIZE 4096
#define USB_SCRIPT_TASK_PRIORITY 4

static const char *TAG = "usb_script";
static bool s_send_in_progress;
static atomic_int s_status = USB_SCRIPT_STATUS_IDLE;
static atomic_size_t s_step;
static atomic_size_t s_total;

typedef struct {
    char path[USB_SCRIPT_PATH_MAX_LEN];
} usb_script_task_arg_t;

static void script_progress_cb(size_t step, size_t total, void *context)
{
    (void)context;
    atomic_store(&s_step, step);
    atomic_store(&s_total, total);
}

static void usb_script_send_task(void *arg)
{
    usb_script_task_arg_t *task_arg = arg;
    ESP_LOGI(TAG, "Executing DuckyScript: %s", task_arg->path);
    esp_err_t ret = ducky_executor_run_file(task_arg->path, script_progress_cb, NULL);
    if (ret != ESP_OK) ESP_LOGE(TAG, "DuckyScript failed: %s", esp_err_to_name(ret));
    atomic_store(&s_status, ret == ESP_OK ? USB_SCRIPT_STATUS_COMPLETED : USB_SCRIPT_STATUS_FAILED);
    s_send_in_progress = false;
    free(task_arg);
    vTaskDelete(NULL);
}

esp_err_t usb_script_send_file(const char *path)
{
    if (!path || path[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (!hid_keyboard_is_ready() || s_send_in_progress) {
        return ESP_ERR_INVALID_STATE;
    }
    if (strnlen(path, USB_SCRIPT_PATH_MAX_LEN) >= USB_SCRIPT_PATH_MAX_LEN) {
        return ESP_ERR_INVALID_SIZE;
    }

    usb_script_task_arg_t *task_arg = malloc(sizeof(*task_arg));
    if (!task_arg) {
        return ESP_ERR_NO_MEM;
    }
    strlcpy(task_arg->path, path, sizeof(task_arg->path));

    atomic_store(&s_step, 0);
    atomic_store(&s_total, 0);
    atomic_store(&s_status, USB_SCRIPT_STATUS_RUNNING);
    s_send_in_progress = true;
    if (xTaskCreate(usb_script_send_task, "usb_script", USB_SCRIPT_TASK_STACK_SIZE, task_arg,
                    USB_SCRIPT_TASK_PRIORITY, NULL) != pdPASS) {
        s_send_in_progress = false;
        atomic_store(&s_status, USB_SCRIPT_STATUS_FAILED);
        free(task_arg);
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

void usb_script_get_progress(usb_script_progress_t *progress)
{
    if (!progress) return;
    progress->status = (usb_script_status_t)atomic_load(&s_status);
    progress->step = atomic_load(&s_step);
    progress->total = atomic_load(&s_total);
}

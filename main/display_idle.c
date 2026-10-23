#include "display_idle.h"

#include <stddef.h>

#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lcd/lcd_driver.h"
#include "nvs.h"
#include "usb_hid_keyboard.h"

#define DISPLAY_IDLE_NVS_NAMESPACE "display_ui"
#define DISPLAY_IDLE_NVS_KEY "timeout_s"
#define DISPLAY_IDLE_TIMER_PERIOD_MS 250
#define DISPLAY_IDLE_SAVE_TASK_STACK_SIZE 4096
#define DISPLAY_IDLE_SAVE_TASK_PRIORITY 2

typedef enum {
    DISPLAY_IDLE_AWAKE,
    DISPLAY_IDLE_OFF,
    DISPLAY_IDLE_WAIT_RELEASE,
} display_idle_state_t;

static const char *TAG = "display_idle";
static const uint16_t s_valid_timeouts[] = {
    120, 60, 30, 20, 10, DISPLAY_IDLE_TIMEOUT_DISABLED_SECONDS
};

static lv_display_t *s_display;
static lv_timer_t *s_timer;
static QueueHandle_t s_save_queue;
static uint16_t s_timeout_seconds = DISPLAY_IDLE_DEFAULT_TIMEOUT_SECONDS;
static display_idle_state_t s_state = DISPLAY_IDLE_AWAKE;
static bool s_usb_timeout_suspended;

static bool timeout_is_valid(uint16_t seconds)
{
    for (size_t i = 0; i < sizeof(s_valid_timeouts) / sizeof(s_valid_timeouts[0]); i++) {
        if (s_valid_timeouts[i] == seconds) {
            return true;
        }
    }
    return false;
}

static uint16_t load_timeout(void)
{
    nvs_handle_t nvs;
    if (nvs_open(DISPLAY_IDLE_NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        return DISPLAY_IDLE_DEFAULT_TIMEOUT_SECONDS;
    }

    uint16_t seconds = DISPLAY_IDLE_DEFAULT_TIMEOUT_SECONDS;
    uint16_t stored = seconds;
    if (nvs_get_u16(nvs, DISPLAY_IDLE_NVS_KEY, &stored) == ESP_OK &&
        timeout_is_valid(stored)) {
        seconds = stored;
    }
    nvs_close(nvs);
    return seconds;
}

static esp_err_t save_timeout(uint16_t seconds)
{
    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(DISPLAY_IDLE_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = nvs_set_u16(nvs, DISPLAY_IDLE_NVS_KEY, seconds);
    if (ret == ESP_OK) {
        ret = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return ret;
}

static void timeout_save_task(void *arg)
{
    (void)arg;

    while (true) {
        uint16_t seconds;
        if (xQueueReceive(s_save_queue, &seconds, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        int64_t start_us = esp_timer_get_time();
        ESP_LOGI(TAG, "Saving auto screen-off timeout: %u seconds", seconds);
        esp_err_t ret = save_timeout(seconds);
        int64_t elapsed_ms = (esp_timer_get_time() - start_us) / 1000;
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "Saved auto screen-off timeout in %lld ms",
                     (long long)elapsed_ms);
        } else {
            ESP_LOGW(TAG, "Failed to save auto screen-off timeout after %lld ms: %s",
                     (long long)elapsed_ms, esp_err_to_name(ret));
        }
    }
}

static void display_idle_timer_cb(lv_timer_t *timer)
{
    (void)timer;

    if (s_display == NULL || s_state != DISPLAY_IDLE_AWAKE) {
        return;
    }

    if (s_timeout_seconds == DISPLAY_IDLE_TIMEOUT_DISABLED_SECONDS) {
        return;
    }

    if (usb_hid_keyboard_is_ready()) {
        lv_display_trigger_activity(s_display);
        if (!s_usb_timeout_suspended) {
            s_usb_timeout_suspended = true;
            ESP_LOGI(TAG, "USB connected; auto screen-off suspended");
        }
        return;
    }

    if (s_usb_timeout_suspended) {
        s_usb_timeout_suspended = false;
        lv_display_trigger_activity(s_display);
        ESP_LOGI(TAG, "USB disconnected; auto screen-off resumed");
    }

    uint32_t timeout_ms = (uint32_t)s_timeout_seconds * 1000U;
    if (lv_display_get_inactive_time(s_display) < timeout_ms) {
        return;
    }

    s_state = DISPLAY_IDLE_OFF;
    ESP_LOGW(TAG, "Display idle timeout after %u seconds; restarting",
             s_timeout_seconds);
    lcd_driver_set_backlight(0);
    vTaskDelay(pdMS_TO_TICKS(150));
    esp_restart();
}

esp_err_t display_idle_init(lv_display_t *display)
{
    if (display == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_timer != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    s_display = display;
    s_timeout_seconds = load_timeout();
    s_state = DISPLAY_IDLE_AWAKE;
    s_usb_timeout_suspended = false;
    lv_display_trigger_activity(s_display);

    s_save_queue = xQueueCreate(1, sizeof(uint16_t));
    if (s_save_queue == NULL) {
        s_display = NULL;
        return ESP_ERR_NO_MEM;
    }

    s_timer = lv_timer_create(display_idle_timer_cb,
                              DISPLAY_IDLE_TIMER_PERIOD_MS, NULL);
    if (s_timer == NULL) {
        vQueueDelete(s_save_queue);
        s_save_queue = NULL;
        s_display = NULL;
        return ESP_ERR_NO_MEM;
    }

    if (xTaskCreate(timeout_save_task, "idle_timeout_save",
                    DISPLAY_IDLE_SAVE_TASK_STACK_SIZE, NULL,
                    DISPLAY_IDLE_SAVE_TASK_PRIORITY, NULL) != pdPASS) {
        lv_timer_delete(s_timer);
        s_timer = NULL;
        vQueueDelete(s_save_queue);
        s_save_queue = NULL;
        s_display = NULL;
        return ESP_ERR_NO_MEM;
    }

    if (s_timeout_seconds == DISPLAY_IDLE_TIMEOUT_DISABLED_SECONDS) {
        ESP_LOGI(TAG, "Auto screen-off disabled");
    } else {
        ESP_LOGI(TAG, "Auto screen-off timeout: %u seconds", s_timeout_seconds);
    }
    return ESP_OK;
}

uint16_t display_idle_get_timeout_seconds(void)
{
    return s_timeout_seconds;
}

esp_err_t display_idle_set_timeout_seconds(uint16_t seconds)
{
    if (!timeout_is_valid(seconds)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_save_queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    s_timeout_seconds = seconds;
    if (s_display != NULL) {
        lv_display_trigger_activity(s_display);
    }
    if (seconds == DISPLAY_IDLE_TIMEOUT_DISABLED_SECONDS) {
        ESP_LOGI(TAG, "Auto screen-off disabled");
    } else {
        ESP_LOGI(TAG, "Auto screen-off timeout changed to %u seconds", seconds);
    }

    if (xQueueOverwrite(s_save_queue, &seconds) != pdPASS) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

bool display_idle_should_suppress_touch(bool pressed)
{
    if (s_display == NULL || s_state == DISPLAY_IDLE_AWAKE) {
        return false;
    }

    if (s_state == DISPLAY_IDLE_OFF) {
        if (pressed) {
            lcd_driver_set_backlight(lcd_driver_get_backlight_percent());
            lv_display_trigger_activity(s_display);
            s_state = DISPLAY_IDLE_WAIT_RELEASE;
            ESP_LOGI(TAG, "Display awakened by touch");
        }
        return true;
    }

    if (!pressed) {
        lv_display_trigger_activity(s_display);
        s_state = DISPLAY_IDLE_AWAKE;
    }
    return true;
}

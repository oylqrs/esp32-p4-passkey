#include "ducky_executor.h"
#include <stdio.h>
#include <string.h>
#include "ducky_parser.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hid_keyboard.h"

#define DUCKY_LINE_MAX_LEN 640
#define HID_TEXT_CHUNK_LEN 159
#define QUEUE_RETRY_MS 20
static const char *TAG = "ducky_executor";

static esp_err_t queue_until_ready(esp_err_t (*operation)(void *), void *arg)
{
    esp_err_t ret;
    do { ret = operation(arg); if (ret == ESP_ERR_TIMEOUT) vTaskDelay(pdMS_TO_TICKS(QUEUE_RETRY_MS)); } while (ret == ESP_ERR_TIMEOUT);
    return ret;
}
static esp_err_t queue_text_op(void *arg) { return hid_type_string_async(arg); }
static esp_err_t queue_release_op(void *arg) { (void)arg; return hid_key_release_async(); }
static esp_err_t queue_delay_op(void *arg) { return hid_delay_async(*(uint32_t *)arg); }
typedef struct { uint8_t modifier; uint8_t key; } key_arg_t;
static esp_err_t queue_key_op(void *arg) { key_arg_t *key = arg; return hid_key_report_async(key->modifier, key->key); }

static esp_err_t queue_text(const char *text)
{
    char chunk[HID_TEXT_CHUNK_LEN + 1];
    while (*text) {
        size_t length = strnlen(text, HID_TEXT_CHUNK_LEN);
        memcpy(chunk, text, length); chunk[length] = '\0';
        esp_err_t ret = queue_until_ready(queue_text_op, chunk);
        if (ret != ESP_OK) return ret;
        text += length;
    }
    return ESP_OK;
}

esp_err_t ducky_executor_run_file(const char *path, ducky_progress_cb_t progress_cb, void *context)
{
    FILE *file = fopen(path, "rb");
    if (!file) return ESP_ERR_NOT_FOUND;
    char line[DUCKY_LINE_MAX_LEN];
    size_t total = 0;
    while (fgets(line, sizeof(line), file)) total++;
    if (ferror(file) || fseek(file, 0, SEEK_SET) != 0) { fclose(file); return ESP_FAIL; }
    if (progress_cb) progress_cb(0, total, context);
    unsigned line_number = 0;
    uint32_t default_delay = 0;
    esp_err_t ret = ESP_OK;
    while (fgets(line, sizeof(line), file)) {
        line_number++;
        if (!strchr(line, '\n') && !feof(file)) { ret = ESP_ERR_INVALID_SIZE; break; }
        ducky_command_t command;
        ret = ducky_parser_parse_line(line, &command);
        if (ret != ESP_OK) break;
        if (command.type == DUCKY_COMMAND_STRING || command.type == DUCKY_COMMAND_STRINGLN) {
            ret = queue_text(command.text);
            if (ret == ESP_OK && command.type == DUCKY_COMMAND_STRINGLN) ret = queue_text("\n");
        } else if (command.type == DUCKY_COMMAND_DELAY) ret = queue_until_ready(queue_delay_op, &command.delay_ms);
        else if (command.type == DUCKY_COMMAND_DEFAULT_DELAY) default_delay = command.delay_ms;
        else if (command.type == DUCKY_COMMAND_KEYSTROKE) {
            key_arg_t key = {command.modifier, command.key};
            ret = queue_until_ready(queue_key_op, &key);
            if (ret == ESP_OK) ret = queue_until_ready(queue_release_op, NULL);
        }
        if (ret != ESP_OK) break;
        if (default_delay && command.type != DUCKY_COMMAND_EMPTY && command.type != DUCKY_COMMAND_DEFAULT_DELAY)
            ret = queue_until_ready(queue_delay_op, &default_delay);
        if (ret != ESP_OK) break;
        if (progress_cb) progress_cb(line_number, total, context);
    }
    if (ret == ESP_OK && ferror(file)) ret = ESP_FAIL;
    fclose(file);
    if (ret == ESP_OK) ret = hid_keyboard_wait_idle();
    if (ret != ESP_OK) ESP_LOGE(TAG, "%s:%u failed: %s", path, line_number, esp_err_to_name(ret));
    return ret;
}

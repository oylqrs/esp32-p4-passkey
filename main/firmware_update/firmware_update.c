#include "firmware_update.h"

#include <dirent.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "firmware_verify.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lcd/lcd_driver.h"
#include "storage_operation_guard.h"
#include "usb_msc.h"

#define UPDATE_TASK_STACK_SIZE 8192
#define UPDATE_TASK_PRIORITY 4
#define UPDATE_BUFFER_SIZE 4096
#define UPDATE_RESTART_DELAY_MS 2000
#define UPDATE_IMAGE_PATH_SIZE 512

static const char *TAG = "firmware_update";
static atomic_bool s_running;
static atomic_int s_state = FIRMWARE_UPDATE_IDLE;
static atomic_size_t s_bytes_written;
static atomic_size_t s_image_size;
static atomic_int s_error;
static char s_version[32];
static char s_sha256[65];
static char s_image_path[UPDATE_IMAGE_PATH_SIZE];
static portMUX_TYPE s_status_lock = portMUX_INITIALIZER_UNLOCKED;

typedef enum {
    UPDATE_ACTION_VERIFY,
    UPDATE_ACTION_INSTALL,
} update_action_t;

static bool has_bin_extension(const char *name)
{
    size_t length = name ? strlen(name) : 0;
    return length > 4 && strcasecmp(name + length - 4, ".bin") == 0;
}

static esp_err_t find_update_image(char *path, size_t path_size)
{
    if (!path || path_size == 0) return ESP_ERR_INVALID_ARG;

    DIR *directory = opendir(FIRMWARE_UPDATE_DIRECTORY);
    if (!directory) return ESP_ERR_NOT_FOUND;

    size_t candidate_count = 0;
    esp_err_t result = ESP_ERR_NOT_FOUND;
    struct dirent *entry;
    while ((entry = readdir(directory)) != NULL) {
        if (!has_bin_extension(entry->d_name)) continue;

        char candidate[UPDATE_IMAGE_PATH_SIZE];
        int written = snprintf(candidate, sizeof(candidate), "%s/%s",
                               FIRMWARE_UPDATE_DIRECTORY, entry->d_name);
        if (written < 0 || (size_t)written >= sizeof(candidate)) {
            result = ESP_ERR_INVALID_SIZE;
            break;
        }

        struct stat file_stat;
        if (stat(candidate, &file_stat) != 0 || !S_ISREG(file_stat.st_mode)) continue;

        candidate_count++;
        if (candidate_count > 1) {
            result = ESP_ERR_INVALID_STATE;
            break;
        }
        strlcpy(path, candidate, path_size);
        result = ESP_OK;
    }
    closedir(directory);

    if (result == ESP_OK) {
        ESP_LOGI(TAG, "Found firmware image: %s", path);
    } else if (result == ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "More than one .bin file found in %s", FIRMWARE_UPDATE_DIRECTORY);
    }
    return result;
}

static const char *partition_display_name(const esp_partition_t *partition)
{
    if (!partition) return "?";
    if (partition->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_0) return "OTA0";
    if (partition->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_1) return "OTA1";
    return partition->label;
}

static void finish_failed(esp_err_t error)
{
    atomic_store(&s_error, error);
    atomic_store(&s_state, FIRMWARE_UPDATE_FAILED);
    atomic_store(&s_running, false);
    ESP_LOGE(TAG, "Firmware update failed: %s", esp_err_to_name(error));
}

static void firmware_update_task(void *arg)
{
    update_action_t action = (update_action_t)(uintptr_t)arg;
    FILE *file = NULL;
    esp_ota_handle_t ota_handle = 0;
    bool ota_started = false;
    char image_path[UPDATE_IMAGE_PATH_SIZE] = {0};

    atomic_store(&s_state, FIRMWARE_UPDATE_MOUNTING);
    esp_err_t ret = usb_hid_mount_usbstore_app();
    if (ret != ESP_OK) goto failed;

    if (action == UPDATE_ACTION_VERIFY) {
        ret = find_update_image(image_path, sizeof(image_path));
        if (ret != ESP_OK) goto failed;
        portENTER_CRITICAL(&s_status_lock);
        strlcpy(s_image_path, image_path, sizeof(s_image_path));
        portEXIT_CRITICAL(&s_status_lock);
    } else {
        portENTER_CRITICAL(&s_status_lock);
        strlcpy(image_path, s_image_path, sizeof(image_path));
        portEXIT_CRITICAL(&s_status_lock);
        if (image_path[0] == '\0') { ret = ESP_ERR_INVALID_STATE; goto failed; }
    }

    file = fopen(image_path, "rb");
    if (!file) { ret = ESP_ERR_NOT_FOUND; goto failed; }

    const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
    if (!target) { ret = ESP_ERR_NOT_FOUND; goto failed; }
    const esp_partition_t *running = esp_ota_get_running_partition();
    ESP_LOGI(TAG, "OTA update: running=%s@0x%lx target=%s@0x%lx",
             running ? running->label : "?", running ? (unsigned long)running->address : 0,
             target->label, (unsigned long)target->address);

    atomic_store(&s_state, FIRMWARE_UPDATE_VERIFYING);
    firmware_image_info_t image_info = {0};
    ret = firmware_verify_file(file, target->size, &image_info);
    if (ret != ESP_OK && ret != FIRMWARE_VERIFY_ERR_SAME_VERSION) goto failed;
    atomic_store(&s_image_size, image_info.image_size);
    portENTER_CRITICAL(&s_status_lock);
    strlcpy(s_version, image_info.app_desc.version, sizeof(s_version));
    for (size_t i = 0; i < sizeof(image_info.sha256); ++i) {
        static const char hex[] = "0123456789abcdef";
        s_sha256[i * 2] = hex[image_info.sha256[i] >> 4];
        s_sha256[i * 2 + 1] = hex[image_info.sha256[i] & 0x0f];
    }
    s_sha256[sizeof(s_sha256) - 1] = '\0';
    portEXIT_CRITICAL(&s_status_lock);

    if (ret == FIRMWARE_VERIFY_ERR_SAME_VERSION) {
        fclose(file);
        atomic_store(&s_error, ret);
        atomic_store(&s_state, FIRMWARE_UPDATE_REJECTED_SAME_VERSION);
        storage_operation_release(STORAGE_OPERATION_FIRMWARE_UPDATE);
        atomic_store(&s_running, false);
        ESP_LOGW(TAG, "Firmware version %s is already installed; update rejected", s_version);
        vTaskDelete(NULL);
        return;
    }

    if (action == UPDATE_ACTION_VERIFY) {
        fclose(file);
        atomic_store(&s_state, FIRMWARE_UPDATE_VERIFIED);
        storage_operation_release(STORAGE_OPERATION_FIRMWARE_UPDATE);
        atomic_store(&s_running, false);
        ESP_LOGI(TAG, "Verified version %s (%u bytes)", s_version,
                 (unsigned)image_info.image_size);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Writing version %s (%u bytes) to %s", s_version,
             (unsigned)image_info.image_size, target->label);
    ret = esp_ota_begin(target, OTA_WITH_SEQUENTIAL_WRITES, &ota_handle);
    if (ret != ESP_OK) goto failed;
    ota_started = true;
    atomic_store(&s_state, FIRMWARE_UPDATE_WRITING);

    uint8_t buffer[UPDATE_BUFFER_SIZE];
    size_t total_written = 0;
    while (total_written < image_info.image_size) {
        size_t read_len = fread(buffer, 1, sizeof(buffer), file);
        if (read_len == 0) { ret = ferror(file) ? ESP_FAIL : ESP_ERR_INVALID_SIZE; goto failed; }
        ret = esp_ota_write(ota_handle, buffer, read_len);
        if (ret != ESP_OK) goto failed;
        total_written += read_len;
        atomic_store(&s_bytes_written, total_written);
    }

    fclose(file);
    file = NULL;
    ret = esp_ota_end(ota_handle);
    ota_started = false;
    if (ret != ESP_OK) goto failed;
    ret = esp_ota_set_boot_partition(target);
    if (ret != ESP_OK) goto failed;
    const esp_partition_t *configured = esp_ota_get_boot_partition();
    if (!configured || configured->address != target->address) {
        ESP_LOGE(TAG, "Boot partition verification failed: expected=%s@0x%lx actual=%s@0x%lx",
                 target->label, (unsigned long)target->address,
                 configured ? configured->label : "?",
                 configured ? (unsigned long)configured->address : 0);
        ret = ESP_ERR_INVALID_STATE;
        goto failed;
    }
    ESP_LOGI(TAG, "Next boot partition confirmed: %s@0x%lx",
             configured->label, (unsigned long)configured->address);

    atomic_store(&s_state, FIRMWARE_UPDATE_COMPLETED);
    atomic_store(&s_running, false);
    ESP_LOGI(TAG, "Firmware update complete; turning display off before restart");
    lcd_driver_set_backlight(0);
    vTaskDelay(pdMS_TO_TICKS(UPDATE_RESTART_DELAY_MS));
    esp_restart();
    storage_operation_release(STORAGE_OPERATION_FIRMWARE_UPDATE);
    vTaskDelete(NULL);
    return;

failed:
    if (file) fclose(file);
    if (ota_started) esp_ota_abort(ota_handle);
    storage_operation_release(STORAGE_OPERATION_FIRMWARE_UPDATE);
    finish_failed(ret);
    vTaskDelete(NULL);
}

esp_err_t firmware_update_init(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *configured = esp_ota_get_boot_partition();
    ESP_LOGI(TAG, "OTA boot: running=%s@0x%lx configured=%s@0x%lx",
             running ? running->label : "?", running ? (unsigned long)running->address : 0,
             configured ? configured->label : "?",
             configured ? (unsigned long)configured->address : 0);
    esp_ota_img_states_t state;
    if (running && esp_ota_get_state_partition(running, &state) == ESP_OK && state == ESP_OTA_IMG_PENDING_VERIFY) {
        esp_err_t ret = esp_ota_mark_app_valid_cancel_rollback();
        if (ret == ESP_OK) ESP_LOGI(TAG, "Running OTA image marked valid: %s", running->label);
        return ret;
    }
    return ESP_OK;
}

static esp_err_t start_update_action(update_action_t action)
{
    bool expected = false;
    if (!atomic_compare_exchange_strong(&s_running, &expected, true)) return ESP_ERR_INVALID_STATE;
    if (action == UPDATE_ACTION_INSTALL && atomic_load(&s_state) != FIRMWARE_UPDATE_VERIFIED) {
        atomic_store(&s_running, false);
        return ESP_ERR_INVALID_STATE;
    }
    if (!storage_operation_try_acquire(STORAGE_OPERATION_FIRMWARE_UPDATE)) {
        storage_operation_t active_operation = storage_operation_get();
        atomic_store(&s_running, false);
        ESP_LOGW(TAG, "Firmware update blocked by active %s operation",
                 storage_operation_name(active_operation));
        return ESP_ERR_INVALID_STATE;
    }
    atomic_store(&s_state, FIRMWARE_UPDATE_MOUNTING);
    atomic_store(&s_bytes_written, 0);
    atomic_store(&s_error, ESP_OK);
    if (action == UPDATE_ACTION_VERIFY) {
        atomic_store(&s_image_size, 0);
        portENTER_CRITICAL(&s_status_lock);
        s_version[0] = '\0';
        s_sha256[0] = '\0';
        s_image_path[0] = '\0';
        portEXIT_CRITICAL(&s_status_lock);
    }
    if (xTaskCreate(firmware_update_task, "fw_update", UPDATE_TASK_STACK_SIZE,
                    (void *)(uintptr_t)action,
                    UPDATE_TASK_PRIORITY, NULL) != pdPASS) {
        storage_operation_release(STORAGE_OPERATION_FIRMWARE_UPDATE);
        finish_failed(ESP_ERR_NO_MEM);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t firmware_update_verify(void)
{
    return start_update_action(UPDATE_ACTION_VERIFY);
}

esp_err_t firmware_update_install(void)
{
    return start_update_action(UPDATE_ACTION_INSTALL);
}

void firmware_update_get_status(firmware_update_status_t *status)
{
    if (!status) return;
    status->state = (firmware_update_state_t)atomic_load(&s_state);
    status->bytes_written = atomic_load(&s_bytes_written);
    status->image_size = atomic_load(&s_image_size);
    status->error = (esp_err_t)atomic_load(&s_error);
    portENTER_CRITICAL(&s_status_lock);
    strlcpy(status->version, s_version, sizeof(status->version));
    strlcpy(status->sha256, s_sha256, sizeof(status->sha256));
    portEXIT_CRITICAL(&s_status_lock);

    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
    strlcpy(status->running_partition, partition_display_name(running),
            sizeof(status->running_partition));
    strlcpy(status->target_partition, partition_display_name(target),
            sizeof(status->target_partition));
}

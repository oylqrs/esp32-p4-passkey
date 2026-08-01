#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "canokey_piv_slots.h"
#include "esp_log.h"
#include "fs.h"
#include "lfs.h"
#include "piv.h"

static const char *TAG = "piv_storage";
static uint8_t s_piv_slot;

int __real_read_file(const char *path, void *buf, lfs_soff_t off, lfs_size_t len);
int __real_write_file(const char *path, const void *buf, lfs_soff_t off, lfs_size_t len, uint8_t trunc);
int __real_read_attr(const char *path, uint8_t attr, void *buf, lfs_size_t len);
int __real_write_attr(const char *path, uint8_t attr, const void *buf, lfs_size_t len);
int __real_remove_attr(const char *path, uint8_t attr);
int __real_get_file_size(const char *path);
int __real_remove_file(const char *path);
int __real_piv_platform_algorithm_extension_config_read(piv_algorithm_extension_config_t *cfg);
int __real_piv_platform_algorithm_extension_config_write(const piv_algorithm_extension_config_t *cfg);

bool canokey_piv_storage_is_path(const char *path)
{
    return path && strncmp(path, "piv-", 4) == 0;
}

int canokey_piv_select_slot(uint8_t slot)
{
    if (slot >= CANOKEY_PIV_SLOT_COUNT) {
        return -1;
    }
    s_piv_slot = slot;
    ESP_LOGI(TAG, "PIV storage slot selected: %u", (unsigned)(s_piv_slot + 1));
    return 0;
}

uint8_t canokey_piv_get_slot(void)
{
    return s_piv_slot;
}

static const char *canokey_piv_map_path(const char *path, char *mapped, size_t mapped_size)
{
    if (!path || s_piv_slot == 0) {
        return path;
    }

    if (strncmp(path, "piv-", 4) == 0) {
        snprintf(mapped, mapped_size, "piv-%u-%s", (unsigned)(s_piv_slot + 1), path + 4);
        return mapped;
    }

    return path;
}

int canokey_piv_storage_read_file(const char *path, void *buf, lfs_soff_t off, lfs_size_t len)
{
    char mapped[32];
    const char *real_path = canokey_piv_map_path(path, mapped, sizeof(mapped));
    int ret = __real_read_file(real_path, buf, off, len);
    ESP_LOGD(TAG,
             "read file path=%s real=%s slot=%u off=%ld len=%lu ret=%d",
             path,
             real_path,
             (unsigned)(s_piv_slot + 1),
             (long)off,
             (unsigned long)len,
             ret);
    return ret;
}

int canokey_piv_storage_write_file(const char *path, const void *buf, lfs_soff_t off, lfs_size_t len, uint8_t trunc)
{
    char mapped[32];
    const char *real_path = canokey_piv_map_path(path, mapped, sizeof(mapped));
    int ret = __real_write_file(real_path, buf, off, len, trunc);
    ESP_LOGD(TAG,
             "write file path=%s real=%s slot=%u off=%ld len=%lu trunc=%u ret=%d",
             path,
             real_path,
             (unsigned)(s_piv_slot + 1),
             (long)off,
             (unsigned long)len,
             trunc,
             ret);
    return ret;
}

int canokey_piv_storage_read_attr(const char *path, uint8_t attr, void *buf, lfs_size_t len)
{
    char mapped[32];
    const char *real_path = canokey_piv_map_path(path, mapped, sizeof(mapped));
    int ret = __real_read_attr(real_path, attr, buf, len);
    ESP_LOGD(TAG,
             "read attr path=%s real=%s slot=%u attr=0x%02x len=%lu ret=%d",
             path,
             real_path,
             (unsigned)(s_piv_slot + 1),
             attr,
             (unsigned long)len,
             ret);
    return ret;
}

int canokey_piv_storage_write_attr(const char *path, uint8_t attr, const void *buf, lfs_size_t len)
{
    char mapped[32];
    const char *real_path = canokey_piv_map_path(path, mapped, sizeof(mapped));
    int ret = __real_write_attr(real_path, attr, buf, len);
    ESP_LOGD(TAG,
             "write attr path=%s real=%s slot=%u attr=0x%02x len=%lu ret=%d",
             path,
             real_path,
             (unsigned)(s_piv_slot + 1),
             attr,
             (unsigned long)len,
             ret);
    return ret;
}

int canokey_piv_storage_remove_attr(const char *path, uint8_t attr)
{
    char mapped[32];
    const char *real_path = canokey_piv_map_path(path, mapped, sizeof(mapped));
    int ret = __real_remove_attr(real_path, attr);
    ESP_LOGD(TAG, "remove attr path=%s real=%s slot=%u attr=0x%02x ret=%d",
             path,
             real_path,
             (unsigned)(s_piv_slot + 1),
             attr,
             ret);
    return ret;
}

int canokey_piv_storage_get_file_size(const char *path)
{
    char mapped[32];
    const char *real_path = canokey_piv_map_path(path, mapped, sizeof(mapped));
    int ret = __real_get_file_size(real_path);
    ESP_LOGD(TAG, "get file size path=%s real=%s slot=%u ret=%d",
             path,
             real_path,
             (unsigned)(s_piv_slot + 1),
             ret);
    return ret;
}

int canokey_piv_storage_remove_file(const char *path)
{
    char mapped[32];
    const char *real_path = canokey_piv_map_path(path, mapped, sizeof(mapped));
    int ret = __real_remove_file(real_path);
    ESP_LOGD(TAG, "remove file path=%s real=%s slot=%u ret=%d",
             path,
             real_path,
             (unsigned)(s_piv_slot + 1),
             ret);
    return ret;
}

int __wrap_piv_platform_algorithm_extension_config_read(piv_algorithm_extension_config_t *cfg)
{
    int ret = __real_piv_platform_algorithm_extension_config_read(cfg);
    ESP_LOGD(TAG, "read algorithm extension config ret=%d", ret);
    return ret;
}

int __wrap_piv_platform_algorithm_extension_config_write(const piv_algorithm_extension_config_t *cfg)
{
    int ret = __real_piv_platform_algorithm_extension_config_write(cfg);
    ESP_LOGD(TAG, "write algorithm extension config ret=%d", ret);
    return ret;
}

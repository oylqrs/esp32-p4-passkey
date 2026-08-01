#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "canokey_gpg_slots.h"
#include "canokey_piv_slots.h"
#include "esp_log.h"
#include "fs.h"
#include "lfs.h"

static const char *TAG = "gpg_flash";
static uint8_t s_gpg_slot;

int __real_write_file(const char *path, const void *buf, lfs_soff_t off, lfs_size_t len, uint8_t trunc);
int __real_read_file(const char *path, void *buf, lfs_soff_t off, lfs_size_t len);
int __real_write_attr(const char *path, uint8_t attr, const void *buf, lfs_size_t len);
int __real_read_attr(const char *path, uint8_t attr, void *buf, lfs_size_t len);
int __real_remove_attr(const char *path, uint8_t attr);
int __real_get_file_size(const char *path);
int __real_remove_file(const char *path);
int __real_admin_platform_serial_read(uint8_t *buf);

bool canokey_gpg_flash_is_path(const char *path)
{
    return path && strncmp(path, "pgp", 3) == 0;
}

int canokey_gpg_select_slot(uint8_t slot)
{
    if (slot >= CANOKEY_GPG_SLOT_COUNT) {
        return -1;
    }
    s_gpg_slot = slot;
    ESP_LOGI(TAG, "GPG storage slot selected: %u", (unsigned)(s_gpg_slot + 1));
    return 0;
}

uint8_t canokey_gpg_get_slot(void)
{
    return s_gpg_slot;
}

int __wrap_admin_platform_serial_read(uint8_t *buf)
{
    if (!buf) {
        return -1;
    }

    if (s_gpg_slot == 0) {
        uint8_t piv_slot = canokey_piv_get_slot();
        if (piv_slot > 0) {
            buf[0] = 0x00;
            buf[1] = 0x00;
            buf[2] = 0x00;
            buf[3] = piv_slot;
            ESP_LOGI(TAG, "PIV slot %u virtual SN=%02x %02x %02x %02x",
                     (unsigned)(piv_slot + 1),
                     buf[0],
                     buf[1],
                     buf[2],
                     buf[3]);
            return 0;
        }
        return __real_admin_platform_serial_read(buf);
    }

    buf[0] = 0x00;
    buf[1] = 0x00;
    buf[2] = 0x00;
    buf[3] = s_gpg_slot;
    ESP_LOGI(TAG, "GPG slot %u virtual SN=%02x %02x %02x %02x",
             (unsigned)(s_gpg_slot + 1),
             buf[0],
             buf[1],
             buf[2],
             buf[3]);
    return 0;
}

static const char *canokey_gpg_map_path(const char *path, char *mapped, size_t mapped_size)
{
    if (!path || s_gpg_slot == 0) {
        return path;
    }

    if (strncmp(path, "pgp-", 4) == 0) {
        snprintf(mapped, mapped_size, "pgp-%u-%s", (unsigned)(s_gpg_slot + 1), path + 4);
        return mapped;
    }

    return path;
}

int canokey_gpg_flash_write_file(const char *path, const void *buf, lfs_soff_t off, lfs_size_t len, uint8_t trunc)
{
    char mapped[32];
    const char *real_path = canokey_gpg_map_path(path, mapped, sizeof(mapped));
    ESP_LOGI(TAG,
             "GPG file write path=%s real=%s slot=%u off=%ld len=%lu trunc=%u",
             path,
             real_path,
             (unsigned)(s_gpg_slot + 1),
             (long)off,
             (unsigned long)len,
             trunc);
    if (buf && len > 0) {
        lfs_size_t dump_len = len > 128 ? 128 : len;
        ESP_LOGI(TAG, "GPG file write preview len=%lu/%lu",
                 (unsigned long)dump_len,
                 (unsigned long)len);
        ESP_LOG_BUFFER_HEXDUMP(TAG, buf, dump_len, ESP_LOG_INFO);
    }

    int ret = __real_write_file(real_path, buf, off, len, trunc);
    ESP_LOGI(TAG, "GPG file write result path=%s real=%s ret=%d", path, real_path, ret);
    return ret;
}

int canokey_gpg_flash_read_file(const char *path, void *buf, lfs_soff_t off, lfs_size_t len)
{
    char mapped[32];
    const char *real_path = canokey_gpg_map_path(path, mapped, sizeof(mapped));
    int ret = __real_read_file(real_path, buf, off, len);
    ESP_LOGI(TAG,
             "GPG file read path=%s real=%s slot=%u off=%ld request_len=%lu ret=%d",
             path,
             real_path,
             (unsigned)(s_gpg_slot + 1),
             (long)off,
             (unsigned long)len,
             ret);
    return ret;
}

int canokey_gpg_flash_write_attr(const char *path, uint8_t attr, const void *buf, lfs_size_t len)
{
    char mapped[32];
    const char *real_path = canokey_gpg_map_path(path, mapped, sizeof(mapped));
    ESP_LOGI(TAG,
             "GPG attr write path=%s real=%s slot=%u attr=0x%02x len=%lu",
             path,
             real_path,
             (unsigned)(s_gpg_slot + 1),
             attr,
             (unsigned long)len);
    if (buf && len > 0) {
        lfs_size_t dump_len = len > 128 ? 128 : len;
        ESP_LOGI(TAG, "GPG attr write preview len=%lu/%lu",
                 (unsigned long)dump_len,
                 (unsigned long)len);
        ESP_LOG_BUFFER_HEXDUMP(TAG, buf, dump_len, ESP_LOG_INFO);
    }

    int ret = __real_write_attr(real_path, attr, buf, len);
    ESP_LOGI(TAG, "GPG attr write result path=%s real=%s attr=0x%02x ret=%d", path, real_path, attr, ret);
    return ret;
}

int canokey_gpg_flash_read_attr(const char *path, uint8_t attr, void *buf, lfs_size_t len)
{
    char mapped[32];
    const char *real_path = canokey_gpg_map_path(path, mapped, sizeof(mapped));
    int ret = __real_read_attr(real_path, attr, buf, len);
    ESP_LOGI(TAG,
             "GPG attr read path=%s real=%s slot=%u attr=0x%02x request_len=%lu ret=%d",
             path,
             real_path,
             (unsigned)(s_gpg_slot + 1),
             attr,
             (unsigned long)len,
             ret);
    if (ret > 0 && buf && len > 0) {
        lfs_size_t dump_len = (lfs_size_t)ret > len ? len : (lfs_size_t)ret;
        if (dump_len > 128) {
            dump_len = 128;
        }
        ESP_LOGI(TAG, "GPG attr read preview len=%lu/%d",
                 (unsigned long)dump_len,
                 ret);
        ESP_LOG_BUFFER_HEXDUMP(TAG, buf, dump_len, ESP_LOG_INFO);
    }
    return ret;
}

int canokey_gpg_flash_remove_attr(const char *path, uint8_t attr)
{
    char mapped[32];
    const char *real_path = canokey_gpg_map_path(path, mapped, sizeof(mapped));
    int ret = __real_remove_attr(real_path, attr);
    ESP_LOGI(TAG, "GPG attr remove path=%s real=%s slot=%u attr=0x%02x ret=%d",
             path,
             real_path,
             (unsigned)(s_gpg_slot + 1),
             attr,
             ret);
    return ret;
}

int canokey_gpg_flash_get_file_size(const char *path)
{
    char mapped[32];
    const char *real_path = canokey_gpg_map_path(path, mapped, sizeof(mapped));
    int ret = __real_get_file_size(real_path);
    ESP_LOGI(TAG, "GPG file size path=%s real=%s slot=%u ret=%d",
             path,
             real_path,
             (unsigned)(s_gpg_slot + 1),
             ret);
    return ret;
}

int canokey_gpg_flash_remove_file(const char *path)
{
    char mapped[32];
    const char *real_path = canokey_gpg_map_path(path, mapped, sizeof(mapped));
    int ret = __real_remove_file(real_path);
    ESP_LOGI(TAG, "GPG file remove path=%s real=%s slot=%u ret=%d",
             path,
             real_path,
             (unsigned)(s_gpg_slot + 1),
             ret);
    return ret;
}

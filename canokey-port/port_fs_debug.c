#include <string.h>

#include "esp_log.h"
#include "fs.h"
#include "lfs.h"

static const char *TAG = "canokey_fs";

int __real_write_file(const char *path, const void *buf, lfs_soff_t off, lfs_size_t len, uint8_t trunc);
int __real_read_file(const char *path, void *buf, lfs_soff_t off, lfs_size_t len);
int __real_write_attr(const char *path, uint8_t attr, const void *buf, lfs_size_t len);
int __real_read_attr(const char *path, uint8_t attr, void *buf, lfs_size_t len);
int __real_remove_attr(const char *path, uint8_t attr);
int __real_get_file_size(const char *path);
int __real_remove_file(const char *path);

bool canokey_gpg_flash_is_path(const char *path);
int canokey_gpg_flash_write_file(const char *path, const void *buf, lfs_soff_t off, lfs_size_t len, uint8_t trunc);
int canokey_gpg_flash_read_file(const char *path, void *buf, lfs_soff_t off, lfs_size_t len);
int canokey_gpg_flash_write_attr(const char *path, uint8_t attr, const void *buf, lfs_size_t len);
int canokey_gpg_flash_read_attr(const char *path, uint8_t attr, void *buf, lfs_size_t len);
int canokey_gpg_flash_remove_attr(const char *path, uint8_t attr);
int canokey_gpg_flash_get_file_size(const char *path);
int canokey_gpg_flash_remove_file(const char *path);

bool canokey_piv_storage_is_path(const char *path);
int canokey_piv_storage_read_file(const char *path, void *buf, lfs_soff_t off, lfs_size_t len);
int canokey_piv_storage_write_file(const char *path, const void *buf, lfs_soff_t off, lfs_size_t len, uint8_t trunc);
int canokey_piv_storage_read_attr(const char *path, uint8_t attr, void *buf, lfs_size_t len);
int canokey_piv_storage_write_attr(const char *path, uint8_t attr, const void *buf, lfs_size_t len);
int canokey_piv_storage_remove_attr(const char *path, uint8_t attr);
int canokey_piv_storage_get_file_size(const char *path);
int canokey_piv_storage_remove_file(const char *path);

static bool is_ctap_resident_file(const char *path)
{
    return path && (strcmp(path, "ctap_dc") == 0 || strcmp(path, "ctap_dm") == 0);
}

static const char *ctap_cert_attr_name(uint8_t attr)
{
    switch (attr) {
    case 0x00:
        return "KEY_ATTR";
    case 0x01:
        return "SIGN_CTR_ATTR";
    case 0x02:
        return "PIN_ATTR";
    case 0x03:
        return "PIN_CTR_ATTR";
    case 0x04:
        return "KH_KEY_ATTR";
    case 0x05:
        return "HE_KEY_ATTR";
    default:
        return "UNKNOWN";
    }
}

static bool is_ctap_key_file(const char *path)
{
    return path && strcmp(path, "ctap_cert") == 0;
}

int __wrap_write_file(const char *path, const void *buf, lfs_soff_t off, lfs_size_t len, uint8_t trunc)
{
    if (canokey_gpg_flash_is_path(path)) {
        return canokey_gpg_flash_write_file(path, buf, off, len, trunc);
    }
    if (canokey_piv_storage_is_path(path)) {
        return canokey_piv_storage_write_file(path, buf, off, len, trunc);
    }

    if (is_ctap_resident_file(path) || is_ctap_key_file(path)) {
        ESP_LOGW(TAG,
                 "CTAP flash write path=%s off=%ld len=%lu trunc=%u",
                 path,
                 (long)off,
                 (unsigned long)len,
                 trunc);
        if (buf && len > 0) {
            lfs_size_t dump_len = len > 128 ? 128 : len;
            ESP_LOGW(TAG, "CTAP flash write data preview len=%lu/%lu",
                     (unsigned long)dump_len,
                     (unsigned long)len);
            ESP_LOG_BUFFER_HEXDUMP(TAG, buf, dump_len, ESP_LOG_WARN);
        }
    }

    int ret = __real_write_file(path, buf, off, len, trunc);
    if (is_ctap_resident_file(path) || is_ctap_key_file(path)) {
        ESP_LOGW(TAG, "CTAP flash write result path=%s ret=%d", path, ret);
    }
    return ret;
}

int __wrap_read_file(const char *path, void *buf, lfs_soff_t off, lfs_size_t len)
{
    if (canokey_gpg_flash_is_path(path)) {
        return canokey_gpg_flash_read_file(path, buf, off, len);
    }
    if (canokey_piv_storage_is_path(path)) {
        return canokey_piv_storage_read_file(path, buf, off, len);
    }

    return __real_read_file(path, buf, off, len);
}

int __wrap_write_attr(const char *path, uint8_t attr, const void *buf, lfs_size_t len)
{
    if (canokey_gpg_flash_is_path(path)) {
        return canokey_gpg_flash_write_attr(path, attr, buf, len);
    }
    if (canokey_piv_storage_is_path(path)) {
        return canokey_piv_storage_write_attr(path, attr, buf, len);
    }

    if ((path && strcmp(path, "ctap_dc") == 0) || is_ctap_key_file(path)) {
        ESP_LOGW(TAG,
                 "CTAP flash attr write path=%s attr=0x%02x(%s) len=%lu",
                 path,
                 attr,
                 is_ctap_key_file(path) ? ctap_cert_attr_name(attr) : "DC_GENERAL_ATTR",
                 (unsigned long)len);
        if (buf && len > 0) {
            ESP_LOG_BUFFER_HEXDUMP(TAG, buf, len, ESP_LOG_WARN);
        }
    }

    int ret = __real_write_attr(path, attr, buf, len);
    if ((path && strcmp(path, "ctap_dc") == 0) || is_ctap_key_file(path)) {
        ESP_LOGW(TAG, "CTAP flash attr write result path=%s attr=0x%02x(%s) ret=%d",
                 path,
                 attr,
                 is_ctap_key_file(path) ? ctap_cert_attr_name(attr) : "DC_GENERAL_ATTR",
                 ret);
    }
    return ret;
}

int __wrap_read_attr(const char *path, uint8_t attr, void *buf, lfs_size_t len)
{
    if (canokey_gpg_flash_is_path(path)) {
        return canokey_gpg_flash_read_attr(path, attr, buf, len);
    }
    if (canokey_piv_storage_is_path(path)) {
        return canokey_piv_storage_read_attr(path, attr, buf, len);
    }

    int ret = __real_read_attr(path, attr, buf, len);
    if ((path && strcmp(path, "ctap_dc") == 0) || is_ctap_key_file(path)) {
        ESP_LOGW(TAG,
                 "CTAP flash attr read path=%s attr=0x%02x(%s) request_len=%lu ret=%d",
                 path,
                 attr,
                 is_ctap_key_file(path) ? ctap_cert_attr_name(attr) : "DC_GENERAL_ATTR",
                 (unsigned long)len,
                 ret);
        if (ret > 0 && buf && len > 0) {
            lfs_size_t dump_len = (lfs_size_t)ret > len ? len : (lfs_size_t)ret;
            ESP_LOGW(TAG,
                     "CTAP flash attr read data preview len=%lu/%d",
                     (unsigned long)dump_len,
                     ret);
            ESP_LOG_BUFFER_HEXDUMP(TAG, buf, dump_len, ESP_LOG_WARN);
        }
    }
    return ret;
}

int __wrap_remove_attr(const char *path, uint8_t attr)
{
    if (canokey_gpg_flash_is_path(path)) {
        return canokey_gpg_flash_remove_attr(path, attr);
    }
    if (canokey_piv_storage_is_path(path)) {
        return canokey_piv_storage_remove_attr(path, attr);
    }

    return __real_remove_attr(path, attr);
}

int __wrap_get_file_size(const char *path)
{
    if (canokey_gpg_flash_is_path(path)) {
        return canokey_gpg_flash_get_file_size(path);
    }
    if (canokey_piv_storage_is_path(path)) {
        return canokey_piv_storage_get_file_size(path);
    }

    return __real_get_file_size(path);
}

int __wrap_remove_file(const char *path)
{
    if (canokey_gpg_flash_is_path(path)) {
        return canokey_gpg_flash_remove_file(path);
    }
    if (canokey_piv_storage_is_path(path)) {
        return canokey_piv_storage_remove_file(path);
    }

    return __real_remove_file(path);
}

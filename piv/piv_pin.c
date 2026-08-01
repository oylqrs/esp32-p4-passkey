#include <stdbool.h>
#include <string.h>

#include "esp_log.h"
#include "pin.h"

static const char *TAG = "piv_pin";

int __real_pin_create(const pin_t *pin, const void *buf, uint8_t len, uint8_t max_retries);
int __real_pin_verify(pin_t *pin, const void *buf, uint8_t len, uint8_t *retries);
int __real_pin_update(pin_t *pin, const void *buf, uint8_t len);
int __real_pin_get_size(const pin_t *pin);
int __real_pin_get_retries(const pin_t *pin);
int __real_pin_get_default_retries(const pin_t *pin);
int __real_pin_set_retries(const pin_t *pin, uint8_t max_retries);
int __real_pin_clear(const pin_t *pin);

static bool canokey_piv_pin_is_path(const char *path)
{
    return path && (strcmp(path, "piv-pin") == 0 || strcmp(path, "piv-puk") == 0);
}

static bool canokey_piv_pin_is_pin(const pin_t *pin)
{
    return pin && canokey_piv_pin_is_path(pin->path);
}

int canokey_piv_pin_create(const pin_t *pin, const void *buf, uint8_t len, uint8_t max_retries)
{
    int ret = __real_pin_create(pin, buf, len, max_retries);
    ESP_LOGD(TAG,
             "create path=%s len=%u max_retries=%u ret=%d",
             pin ? pin->path : "(null)",
             len,
             max_retries,
             ret);
    return ret;
}

int canokey_piv_pin_verify(pin_t *pin, const void *buf, uint8_t len, uint8_t *retries)
{
    int ret = __real_pin_verify(pin, buf, len, retries);
    ESP_LOGD(TAG,
             "verify path=%s len=%u ret=%d",
             pin ? pin->path : "(null)",
             len,
             ret);
    return ret;
}

int canokey_piv_pin_update(pin_t *pin, const void *buf, uint8_t len)
{
    int ret = __real_pin_update(pin, buf, len);
    ESP_LOGD(TAG, "update path=%s len=%u ret=%d", pin ? pin->path : "(null)", len, ret);
    return ret;
}

int canokey_piv_pin_get_size(const pin_t *pin)
{
    int ret = __real_pin_get_size(pin);
    ESP_LOGD(TAG, "get size path=%s ret=%d", pin ? pin->path : "(null)", ret);
    return ret;
}

int canokey_piv_pin_get_retries(const pin_t *pin)
{
    int ret = __real_pin_get_retries(pin);
    ESP_LOGD(TAG, "get retries path=%s ret=%d", pin ? pin->path : "(null)", ret);
    return ret;
}

int canokey_piv_pin_get_default_retries(const pin_t *pin)
{
    int ret = __real_pin_get_default_retries(pin);
    ESP_LOGD(TAG, "get default retries path=%s ret=%d", pin ? pin->path : "(null)", ret);
    return ret;
}

int canokey_piv_pin_set_retries(const pin_t *pin, uint8_t max_retries)
{
    int ret = __real_pin_set_retries(pin, max_retries);
    ESP_LOGD(TAG,
             "set retries path=%s max_retries=%u ret=%d",
             pin ? pin->path : "(null)",
             max_retries,
             ret);
    return ret;
}

int canokey_piv_pin_clear(const pin_t *pin)
{
    int ret = __real_pin_clear(pin);
    ESP_LOGD(TAG, "clear path=%s ret=%d", pin ? pin->path : "(null)", ret);
    return ret;
}

int __wrap_pin_create(const pin_t *pin, const void *buf, uint8_t len, uint8_t max_retries)
{
    if (canokey_piv_pin_is_pin(pin)) {
        return canokey_piv_pin_create(pin, buf, len, max_retries);
    }

    return __real_pin_create(pin, buf, len, max_retries);
}

int __wrap_pin_verify(pin_t *pin, const void *buf, uint8_t len, uint8_t *retries)
{
    if (canokey_piv_pin_is_pin(pin)) {
        return canokey_piv_pin_verify(pin, buf, len, retries);
    }

    return __real_pin_verify(pin, buf, len, retries);
}

int __wrap_pin_update(pin_t *pin, const void *buf, uint8_t len)
{
    if (canokey_piv_pin_is_pin(pin)) {
        return canokey_piv_pin_update(pin, buf, len);
    }

    return __real_pin_update(pin, buf, len);
}

int __wrap_pin_get_size(const pin_t *pin)
{
    if (canokey_piv_pin_is_pin(pin)) {
        return canokey_piv_pin_get_size(pin);
    }

    return __real_pin_get_size(pin);
}

int __wrap_pin_get_retries(const pin_t *pin)
{
    if (canokey_piv_pin_is_pin(pin)) {
        return canokey_piv_pin_get_retries(pin);
    }

    return __real_pin_get_retries(pin);
}

int __wrap_pin_get_default_retries(const pin_t *pin)
{
    if (canokey_piv_pin_is_pin(pin)) {
        return canokey_piv_pin_get_default_retries(pin);
    }

    return __real_pin_get_default_retries(pin);
}

int __wrap_pin_set_retries(const pin_t *pin, uint8_t max_retries)
{
    if (canokey_piv_pin_is_pin(pin)) {
        return canokey_piv_pin_set_retries(pin, max_retries);
    }

    return __real_pin_set_retries(pin, max_retries);
}

int __wrap_pin_clear(const pin_t *pin)
{
    if (canokey_piv_pin_is_pin(pin)) {
        return canokey_piv_pin_clear(pin);
    }

    return __real_pin_clear(pin);
}

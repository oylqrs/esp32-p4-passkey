#include <stdint.h>

#include "esp_log.h"
#include "piv.h"

static const char *TAG = "piv_port";

int __real_piv_install(uint8_t reset);
void __real_piv_poweroff(void);

int canokey_piv_port_install(uint8_t reset)
{
    ESP_LOGI(TAG, "PIV install start reset=%u", (unsigned)reset);
    int ret = __real_piv_install(reset);
    ESP_LOGI(TAG, "PIV install done reset=%u ret=%d", (unsigned)reset, ret);
    return ret;
}

void canokey_piv_port_poweroff(void)
{
    ESP_LOGD(TAG, "PIV poweroff");
    __real_piv_poweroff();
}

int __wrap_piv_install(uint8_t reset)
{
    return canokey_piv_port_install(reset);
}

void __wrap_piv_poweroff(void)
{
    canokey_piv_port_poweroff();
}

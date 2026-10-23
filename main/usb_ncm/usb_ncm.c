#include "usb_ncm/usb_ncm.h"

#include <errno.h>
#include <inttypes.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "PCF85063/pcf85063.h"
#include "browser_autofill/keepass_http_server.h"
#include "dhcpserver/dhcpserver_options.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/esp_netif_net_stack.h"
#include "lwip/sockets.h"
#include "nvs.h"
#include "tinyusb_net.h"
#include "ui/timezone_setup_ui.h"

#define USB_NCM_IP_NVS_NAMESPACE "usb_ncm"
#define USB_NCM_IP_NVS_KEY "ip"
#define USB_NCM_DEFAULT_IP "172.16.8.1"
#define USB_NCM_TIME_SYNC_PORT 22
#define USB_NCM_TIME_SYNC_TASK_STACK 4096
#define USB_NCM_TIME_SYNC_TASK_PRIORITY 4
#define USB_NCM_TIME_SYNC_RX_TIMEOUT_US 250000
#define USB_NCM_MIN_TIMESTAMP UINT64_C(946684800)
#define USB_NCM_MAX_TIMESTAMP UINT64_C(4102444799)

static const char *TAG = "usb_ncm";
static esp_netif_t *s_netif;
static TaskHandle_t s_time_sync_task;
static atomic_bool s_time_sync_running;

static bool usb_ncm_parse_ip(const char *address, uint8_t octets[4])
{
    if (!address) {
        return false;
    }

    const char *cursor = address;
    for (size_t i = 0; i < 4; ++i) {
        unsigned value = 0;
        size_t digits = 0;
        while (*cursor >= '0' && *cursor <= '9' && digits < 3) {
            value = value * 10 + (unsigned)(*cursor++ - '0');
            ++digits;
        }
        if (!digits || value > 255 || (i < 3 ? *cursor++ != '.' : *cursor != '\0')) {
            return false;
        }
        octets[i] = (uint8_t)value;
    }

    bool private_address = octets[0] == 10 ||
                           (octets[0] == 172 && octets[1] >= 16 && octets[1] <= 31) ||
                           (octets[0] == 192 && octets[1] == 168);
    return private_address && octets[3] > 0 && octets[3] < 255;
}

void usb_ncm_get_ip_address(char address[USB_NCM_IP_ADDRESS_LEN])
{
    memcpy(address, USB_NCM_DEFAULT_IP, sizeof(USB_NCM_DEFAULT_IP));

    nvs_handle_t nvs;
    if (nvs_open(USB_NCM_IP_NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        return;
    }

    char stored[USB_NCM_IP_ADDRESS_LEN];
    size_t length = sizeof(stored);
    esp_err_t ret = nvs_get_str(nvs, USB_NCM_IP_NVS_KEY, stored, &length);
    nvs_close(nvs);
    uint8_t octets[4];
    if (ret == ESP_OK && usb_ncm_parse_ip(stored, octets)) {
        memcpy(address, stored, length);
    }
}

esp_err_t usb_ncm_set_ip_address(const char *address)
{
    uint8_t octets[4];
    if (!usb_ncm_parse_ip(address, octets)) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(USB_NCM_IP_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = nvs_set_str(nvs, USB_NCM_IP_NVS_KEY, address);
    if (ret == ESP_OK) {
        ret = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return ret;
}

static esp_err_t usb_ncm_sync_rtc(const char *message)
{
    static const char prefix[] = "timestamp=";
    if (strncmp(message, prefix, sizeof(prefix) - 1) != 0) {
        return ESP_ERR_INVALID_ARG;
    }

    const char *value = message + sizeof(prefix) - 1;
    char *end = NULL;
    errno = 0;
    uint64_t timestamp = strtoull(value, &end, 10);
    if (errno == ERANGE || end == value) {
        return ESP_ERR_INVALID_ARG;
    }
    while (*end == ' ' || *end == '\r' || *end == '\n' || *end == '\t') {
        end++;
    }
    if (*end != '\0' || timestamp < USB_NCM_MIN_TIMESTAMP ||
        timestamp > USB_NCM_MAX_TIMESTAMP) {
        return ESP_ERR_INVALID_ARG;
    }

    int16_t utc_offset_minutes = timezone_setup_get_utc_offset_minutes();
    time_t local_timestamp = (time_t)(timestamp + (int64_t)utc_offset_minutes * 60);
    struct tm local_time;
    if (gmtime_r(&local_timestamp, &local_time) == NULL) {
        return ESP_FAIL;
    }

    const pcf85063_time_t rtc_time = {
        .year = (uint16_t)(local_time.tm_year + 1900),
        .month = (uint8_t)(local_time.tm_mon + 1),
        .day = (uint8_t)local_time.tm_mday,
        .weekday = (uint8_t)local_time.tm_wday,
        .hour = (uint8_t)local_time.tm_hour,
        .minute = (uint8_t)local_time.tm_min,
        .second = (uint8_t)local_time.tm_sec,
    };
    ESP_RETURN_ON_ERROR(pcf85063_set_time(&rtc_time), TAG, "write RTC failed");
    ESP_LOGI(TAG,
             "RTC synchronized: timestamp=%" PRIu64 " local=%04u-%02u-%02u %02u:%02u:%02u UTC%+d:%02d",
             timestamp, rtc_time.year, rtc_time.month, rtc_time.day,
             rtc_time.hour, rtc_time.minute, rtc_time.second,
             utc_offset_minutes / 60, abs(utc_offset_minutes % 60));
    return ESP_OK;
}

static void usb_ncm_time_sync_task(void *arg)
{
    (void)arg;
    int socket_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (socket_fd < 0) {
        ESP_LOGE(TAG, "create time sync socket failed: errno=%d", errno);
        goto done;
    }

    const struct timeval timeout = {
        .tv_sec = 0,
        .tv_usec = USB_NCM_TIME_SYNC_RX_TIMEOUT_US,
    };
    if (setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0) {
        ESP_LOGE(TAG, "set time sync socket timeout failed: errno=%d", errno);
        goto done;
    }

    const struct sockaddr_in listen_address = {
        .sin_family = AF_INET,
        .sin_port = htons(USB_NCM_TIME_SYNC_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(socket_fd, (const struct sockaddr *)&listen_address, sizeof(listen_address)) < 0) {
        ESP_LOGE(TAG, "bind UDP/%d failed: errno=%d", USB_NCM_TIME_SYNC_PORT, errno);
        goto done;
    }

    ESP_LOGI(TAG, "RTC time sync listening on UDP/%d", USB_NCM_TIME_SYNC_PORT);
    while (atomic_load_explicit(&s_time_sync_running, memory_order_acquire)) {
        char message[64];
        struct sockaddr_storage source_address;
        socklen_t source_length = sizeof(source_address);
        int received = recvfrom(socket_fd, message, sizeof(message) - 1, 0,
                                (struct sockaddr *)&source_address, &source_length);
        if (received < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            if (atomic_load_explicit(&s_time_sync_running, memory_order_relaxed)) {
                ESP_LOGE(TAG, "receive time sync packet failed: errno=%d", errno);
            }
            break;
        }

        message[received] = '\0';
        esp_err_t ret = usb_ncm_sync_rtc(message);
        const char *reply = ret == ESP_OK ? "OK" : "ERROR";
        (void)sendto(socket_fd, reply, strlen(reply), 0,
                     (const struct sockaddr *)&source_address, source_length);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Rejected time sync message '%s': %s",
                     message, esp_err_to_name(ret));
        }
    }

done:
    if (socket_fd >= 0) {
        close(socket_fd);
    }
    atomic_store_explicit(&s_time_sync_running, false, memory_order_release);
    s_time_sync_task = NULL;
    vTaskDelete(NULL);
}

static esp_err_t usb_ncm_time_sync_start(void)
{
    if (s_time_sync_task) {
        return ESP_OK;
    }

    atomic_store_explicit(&s_time_sync_running, true, memory_order_release);
    if (xTaskCreate(usb_ncm_time_sync_task, "ncm_time_sync",
                    USB_NCM_TIME_SYNC_TASK_STACK, NULL,
                    USB_NCM_TIME_SYNC_TASK_PRIORITY,
                    &s_time_sync_task) != pdPASS) {
        atomic_store_explicit(&s_time_sync_running, false, memory_order_release);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static void usb_ncm_time_sync_stop(void)
{
    atomic_store_explicit(&s_time_sync_running, false, memory_order_release);
    while (s_time_sync_task) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

static void usb_ncm_free_rx_buffer(void *handle, void *buffer)
{
    (void)handle;
    free(buffer);
}

static esp_err_t usb_ncm_transmit(void *handle, void *buffer, size_t length)
{
    (void)handle;
    return tinyusb_net_send_sync(buffer, (uint16_t)length, NULL, pdMS_TO_TICKS(100));
}

static esp_err_t usb_ncm_receive(void *buffer, uint16_t length, void *context)
{
    (void)context;
    if (!s_netif) {
        return ESP_ERR_INVALID_STATE;
    }

    void *copy = malloc(length);
    if (!copy) {
        return ESP_ERR_NO_MEM;
    }
    memcpy(copy, buffer, length);
    esp_err_t ret = esp_netif_receive(s_netif, copy, length, NULL);
    if (ret != ESP_OK) {
        free(copy);
    }
    return ret;
}

esp_err_t usb_ncm_init(void)
{
    if (s_netif) {
        return ESP_OK;
    }

    esp_err_t ret = esp_netif_init();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        return ret;
    }

    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        return ret;
    }

    const tinyusb_net_config_t net_config = {
        .mac_addr = {0x02, 0x16, 0x08, 0x00, 0x00, 0x01},
        .on_recv_callback = usb_ncm_receive,
    };
    ESP_RETURN_ON_ERROR(tinyusb_net_init(&net_config), TAG, "TinyUSB NCM init failed");

    char ip_address[USB_NCM_IP_ADDRESS_LEN];
    uint8_t octets[4];
    usb_ncm_get_ip_address(ip_address);
    (void)usb_ncm_parse_ip(ip_address, octets);
    esp_netif_ip_info_t ip_info = {
        .ip = {.addr = ESP_IP4TOADDR(octets[0], octets[1], octets[2], octets[3])},
        .gw = {.addr = ESP_IP4TOADDR(octets[0], octets[1], octets[2], octets[3])},
        .netmask = {.addr = ESP_IP4TOADDR(255, 255, 255, 0)},
    };
    const esp_netif_inherent_config_t base_config = {
        .flags = ESP_NETIF_DHCP_SERVER | ESP_NETIF_FLAG_AUTOUP,
        .ip_info = &ip_info,
        .if_key = "usb_ncm",
        .if_desc = "USB NCM virtual network",
        .route_prio = 10,
    };
    static const esp_netif_driver_ifconfig_t driver_config = {
        .handle = (void *)1,
        .transmit = usb_ncm_transmit,
        .driver_free_rx_buffer = usb_ncm_free_rx_buffer,
    };
    static const struct esp_netif_netstack_config stack_config = {
        .lwip = {
            .init_fn = ethernetif_init,
            .input_fn = ethernetif_input,
        },
    };
    const esp_netif_config_t config = {
        .base = &base_config,
        .driver = &driver_config,
        .stack = &stack_config,
    };

    s_netif = esp_netif_new(&config);
    if (!s_netif) {
        tinyusb_net_deinit();
        return ESP_ERR_NO_MEM;
    }

    uint8_t lwip_mac[6] = {0x02, 0x16, 0x08, 0x00, 0x00, 0x02};
    ret = esp_netif_set_mac(s_netif, lwip_mac);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "set NCM MAC failed: %s", esp_err_to_name(ret));
        goto fail;
    }

    uint32_t lease_minutes = 1;
    ret = esp_netif_dhcps_option(s_netif, ESP_NETIF_OP_SET,
                                 IP_ADDRESS_LEASE_TIME,
                                 &lease_minutes,
                                 sizeof(lease_minutes));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "set DHCP lease failed: %s", esp_err_to_name(ret));
        goto fail;
    }
    esp_netif_action_start(s_netif, NULL, 0, NULL);
    ret = usb_ncm_time_sync_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "start RTC time sync failed: %s", esp_err_to_name(ret));
        esp_netif_action_stop(s_netif, NULL, 0, NULL);
        goto fail;
    }
    ret = keepass_http_server_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "start KeePassHTTP failed: %s", esp_err_to_name(ret));
        usb_ncm_time_sync_stop();
        esp_netif_action_stop(s_netif, NULL, 0, NULL);
        goto fail;
    }
    ESP_LOGI(TAG, "USB NCM ready at %s/24", ip_address);
    return ESP_OK;

fail:
    esp_netif_destroy(s_netif);
    s_netif = NULL;
    tinyusb_net_deinit();
    return ret;
}

void usb_ncm_deinit(void)
{
    keepass_http_server_stop();
    usb_ncm_time_sync_stop();
    if (!s_netif) {
        tinyusb_net_deinit();
        return;
    }

    esp_netif_action_stop(s_netif, NULL, 0, NULL);
    esp_netif_destroy(s_netif);
    s_netif = NULL;
    tinyusb_net_deinit();
    ESP_LOGI(TAG, "USB NCM stopped");
}

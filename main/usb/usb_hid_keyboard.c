#include "usb_hid_keyboard.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "canokey_esp32p4.h"
#include "ctaphid.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "diskio_impl.h"
#include "ff.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "usb_msc.h"
#include "usbd_ctaphid.h"
#include "wear_levelling.h"

#define HID_TASK_STACK_SIZE     4096
#define HID_TASK_PRIORITY       5
#define HID_QUEUE_DEPTH         4
#define HID_TEXT_MAX_LEN        160
#define HID_REPORT_INTERVAL_MS  12
#define HID_MOUNT_TIMEOUT_MS    3000
#define MSC_DUMP_TASK_STACK     4096
#define MSC_DUMP_TASK_PRIORITY  4
#define MSC_DUMP_CHUNK_SIZE     128
#define MSC_FILE_READ_MAX_BYTES (1024)
#define MSC_PATH_MAX_LEN        256
#define MSC_MOUNT_TIMEOUT_MS    3000
#define MSC_MOUNT_APP_DONE_BIT  BIT0
#define MSC_MOUNT_USB_DONE_BIT  BIT1
#define MSC_MOUNT_FAILED_BIT    BIT2
#define MSC_MOUNT_BASE_PATH     "/data"

#define HID_ITF_NUM_KEYBOARD    0
#define HID_ITF_NUM_CTAPHID     1
#define USB_MSC_DEFAULT_ENABLED 1
#define USB_USE_ESP32P4_HIGH_SPEED 0
#if USB_MSC_DEFAULT_ENABLED
#define MSC_ITF_NUM             2
#define USB_ITF_NUM_TOTAL       3
#else
#define USB_ITF_NUM_TOTAL       2
#endif

#define HID_STR_INDEX           4
#define CTAPHID_STR_INDEX       5
#if USB_MSC_DEFAULT_ENABLED
#define MSC_STR_INDEX           6
#endif

#define HID_EP_IN               0x81
#define CTAPHID_EP_OUT          0x03
#define CTAPHID_EP_IN           0x83
#define CTAPHID_EP_SIZE         64
#if USB_MSC_DEFAULT_ENABLED
#define MSC_EP_OUT              0x02
#define MSC_EP_IN               0x82
#define MSC_EP_SIZE_FS          64
#define MSC_EP_SIZE_HS          512
#endif

#if USB_MSC_DEFAULT_ENABLED
#define TUSB_DESC_TOTAL_LEN     (TUD_CONFIG_DESC_LEN + TUD_HID_DESC_LEN + TUD_HID_INOUT_DESC_LEN + TUD_MSC_DESC_LEN)
#else
#define TUSB_DESC_TOTAL_LEN     (TUD_CONFIG_DESC_LEN + TUD_HID_DESC_LEN + TUD_HID_INOUT_DESC_LEN)
#endif

#define MSC_PARTITION_LABEL     "usbstore"
#define MSC_NVS_NAMESPACE       "usbstore"
#define MSC_NVS_FORMAT_KEY      "fmt_v2_done"

static const char *TAG = "usb_hid";

typedef struct {
    char text[HID_TEXT_MAX_LEN];
} hid_text_msg_t;

static QueueHandle_t s_hid_queue;
static bool s_hid_started;
static bool s_dump_in_progress;
static wl_handle_t s_msc_wl_handle = WL_INVALID_HANDLE;

#include "usb_hid_keyboard.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "class/hid/hid_device.h"
#include "canokey_esp32p4.h"
#include "ctaphid.h"
#include "usb_ncm/usb_ncm.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "log_storage.h"
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
#include "tinyusb_msc.h"
#include "usb_ccid.h"
#include "keyboard_layout.h"
#include "usbd_ctaphid.h"
#include "wear_levelling.h"

#define HID_TASK_STACK_SIZE     4096
#define HID_TASK_PRIORITY       5
#define HID_QUEUE_DEPTH         4
#define HID_TEXT_MAX_LEN        160
#define HID_REPORT_INTERVAL_MS  25
#define HID_MOUNT_TIMEOUT_MS    3000
#define HID_REPORT_TIMEOUT_MS   1000
#define USB_DISCONNECT_DELAY_MS 500
#define USB_REINSTALL_DELAY_MS  100
#define USB_ENUM_SETTLE_MS      500
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
#define CCID_ITF_NUM            2

#define USB_MSC_DEFAULT_ENABLED 1
#define USB_USE_ESP32P4_HIGH_SPEED 0
#if USB_MSC_DEFAULT_ENABLED
#define MSC_ITF_NUM             3
#define USB_ITF_NUM_TOTAL       4
#else
#define USB_ITF_NUM_TOTAL       3
#endif
#if !USB_CCID_ENABLED
#undef MSC_ITF_NUM
#undef USB_ITF_NUM_TOTAL
#if USB_MSC_DEFAULT_ENABLED
#define MSC_ITF_NUM             2
#define USB_ITF_NUM_TOTAL       3
#else
#define USB_ITF_NUM_TOTAL       2
#endif
#endif

#define MSC_ITF_NUM_NO_CCID 2
#if USB_MSC_DEFAULT_ENABLED
#define USB_ITF_NUM_TOTAL_NO_CCID 3
#else
#define USB_ITF_NUM_TOTAL_NO_CCID 2
#endif

#define HID_STR_INDEX           4
#define CTAPHID_STR_INDEX       5
#define AUX_INTERFACE_STR_INDEX 7
#if USB_MSC_DEFAULT_ENABLED
#define MSC_STR_INDEX           AUX_INTERFACE_STR_INDEX
#endif
#define NCM_MAC_STR_INDEX       6
#define NCM_STR_INDEX           AUX_INTERFACE_STR_INDEX

#define HID_EP_IN               0x81
#define CTAPHID_EP_OUT          0x03
#define CTAPHID_EP_IN           0x83
#define CTAPHID_EP_SIZE         64
#define CCID_EP_OUT             0x04
#define CCID_EP_IN              0x84
#define CCID_EP_INT_IN          0x85
#define CCID_EP_SIZE            64
#define CCID_EP_SIZE_HS         512
#define CCID_EP_INT_SIZE        16
#define CCID_U16_LSB(value)     ((uint8_t)((value) & 0xFFu))
#define CCID_U16_MSB(value)     ((uint8_t)(((value) >> 8) & 0xFFu))
#if USB_MSC_DEFAULT_ENABLED
#define MSC_EP_OUT              0x02
#define MSC_EP_IN               0x82
#define MSC_EP_SIZE_FS          64
#define MSC_EP_SIZE_HS          512
#endif

#if USB_CCID_ENABLED && USB_MSC_DEFAULT_ENABLED
#define TUSB_DESC_TOTAL_LEN     (TUD_CONFIG_DESC_LEN + TUD_HID_DESC_LEN + TUD_HID_INOUT_DESC_LEN + 84 + TUD_MSC_DESC_LEN)
#elif USB_CCID_ENABLED
#define TUSB_DESC_TOTAL_LEN     (TUD_CONFIG_DESC_LEN + TUD_HID_DESC_LEN + TUD_HID_INOUT_DESC_LEN + 84)
#elif USB_MSC_DEFAULT_ENABLED
#define TUSB_DESC_TOTAL_LEN     (TUD_CONFIG_DESC_LEN + TUD_HID_DESC_LEN + TUD_HID_INOUT_DESC_LEN + TUD_MSC_DESC_LEN)
#else
#define TUSB_DESC_TOTAL_LEN     (TUD_CONFIG_DESC_LEN + TUD_HID_DESC_LEN + TUD_HID_INOUT_DESC_LEN)
#endif

#if USB_MSC_DEFAULT_ENABLED
#define TUSB_DESC_TOTAL_LEN_NO_CCID (TUD_CONFIG_DESC_LEN + TUD_HID_DESC_LEN + TUD_HID_INOUT_DESC_LEN + TUD_MSC_DESC_LEN)
#else
#define TUSB_DESC_TOTAL_LEN_NO_CCID (TUD_CONFIG_DESC_LEN + TUD_HID_DESC_LEN + TUD_HID_INOUT_DESC_LEN)
#endif

#define MSC_PARTITION_LABEL     "usbstore"
#define MSC_NVS_NAMESPACE       "usbstore"
#define MSC_NVS_FORMAT_KEY      "fmt_v2_done"
#define CONNECT_SETTINGS_NVS_NAMESPACE "connect_ui"
#define CONNECT_HID_LAYOUT_NVS_KEY    "hid_layout"
#define CONNECT_USB_VID_NVS_KEY       "usb_vid"
#define USB_VID_DEFAULT               0x303AU
#define USB_VID_MIN                   0x0001U
#define USB_VID_MAX                   0xFFFEU
#define MSC_ERASE_CHUNK_SECTORS 16U
#define MSC_ERASE_LOG_BYTES     (1024U * 1024U)

static const char *TAG = "usb_hid";
static esp_err_t usbstore_set_mount_point(tinyusb_msc_mount_point_t mount_point);

typedef enum {
    HID_MESSAGE_TEXT,
    HID_MESSAGE_KEY_PRESS,
    HID_MESSAGE_KEY_RELEASE,
    HID_MESSAGE_DELAY,
    HID_MESSAGE_SYNC,
} hid_message_type_t;

typedef struct {
    hid_message_type_t type;
    uint8_t key;
    uint8_t modifier;
    uint32_t delay_ms;
    TaskHandle_t waiter;
    char text[HID_TEXT_MAX_LEN];
} hid_text_msg_t;

typedef enum {
    USB_MODE_HID_ONLY,
    USB_MODE_HID_CCID,
    USB_MODE_HID_MSC,
    USB_MODE_HID_NCM,
    USB_MODE_HID_JADE_CDC,
} usb_mode_t;

static QueueHandle_t s_hid_queue;
static bool s_hid_started;
static bool s_dump_in_progress;
static bool s_smartcard_reader_enabled;
static bool s_usbstore_exposed_to_usb;
static bool s_ncm_enabled;
static bool s_jade_cdc_enabled;
static usb_mode_t s_usb_mode = USB_MODE_HID_ONLY;
static wl_handle_t s_msc_wl_handle = WL_INVALID_HANDLE;
static tinyusb_msc_storage_handle_t s_msc_storage;
static EventGroupHandle_t s_msc_event_group;
static uint8_t s_msc_fat_partition_number;

static const uint8_t s_hid_report_descriptor[] = {
    TUD_HID_REPORT_DESC_KEYBOARD(HID_REPORT_ID(HID_ITF_PROTOCOL_KEYBOARD)),
};

static const uint8_t s_ctaphid_report_descriptor[] = {
    0x06, 0xD0, 0xF1,
    0x09, 0x01,
    0xA1, 0x01,
    0x09, 0x20,
    0x15, 0x00,
    0x26, 0xFF, 0x00,
    0x75, 0x08,
    0x95, 0x40,
    0x81, 0x02,
    0x09, 0x21,
    0x15, 0x00,
    0x26, 0xFF, 0x00,
    0x75, 0x08,
    0x95, 0x40,
    0x91, 0x02,
    0xC0,
};
/*修改 open PGP的名字  */
static const char *s_hid_string_descriptor[] = {
    (char[]){0x09, 0x04},
    "Miixkey",
    "MiixKey HID Keyboard + Flash Disk",
    "000001",
    "Keyboard",
    "FIDO2/U2F",
    "021608000001",
    "MIIX-USB",
};

_Static_assert(sizeof(s_hid_string_descriptor) / sizeof(s_hid_string_descriptor[0]) <= 8,
               "esp_tinyusb supports at most 8 USB string descriptors");

static const tusb_desc_device_t s_usb_hid_device_descriptor = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = 0x00,
    .bDeviceSubClass = 0x00,
    .bDeviceProtocol = 0x00,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = 0x303A,
    .idProduct = 0x4010,
    .bcdDevice = 0x0100,
    .iManufacturer = 0x01,
    .iProduct = 0x02,
    .iSerialNumber = 0x03,
    .bNumConfigurations = 0x01,
};

static const tusb_desc_device_t s_usb_ccid_device_descriptor = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = 0x00,
    .bDeviceSubClass = 0x00,
    .bDeviceProtocol = 0x00,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = 0x303A,
    .idProduct = 0x4011,
    .bcdDevice = 0x0100,
    .iManufacturer = 0x01,
    .iProduct = 0x02,
    .iSerialNumber = 0x03,
    .bNumConfigurations = 0x01,
};

static const tusb_desc_device_t s_usb_msc_device_descriptor = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = 0x00,
    .bDeviceSubClass = 0x00,
    .bDeviceProtocol = 0x00,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = 0x303A,
    .idProduct = 0x4012,
    .bcdDevice = 0x0100,
    .iManufacturer = 0x01,
    .iProduct = 0x02,
    .iSerialNumber = 0x03,
    .bNumConfigurations = 0x01,
};

#if (TUD_OPT_HIGH_SPEED)
static const tusb_desc_device_qualifier_t s_usb_device_qualifier = {
    .bLength = sizeof(tusb_desc_device_qualifier_t),
    .bDescriptorType = TUSB_DESC_DEVICE_QUALIFIER,
    .bcdUSB = 0x0200,
    .bDeviceClass = 0x00,
    .bDeviceSubClass = 0x00,
    .bDeviceProtocol = 0x00,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .bNumConfigurations = 0x01,
    .bReserved = 0x00,
};
#endif

static const uint8_t s_usb_configuration_descriptor_hid_fs[] = {
    TUD_CONFIG_DESCRIPTOR(1, 2, 0, TUD_CONFIG_DESC_LEN + TUD_HID_DESC_LEN + TUD_HID_INOUT_DESC_LEN, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    TUD_HID_DESCRIPTOR(HID_ITF_NUM_KEYBOARD, HID_STR_INDEX, false, sizeof(s_hid_report_descriptor), HID_EP_IN, 16, 10),
    TUD_HID_INOUT_DESCRIPTOR(HID_ITF_NUM_CTAPHID, CTAPHID_STR_INDEX, HID_ITF_PROTOCOL_NONE,
                             sizeof(s_ctaphid_report_descriptor), CTAPHID_EP_OUT, CTAPHID_EP_IN,
                             CTAPHID_EP_SIZE, 5),
};

static const tusb_desc_device_t s_usb_ncm_device_descriptor = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = 0x303A,
    .idProduct = 0x4013,
    .bcdDevice = 0x0100,
    .iManufacturer = 0x01,
    .iProduct = 0x02,
    .iSerialNumber = 0x03,
    .bNumConfigurations = 0x01,
};

static const tusb_desc_device_t s_usb_jade_device_descriptor = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = 0x303A,
    .idProduct = 0x4014,
    .bcdDevice = 0x0100,
    .iManufacturer = 0x01,
    .iProduct = 0x02,
    .iSerialNumber = 0x03,
    .bNumConfigurations = 0x01,
};

static tusb_desc_device_t s_usb_active_device_descriptor;

#if (TUD_OPT_HIGH_SPEED)
static const uint8_t s_usb_configuration_descriptor_hid_hs[] = {
    TUD_CONFIG_DESCRIPTOR(1, 2, 0, TUD_CONFIG_DESC_LEN + TUD_HID_DESC_LEN + TUD_HID_INOUT_DESC_LEN, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    TUD_HID_DESCRIPTOR(HID_ITF_NUM_KEYBOARD, HID_STR_INDEX, false, sizeof(s_hid_report_descriptor), HID_EP_IN, 16, 10),
    TUD_HID_INOUT_DESCRIPTOR(HID_ITF_NUM_CTAPHID, CTAPHID_STR_INDEX, HID_ITF_PROTOCOL_NONE,
                             sizeof(s_ctaphid_report_descriptor), CTAPHID_EP_OUT, CTAPHID_EP_IN,
                             CTAPHID_EP_SIZE, 5),
};

static const tusb_desc_device_qualifier_t s_usb_ncm_device_qualifier = {
    .bLength = sizeof(tusb_desc_device_qualifier_t),
    .bDescriptorType = TUSB_DESC_DEVICE_QUALIFIER,
    .bcdUSB = 0x0200,
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .bNumConfigurations = 0x01,
    .bReserved = 0x00,
};
#endif

static const uint8_t s_usb_configuration_descriptor_hid_ncm_fs[] = {
    TUD_CONFIG_DESCRIPTOR(1, 4, 0,
                          TUD_CONFIG_DESC_LEN + TUD_HID_DESC_LEN +
                          TUD_HID_INOUT_DESC_LEN + TUD_CDC_NCM_DESC_LEN,
                          TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    TUD_HID_DESCRIPTOR(HID_ITF_NUM_KEYBOARD, HID_STR_INDEX, false,
                       sizeof(s_hid_report_descriptor), HID_EP_IN, 16, 10),
    TUD_HID_INOUT_DESCRIPTOR(HID_ITF_NUM_CTAPHID, CTAPHID_STR_INDEX,
                             HID_ITF_PROTOCOL_NONE, sizeof(s_ctaphid_report_descriptor),
                             CTAPHID_EP_OUT, CTAPHID_EP_IN, CTAPHID_EP_SIZE, 5),
    TUD_CDC_NCM_DESCRIPTOR(2, NCM_STR_INDEX, NCM_MAC_STR_INDEX,
                           0x84, 64, 0x02, 0x82, 64, CFG_TUD_NET_MTU),
};

static const uint8_t s_usb_configuration_descriptor_hid_jade_fs[] = {
    TUD_CONFIG_DESCRIPTOR(1, 4, 0,
                          TUD_CONFIG_DESC_LEN + TUD_HID_DESC_LEN +
                          TUD_HID_INOUT_DESC_LEN + TUD_CDC_DESC_LEN,
                          TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    TUD_HID_DESCRIPTOR(HID_ITF_NUM_KEYBOARD, HID_STR_INDEX, false,
                       sizeof(s_hid_report_descriptor), HID_EP_IN, 16, 10),
    TUD_HID_INOUT_DESCRIPTOR(HID_ITF_NUM_CTAPHID, CTAPHID_STR_INDEX,
                             HID_ITF_PROTOCOL_NONE, sizeof(s_ctaphid_report_descriptor),
                             CTAPHID_EP_OUT, CTAPHID_EP_IN, CTAPHID_EP_SIZE, 5),
    TUD_CDC_DESCRIPTOR(2, AUX_INTERFACE_STR_INDEX, 0x85, 8, 0x04, 0x84, 64),
};

#if (TUD_OPT_HIGH_SPEED)
static const uint8_t s_usb_configuration_descriptor_hid_ncm_hs[] = {
    TUD_CONFIG_DESCRIPTOR(1, 4, 0,
                          TUD_CONFIG_DESC_LEN + TUD_HID_DESC_LEN +
                          TUD_HID_INOUT_DESC_LEN + TUD_CDC_NCM_DESC_LEN,
                          TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    TUD_HID_DESCRIPTOR(HID_ITF_NUM_KEYBOARD, HID_STR_INDEX, false,
                       sizeof(s_hid_report_descriptor), HID_EP_IN, 16, 10),
    TUD_HID_INOUT_DESCRIPTOR(HID_ITF_NUM_CTAPHID, CTAPHID_STR_INDEX,
                             HID_ITF_PROTOCOL_NONE, sizeof(s_ctaphid_report_descriptor),
                             CTAPHID_EP_OUT, CTAPHID_EP_IN, CTAPHID_EP_SIZE, 5),
    TUD_CDC_NCM_DESCRIPTOR(2, NCM_STR_INDEX, NCM_MAC_STR_INDEX,
                           0x84, 64, 0x02, 0x82, 512, CFG_TUD_NET_MTU),
};

static const uint8_t s_usb_configuration_descriptor_hid_jade_hs[] = {
    TUD_CONFIG_DESCRIPTOR(1, 4, 0,
                          TUD_CONFIG_DESC_LEN + TUD_HID_DESC_LEN +
                          TUD_HID_INOUT_DESC_LEN + TUD_CDC_DESC_LEN,
                          TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    TUD_HID_DESCRIPTOR(HID_ITF_NUM_KEYBOARD, HID_STR_INDEX, false,
                       sizeof(s_hid_report_descriptor), HID_EP_IN, 16, 10),
    TUD_HID_INOUT_DESCRIPTOR(HID_ITF_NUM_CTAPHID, CTAPHID_STR_INDEX,
                             HID_ITF_PROTOCOL_NONE, sizeof(s_ctaphid_report_descriptor),
                             CTAPHID_EP_OUT, CTAPHID_EP_IN, CTAPHID_EP_SIZE, 5),
    TUD_CDC_DESCRIPTOR(2, AUX_INTERFACE_STR_INDEX, 0x85, 8, 0x04, 0x84, 512),
};
#endif

static const uint8_t s_usb_configuration_descriptor_ccid_fs[] = {
    TUD_CONFIG_DESCRIPTOR(1, 3, 0, TUD_CONFIG_DESC_LEN + TUD_HID_DESC_LEN + TUD_HID_INOUT_DESC_LEN + 84, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    TUD_HID_DESCRIPTOR(HID_ITF_NUM_KEYBOARD, HID_STR_INDEX, false, sizeof(s_hid_report_descriptor), HID_EP_IN, 16, 10),
    TUD_HID_INOUT_DESCRIPTOR(HID_ITF_NUM_CTAPHID, CTAPHID_STR_INDEX, HID_ITF_PROTOCOL_NONE,
                             sizeof(s_ctaphid_report_descriptor), CTAPHID_EP_OUT, CTAPHID_EP_IN,
                             CTAPHID_EP_SIZE, 5),
    9, TUSB_DESC_INTERFACE, 2, 0, 3, TUSB_CLASS_SMART_CARD, 0, 0, AUX_INTERFACE_STR_INDEX,
    0x36, 0x21, 0x10, 0x01, 0x00, 0x07, 0x02, 0x00, 0x00, 0x00,
    0xA0, 0x0F, 0x00, 0x00,
    0xA0, 0x0F, 0x00, 0x00,
    0x00,
    0x80, 0x25, 0x00, 0x00,
    0x80, 0x25, 0x00, 0x00,
    0x00, 0x00, 0x04, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x04, 0x04, 0x00, 0x0A, 0x04, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
    7, TUSB_DESC_ENDPOINT, CCID_EP_OUT, TUSB_XFER_BULK, CCID_U16_LSB(CCID_EP_SIZE_HS), CCID_U16_MSB(CCID_EP_SIZE_HS), 0,
    7, TUSB_DESC_ENDPOINT, CCID_EP_IN, TUSB_XFER_BULK, CCID_U16_LSB(CCID_EP_SIZE_HS), CCID_U16_MSB(CCID_EP_SIZE_HS), 0,
    7, TUSB_DESC_ENDPOINT, CCID_EP_INT_IN, TUSB_XFER_INTERRUPT, CCID_U16_LSB(CCID_EP_INT_SIZE), CCID_U16_MSB(CCID_EP_INT_SIZE), 255,
};

#if (TUD_OPT_HIGH_SPEED)
static const uint8_t s_usb_configuration_descriptor_ccid_hs[] = {
    TUD_CONFIG_DESCRIPTOR(1, 3, 0, TUD_CONFIG_DESC_LEN + TUD_HID_DESC_LEN + TUD_HID_INOUT_DESC_LEN + 84, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    TUD_HID_DESCRIPTOR(HID_ITF_NUM_KEYBOARD, HID_STR_INDEX, false, sizeof(s_hid_report_descriptor), HID_EP_IN, 16, 10),
    TUD_HID_INOUT_DESCRIPTOR(HID_ITF_NUM_CTAPHID, CTAPHID_STR_INDEX, HID_ITF_PROTOCOL_NONE,
                             sizeof(s_ctaphid_report_descriptor), CTAPHID_EP_OUT, CTAPHID_EP_IN,
                             CTAPHID_EP_SIZE, 5),
    9, TUSB_DESC_INTERFACE, 2, 0, 3, TUSB_CLASS_SMART_CARD, 0, 0, AUX_INTERFACE_STR_INDEX,
    0x36, 0x21, 0x10, 0x01, 0x00, 0x07, 0x02, 0x00, 0x00, 0x00,
    0xA0, 0x0F, 0x00, 0x00,
    0xA0, 0x0F, 0x00, 0x00,
    0x00,
    0x80, 0x25, 0x00, 0x00,
    0x80, 0x25, 0x00, 0x00,
    0x00, 0x00, 0x04, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x04, 0x04, 0x00, 0x0A, 0x04, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
    7, TUSB_DESC_ENDPOINT, CCID_EP_OUT, TUSB_XFER_BULK, CCID_U16_LSB(CCID_EP_SIZE_HS), CCID_U16_MSB(CCID_EP_SIZE_HS), 0,
    7, TUSB_DESC_ENDPOINT, CCID_EP_IN, TUSB_XFER_BULK, CCID_U16_LSB(CCID_EP_SIZE_HS), CCID_U16_MSB(CCID_EP_SIZE_HS), 0,
    7, TUSB_DESC_ENDPOINT, CCID_EP_INT_IN, TUSB_XFER_INTERRUPT, CCID_U16_LSB(CCID_EP_INT_SIZE), CCID_U16_MSB(CCID_EP_INT_SIZE), 16,
};
#endif

#if USB_MSC_DEFAULT_ENABLED
static const uint8_t s_usb_configuration_descriptor_msc_fs[] = {
    TUD_CONFIG_DESCRIPTOR(1, 3, 0, TUD_CONFIG_DESC_LEN + TUD_HID_DESC_LEN + TUD_HID_INOUT_DESC_LEN + TUD_MSC_DESC_LEN, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    TUD_HID_DESCRIPTOR(HID_ITF_NUM_KEYBOARD, HID_STR_INDEX, false, sizeof(s_hid_report_descriptor), HID_EP_IN, 16, 10),
    TUD_HID_INOUT_DESCRIPTOR(HID_ITF_NUM_CTAPHID, CTAPHID_STR_INDEX, HID_ITF_PROTOCOL_NONE,
                             sizeof(s_ctaphid_report_descriptor), CTAPHID_EP_OUT, CTAPHID_EP_IN,
                             CTAPHID_EP_SIZE, 5),
    TUD_MSC_DESCRIPTOR(MSC_ITF_NUM_NO_CCID, MSC_STR_INDEX, MSC_EP_OUT, MSC_EP_IN, MSC_EP_SIZE_FS),
};

#if (TUD_OPT_HIGH_SPEED)
static const uint8_t s_usb_configuration_descriptor_msc_hs[] = {
    TUD_CONFIG_DESCRIPTOR(1, 3, 0, TUD_CONFIG_DESC_LEN + TUD_HID_DESC_LEN + TUD_HID_INOUT_DESC_LEN + TUD_MSC_DESC_LEN, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    TUD_HID_DESCRIPTOR(HID_ITF_NUM_KEYBOARD, HID_STR_INDEX, false, sizeof(s_hid_report_descriptor), HID_EP_IN, 16, 10),
    TUD_HID_INOUT_DESCRIPTOR(HID_ITF_NUM_CTAPHID, CTAPHID_STR_INDEX, HID_ITF_PROTOCOL_NONE,
                             sizeof(s_ctaphid_report_descriptor), CTAPHID_EP_OUT, CTAPHID_EP_IN,
                             CTAPHID_EP_SIZE, 5),
    TUD_MSC_DESCRIPTOR(MSC_ITF_NUM_NO_CCID, MSC_STR_INDEX, MSC_EP_OUT, MSC_EP_IN, MSC_EP_SIZE_HS),
};
#endif
#endif

static uint32_t usb_desc_u32_le(const uint8_t *p)
{
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static void usb_log_ccid_functional_descriptor(void)
{
    const uint8_t *desc = s_usb_configuration_descriptor_ccid_fs;
    size_t total = sizeof(s_usb_configuration_descriptor_ccid_fs);

    for (size_t offset = 0; offset + 2 <= total;) {
        uint8_t length = desc[offset];
        uint8_t type = desc[offset + 1];
        if (length == 0 || offset + length > total) {
            break;
        }
        if (length == 0x36 && type == 0x21) {
            /* CCID functional descriptor offsets from bLength. */
            uint32_t protocols = usb_desc_u32_le(desc + offset + 6);
            uint32_t default_clock = usb_desc_u32_le(desc + offset + 10);
            uint32_t maximum_clock = usb_desc_u32_le(desc + offset + 14);
            uint32_t data_rate = usb_desc_u32_le(desc + offset + 19);
            uint32_t max_data_rate = usb_desc_u32_le(desc + offset + 23);
            uint32_t features = usb_desc_u32_le(desc + offset + 40);
            uint32_t max_message = usb_desc_u32_le(desc + offset + 44);
            ESP_LOGI(TAG,
                     "CCID descriptor offset=%u len=%u protocols=0x%08lx "
                     "clock=%lu/%lu data_rate=%lu/%lu features=0x%08lx "
                     "max_message=%lu",
                     (unsigned)offset,
                     (unsigned)length,
                     (unsigned long)protocols,
                     (unsigned long)default_clock,
                     (unsigned long)maximum_clock,
                     (unsigned long)data_rate,
                     (unsigned long)max_data_rate,
                     (unsigned long)features,
                     (unsigned long)max_message);
            ESP_LOG_BUFFER_HEX_LEVEL(TAG, desc + offset, length, ESP_LOG_INFO);
            return;
        }
        offset += length;
    }

    ESP_LOGE(TAG, "CCID functional descriptor not found");
}

static bool ascii_to_hid(char c, uint8_t *modifier, uint8_t *keycode)
{
    return keyboard_layout_char(c, modifier, keycode);
}

static bool wait_for_keyboard_ready(uint32_t timeout_ms)
{
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);

    while (tud_mounted() && !tud_hid_n_ready(HID_ITF_NUM_KEYBOARD)) {
        if (xTaskGetTickCount() >= deadline) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    return tud_mounted();
}

static bool send_keyboard_report(uint8_t modifier, const uint8_t keycode[6])
{
    if (!wait_for_keyboard_ready(HID_REPORT_TIMEOUT_MS)) {
        return false;
    }

    return tud_hid_n_keyboard_report(HID_ITF_NUM_KEYBOARD, HID_ITF_PROTOCOL_KEYBOARD, modifier, keycode);
}

static bool send_key(char c)
{
    uint8_t modifier = 0;
    uint8_t keycode[6] = {0};
    uint8_t release_keycode[6] = {0};

    if (!ascii_to_hid(c, &modifier, &keycode[0])) {
        ESP_LOGW(TAG, "Unsupported HID char: 0x%02x", (unsigned char)c);
        return false;
    }

    if (!send_keyboard_report(modifier, keycode)) {
        ESP_LOGW(TAG, "Failed to send HID key press for char 0x%02x", (unsigned char)c);
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(HID_REPORT_INTERVAL_MS));

    if (!send_keyboard_report(0, release_keycode)) {
        ESP_LOGW(TAG, "Failed to send HID key release for char 0x%02x", (unsigned char)c);
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(HID_REPORT_INTERVAL_MS));
    return true;
}

static bool wait_for_mount(void)
{
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(HID_MOUNT_TIMEOUT_MS);

    while (!tud_mounted()) {
        if (xTaskGetTickCount() >= deadline) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(25));
    }

    return true;
}

static void tinyusb_event_handler(tinyusb_event_t *event, void *arg)
{
    (void)arg;

    switch (event->id) {
    case TINYUSB_EVENT_ATTACHED:
        ESP_LOGI(TAG, "USB host attached on rhport=%u mounted=%u hid0_ready=%u ctaphid_ready=%u",
                 event->rhport,
                 tud_mounted() ? 1 : 0,
                 tud_hid_n_ready(HID_ITF_NUM_KEYBOARD) ? 1 : 0,
                 tud_hid_n_ready(HID_ITF_NUM_CTAPHID) ? 1 : 0);
        if (s_smartcard_reader_enabled) {
            (void)usb_ccid_start();
        }
        break;
    case TINYUSB_EVENT_DETACHED:
        ESP_LOGW(TAG, "USB host detached on rhport=%u mounted=%u", event->rhport, tud_mounted() ? 1 : 0);
        break;
    default:
        ESP_LOGI(TAG, "USB event id=%d on rhport=%u mounted=%u hid0_ready=%u ctaphid_ready=%u",
                 event->id,
                 event->rhport,
                 tud_mounted() ? 1 : 0,
                 tud_hid_n_ready(HID_ITF_NUM_KEYBOARD) ? 1 : 0,
                 tud_hid_n_ready(HID_ITF_NUM_CTAPHID) ? 1 : 0);
        break;
    }
}

static void hid_task(void *arg)
{
    (void)arg;
    hid_text_msg_t msg = {0};

    while (true) {
        if (xQueueReceive(s_hid_queue, &msg, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        if (msg.type == HID_MESSAGE_SYNC) {
            xTaskNotifyGive(msg.waiter);
            continue;
        }

        if (!wait_for_mount()) {
            ESP_LOGW(TAG, "USB host not mounted; dropped HID text");
            continue;
        }

        if (msg.type == HID_MESSAGE_KEY_PRESS) {
            uint8_t keycode[6] = {msg.key};
            if (!send_keyboard_report(msg.modifier, keycode)) {
                ESP_LOGW(TAG, "Failed to send HID key press: 0x%02x", msg.key);
            }
            vTaskDelay(pdMS_TO_TICKS(HID_REPORT_INTERVAL_MS));
        } else if (msg.type == HID_MESSAGE_KEY_RELEASE) {
            uint8_t keycode[6] = {0};
            if (!send_keyboard_report(0, keycode)) {
                ESP_LOGW(TAG, "Failed to send HID key release");
            }
            vTaskDelay(pdMS_TO_TICKS(HID_REPORT_INTERVAL_MS));
        } else if (msg.type == HID_MESSAGE_DELAY) {
            vTaskDelay(pdMS_TO_TICKS(msg.delay_ms));
        } else {
            for (size_t i = 0; msg.text[i] != '\0'; i++) {
                send_key(msg.text[i]);
            }
        }
    }
}

static void msc_event_handler(tinyusb_msc_storage_handle_t handle, tinyusb_msc_event_t *event, void *arg)
{
    (void)handle;
    (void)arg;

    const char *mount_point = event->mount_point == TINYUSB_MSC_STORAGE_MOUNT_USB ? "USB" : "APP";

    switch (event->id) {
    case TINYUSB_MSC_EVENT_MOUNT_START:
        ESP_LOGI(TAG, "MSC storage mount start: %s", mount_point);
        break;
    case TINYUSB_MSC_EVENT_MOUNT_COMPLETE:
        ESP_LOGI(TAG, "MSC storage mount complete: %s", mount_point);
        if (s_msc_event_group) {
            xEventGroupSetBits(s_msc_event_group,
                               event->mount_point == TINYUSB_MSC_STORAGE_MOUNT_APP ? MSC_MOUNT_APP_DONE_BIT : MSC_MOUNT_USB_DONE_BIT);
        }
        break;
    case TINYUSB_MSC_EVENT_MOUNT_FAILED:
        ESP_LOGE(TAG, "MSC storage mount failed: %s", mount_point);
        if (s_msc_event_group) {
            xEventGroupSetBits(s_msc_event_group, MSC_MOUNT_FAILED_BIT);
        }
        break;
    case TINYUSB_MSC_EVENT_FORMAT_REQUIRED:
        ESP_LOGW(TAG, "MSC storage format required");
        if (s_msc_event_group) {
            xEventGroupSetBits(s_msc_event_group, MSC_MOUNT_FAILED_BIT);
        }
        break;
    case TINYUSB_MSC_EVENT_FORMAT_FAILED:
        ESP_LOGE(TAG, "MSC storage format failed");
        if (s_msc_event_group) {
            xEventGroupSetBits(s_msc_event_group, MSC_MOUNT_FAILED_BIT);
        }
        break;
    default:
        ESP_LOGI(TAG, "MSC storage event: %d", event->id);
        break;
    }
}

static esp_err_t ensure_nvs_ready(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "Failed to erase NVS");
        ret = nvs_flash_init();
    }

    return ret;
}

uint16_t usb_hid_get_vid(void)
{
    uint16_t vid = USB_VID_DEFAULT;
    if (ensure_nvs_ready() != ESP_OK) {
        return vid;
    }

    nvs_handle_t nvs;
    if (nvs_open(CONNECT_SETTINGS_NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        return vid;
    }

    esp_err_t ret = nvs_get_u16(nvs, CONNECT_USB_VID_NVS_KEY, &vid);
    nvs_close(nvs);
    if (ret != ESP_OK || vid < USB_VID_MIN || vid > USB_VID_MAX) {
        return USB_VID_DEFAULT;
    }
    return vid;
}

esp_err_t usb_hid_set_vid(uint16_t vid)
{
    if (vid < USB_VID_MIN || vid > USB_VID_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_RETURN_ON_ERROR(ensure_nvs_ready(), TAG, "Failed to init NVS");

    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(CONNECT_SETTINGS_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    ESP_RETURN_ON_ERROR(ret, TAG, "Failed to open USB VID NVS");

    ret = nvs_set_u16(nvs, CONNECT_USB_VID_NVS_KEY, vid);
    if (ret == ESP_OK) {
        ret = nvs_commit(nvs);
    }
    nvs_close(nvs);

    if (ret == ESP_OK) {
        s_usb_active_device_descriptor.idVendor = vid;
    }
    return ret;
}

static esp_err_t erase_and_format_usbstore(void)
{
    log_storage_pause();
    esp_err_t ret = tinyusb_msc_set_storage_mount_point(s_msc_storage, TINYUSB_MSC_STORAGE_MOUNT_APP);
    if (ret != ESP_OK) {
        return ret;
    }

    vTaskDelay(pdMS_TO_TICKS(50));

    size_t wl_bytes = wl_size(s_msc_wl_handle);
    size_t wl_sector_bytes = wl_sector_size(s_msc_wl_handle);
    if (wl_bytes == 0 || wl_sector_bytes == 0 || (wl_bytes % wl_sector_bytes) != 0) {
        ESP_LOGW(TAG, "Invalid usbstore WL geometry: size=%u sector=%u",
                 (unsigned int)wl_bytes,
                 (unsigned int)wl_sector_bytes);
        return ESP_ERR_INVALID_SIZE;
    }

    const size_t erase_chunk_bytes = wl_sector_bytes * MSC_ERASE_CHUNK_SECTORS;
    size_t next_log_bytes = MSC_ERASE_LOG_BYTES;
    ESP_LOGI(TAG, "Erase usbstore in chunks: total=%u sector=%u chunk=%u",
             (unsigned int)wl_bytes,
             (unsigned int)wl_sector_bytes,
             (unsigned int)erase_chunk_bytes);

    for (size_t offset = 0; offset < wl_bytes;) {
        size_t remaining = wl_bytes - offset;
        size_t current_chunk_bytes = remaining < erase_chunk_bytes ? remaining : erase_chunk_bytes;
        ret = wl_erase_range(s_msc_wl_handle, offset, current_chunk_bytes);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to erase usbstore WL area at offset=%u size=%u: %s",
                     (unsigned int)offset,
                     (unsigned int)current_chunk_bytes,
                     esp_err_to_name(ret));
            return ret;
        }

        offset += current_chunk_bytes;
        if (offset >= next_log_bytes || offset == wl_bytes) {
            ESP_LOGI(TAG, "usbstore erase progress: %u/%u bytes",
                     (unsigned int)offset,
                     (unsigned int)wl_bytes);
            next_log_bytes += MSC_ERASE_LOG_BYTES;
        }

        vTaskDelay(1);
    }

    s_msc_fat_partition_number = 0;
    ret = tinyusb_msc_format_storage(s_msc_storage);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to format usbstore: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "usbstore erase and format complete");
    return ESP_OK;
}

static esp_err_t format_usbstore_filesystem(void)
{
    log_storage_pause();
    esp_err_t ret = tinyusb_msc_set_storage_mount_point(s_msc_storage, TINYUSB_MSC_STORAGE_MOUNT_APP);
    if (ret != ESP_OK) {
        log_storage_resume();
        return ret;
    }

    vTaskDelay(pdMS_TO_TICKS(50));
    s_msc_fat_partition_number = 0;
    ret = tinyusb_msc_format_storage(s_msc_storage);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to format usbstore filesystem: %s", esp_err_to_name(ret));
        log_storage_resume();
        return ret;
    }

    log_storage_resume();
    ESP_LOGI(TAG, "usbstore filesystem format complete");
    return ESP_OK;
}

esp_err_t usb_msc_factory_format(void)
{
    if (!s_msc_storage || s_msc_wl_handle == WL_INVALID_HANDLE) {
        return ESP_ERR_INVALID_STATE;
    }
    return erase_and_format_usbstore();
}

esp_err_t usb_msc_mark_storage_initialized(void)
{
    ESP_RETURN_ON_ERROR(ensure_nvs_ready(), TAG, "Failed to init NVS");

    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(MSC_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    ESP_RETURN_ON_ERROR(ret, TAG, "Failed to open usbstore NVS");
    ret = nvs_set_u8(nvs, MSC_NVS_FORMAT_KEY, 1);
    if (ret == ESP_OK) {
        ret = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return ret;
}

static void load_hid_keyboard_layout(void)
{
    if (ensure_nvs_ready() != ESP_OK) {
        return;
    }

    nvs_handle_t nvs;
    if (nvs_open(CONNECT_SETTINGS_NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        return;
    }

    uint8_t selected = HID_LAYOUT_US;
    if (nvs_get_u8(nvs, CONNECT_HID_LAYOUT_NVS_KEY, &selected) == ESP_OK &&
        selected < HID_LAYOUT_COUNT) {
        keyboard_layout_set((hid_keyboard_layout_t)selected);
    }
    nvs_close(nvs);
}

static esp_err_t format_usbstore_on_first_boot(void)
{
    ESP_RETURN_ON_ERROR(ensure_nvs_ready(), TAG, "Failed to init NVS");

    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(MSC_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    ESP_RETURN_ON_ERROR(ret, TAG, "Failed to open usbstore NVS");

    uint8_t format_done = 0;
    ret = nvs_get_u8(nvs, MSC_NVS_FORMAT_KEY, &format_done);
    if (ret == ESP_OK && format_done == 1) {
        nvs_close(nvs);
        ESP_LOGI(TAG, "usbstore first-boot format already done: %s=1", MSC_NVS_FORMAT_KEY);
        return ESP_OK;
    }

    if (ret != ESP_OK && ret != ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(nvs);
        ESP_LOGW(TAG, "Failed to read usbstore format flag: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGW(TAG, "First boot: format usbstore filesystem");
    ret = format_usbstore_filesystem();
    if (ret != ESP_OK) {
        nvs_close(nvs);
        return ret;
    }

    ret = nvs_set_u8(nvs, MSC_NVS_FORMAT_KEY, 1);
    if (ret == ESP_OK) {
        ret = nvs_commit(nvs);
    }
    nvs_close(nvs);
    ESP_RETURN_ON_ERROR(ret, TAG, "Failed to save usbstore format flag");

    ESP_LOGI(TAG, "usbstore first-boot format complete");
    return ESP_OK;
}

esp_err_t usb_msc_storage_init(void)
{
    if (s_msc_storage) {
        return ESP_OK;
    }

    if (!s_msc_event_group) {
        s_msc_event_group = xEventGroupCreate();
        ESP_RETURN_ON_FALSE(s_msc_event_group, ESP_ERR_NO_MEM, TAG, "Failed to create MSC event group");
    }

    const esp_partition_t *storage_partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                                                        ESP_PARTITION_SUBTYPE_DATA_FAT,
                                                                        MSC_PARTITION_LABEL);
    ESP_RETURN_ON_FALSE(storage_partition, ESP_ERR_NOT_FOUND, TAG, "FAT partition '%s' not found", MSC_PARTITION_LABEL);
    ESP_LOGI(TAG, "MSC flash partition '%s': offset=0x%lx size=%lu bytes",
             storage_partition->label,
             (unsigned long)storage_partition->address,
             (unsigned long)storage_partition->size);

    ESP_RETURN_ON_ERROR(wl_mount(storage_partition, &s_msc_wl_handle), TAG, "Failed to mount wear levelling");

    const tinyusb_msc_driver_config_t driver_cfg = {
        .user_flags = {
            .auto_mount_off = 1,
        },
        .callback = msc_event_handler,
        .callback_arg = NULL,
    };
    esp_err_t ret = tinyusb_msc_install_driver(&driver_cfg);
    if (ret != ESP_OK) {
        wl_unmount(s_msc_wl_handle);
        s_msc_wl_handle = WL_INVALID_HANDLE;
        return ret;
    }

    tinyusb_msc_storage_config_t storage_cfg = {
        .medium.wl_handle = s_msc_wl_handle,
        .mount_point = TINYUSB_MSC_STORAGE_MOUNT_APP,
        .fat_fs = {
            .base_path = MSC_MOUNT_BASE_PATH,
            .config = {
                .format_if_mount_failed = false,
                .max_files = 5,
                .allocation_unit_size = 4096,
            },
            .do_not_format = true,
            .format_flags = 0,
        },
    };

    ret = tinyusb_msc_new_storage_spiflash(&storage_cfg, &s_msc_storage);
    if (ret != ESP_OK) {
        (void)tinyusb_msc_uninstall_driver();
        if (s_msc_wl_handle != WL_INVALID_HANDLE) {
            wl_unmount(s_msc_wl_handle);
            s_msc_wl_handle = WL_INVALID_HANDLE;
        }
        return ret;
    }
    ESP_RETURN_ON_ERROR(format_usbstore_on_first_boot(), TAG, "Failed first-boot usbstore format");

    ESP_LOGI(TAG, "MSC flash storage mounted to APP; use switch_usbstore to expose USB disk");
    return ESP_OK;
}

static void log_file_preview(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGW(TAG, "Failed to open file '%s': errno=%d", path, errno);
        return;
    }

    uint8_t buffer[MSC_DUMP_CHUNK_SIZE];
    size_t total = 0;

    while (total < MSC_FILE_READ_MAX_BYTES) {
        size_t to_read = MSC_FILE_READ_MAX_BYTES - total;
        if (to_read > sizeof(buffer)) {
            to_read = sizeof(buffer);
        }

        size_t read_len = fread(buffer, 1, to_read, f);
        if (read_len == 0) {
            break;
        }

        ESP_LOGI(TAG, "file data '%s' offset=%u len=%u",
                 path,
                 (unsigned int)total,
                 (unsigned int)read_len);
        ESP_LOG_BUFFER_HEXDUMP(TAG, buffer, read_len, ESP_LOG_INFO);
        total += read_len;
    }

    if (!feof(f)) {
        ESP_LOGI(TAG, "file preview truncated at %u bytes: %s",
                 (unsigned int)MSC_FILE_READ_MAX_BYTES,
                 path);
    }

    fclose(f);
}

static void list_storage_dir(const char *path, int depth)
{
    DIR *dir = opendir(path);
    if (!dir) {
        ESP_LOGW(TAG, "Failed to open dir '%s': errno=%d", path, errno);
        return;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        char child_path[MSC_PATH_MAX_LEN];
        int written = snprintf(child_path, sizeof(child_path), "%s/%s", path, entry->d_name);
        if (written < 0 || written >= (int)sizeof(child_path)) {
            ESP_LOGW(TAG, "Path too long under '%s': %s", path, entry->d_name);
            continue;
        }

        struct stat st;
        if (stat(child_path, &st) != 0) {
            ESP_LOGW(TAG, "Failed to stat '%s': errno=%d", child_path, errno);
            continue;
        }

        if (S_ISDIR(st.st_mode)) {
            ESP_LOGI(TAG, "%*s[DIR]  %s", depth * 2, "", child_path);
            list_storage_dir(child_path, depth + 1);
        } else {
            ESP_LOGI(TAG, "%*s[FILE] %s size=%ld", depth * 2, "", child_path, (long)st.st_size);
            log_file_preview(child_path);
        }
    }

    closedir(dir);
}

static esp_err_t set_storage_mount_point_wait(tinyusb_msc_mount_point_t mount_point)
{
    tinyusb_msc_mount_point_t current_mount_point;
    esp_err_t ret = tinyusb_msc_get_storage_mount_point(s_msc_storage, &current_mount_point);
    if (ret != ESP_OK) {
        return ret;
    }

    if (current_mount_point == mount_point) {
        if (mount_point == TINYUSB_MSC_STORAGE_MOUNT_APP) {
            log_storage_resume();
        }
        return ESP_OK;
    }

    log_storage_pause();
    const EventBits_t done_bit = mount_point == TINYUSB_MSC_STORAGE_MOUNT_APP ? MSC_MOUNT_APP_DONE_BIT : MSC_MOUNT_USB_DONE_BIT;
    xEventGroupClearBits(s_msc_event_group, MSC_MOUNT_APP_DONE_BIT | MSC_MOUNT_USB_DONE_BIT | MSC_MOUNT_FAILED_BIT);

    ret = tinyusb_msc_set_storage_mount_point(s_msc_storage, mount_point);
    if (ret != ESP_OK) {
        return ret;
    }

    EventBits_t bits = xEventGroupWaitBits(s_msc_event_group,
                                           done_bit | MSC_MOUNT_FAILED_BIT,
                                           pdTRUE,
                                           pdFALSE,
                                           pdMS_TO_TICKS(MSC_MOUNT_TIMEOUT_MS));
    if ((bits & done_bit) != 0) {
        if (mount_point == TINYUSB_MSC_STORAGE_MOUNT_APP) {
            log_storage_resume();
        }
        return ESP_OK;
    }

    if ((bits & MSC_MOUNT_FAILED_BIT) != 0) {
        return ESP_FAIL;
    }

    return ESP_ERR_TIMEOUT;
}

static void storage_dump_task(void *arg)
{
    (void)arg;

    if (!s_msc_storage) {
        ESP_LOGW(TAG, "MSC storage is not initialized");
        goto done;
    }

    ESP_LOGI(TAG, "Mount usbstore to APP and list files at %s", MSC_MOUNT_BASE_PATH);
    esp_err_t ret = set_storage_mount_point_wait(TINYUSB_MSC_STORAGE_MOUNT_APP);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to mount usbstore to APP before listing files: %s", esp_err_to_name(ret));
        esp_err_t restore_ret = tinyusb_msc_set_storage_mount_point(s_msc_storage, TINYUSB_MSC_STORAGE_MOUNT_APP);
        if (restore_ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to keep usbstore on APP side: %s", esp_err_to_name(restore_ret));
        }
        goto done;
    }

    list_storage_dir(MSC_MOUNT_BASE_PATH, 0);
    ESP_LOGI(TAG, "usbstore remains mounted to APP after dump");

done:
    s_dump_in_progress = false;
    vTaskDelete(NULL);
}

static esp_err_t install_tinyusb_driver(usb_mode_t mode)
{
#if CONFIG_IDF_TARGET_ESP32P4 && USB_USE_ESP32P4_HIGH_SPEED
    // Board PIN49=USB1_N and PIN50=USB1_P are wired to the ESP32-P4 high-speed USB PHY.
    tinyusb_config_t tusb_cfg = TINYUSB_CONFIG_HIGH_SPEED(tinyusb_event_handler, NULL);
#else
    tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG(tinyusb_event_handler);
#endif
    const tusb_desc_device_t *device_descriptor_template;
    switch (mode) {
    case USB_MODE_HID_JADE_CDC:
        device_descriptor_template = &s_usb_jade_device_descriptor;
        tusb_cfg.descriptor.full_speed_config = s_usb_configuration_descriptor_hid_jade_fs;
        break;
    case USB_MODE_HID_NCM:
        device_descriptor_template = &s_usb_ncm_device_descriptor;
        tusb_cfg.descriptor.full_speed_config = s_usb_configuration_descriptor_hid_ncm_fs;
        break;
    case USB_MODE_HID_CCID:
        device_descriptor_template = &s_usb_ccid_device_descriptor;
        tusb_cfg.descriptor.full_speed_config = s_usb_configuration_descriptor_ccid_fs;
        break;
    case USB_MODE_HID_MSC:
#if USB_MSC_DEFAULT_ENABLED
        device_descriptor_template = &s_usb_msc_device_descriptor;
        tusb_cfg.descriptor.full_speed_config = s_usb_configuration_descriptor_msc_fs;
        break;
#else
        return ESP_ERR_NOT_SUPPORTED;
#endif
    case USB_MODE_HID_ONLY:
    default:
        device_descriptor_template = &s_usb_hid_device_descriptor;
        tusb_cfg.descriptor.full_speed_config = s_usb_configuration_descriptor_hid_fs;
        break;
    }
    s_usb_active_device_descriptor = *device_descriptor_template;
    s_usb_active_device_descriptor.idVendor = usb_hid_get_vid();
    tusb_cfg.descriptor.device = &s_usb_active_device_descriptor;
    tusb_cfg.descriptor.string = s_hid_string_descriptor;
    tusb_cfg.descriptor.string_count = sizeof(s_hid_string_descriptor) / sizeof(s_hid_string_descriptor[0]);
#if (TUD_OPT_HIGH_SPEED)
    tusb_cfg.descriptor.qualifier = mode == USB_MODE_HID_NCM
                                        ? &s_usb_ncm_device_qualifier
                                        : &s_usb_device_qualifier;
    switch (mode) {
    case USB_MODE_HID_JADE_CDC:
        tusb_cfg.descriptor.high_speed_config = s_usb_configuration_descriptor_hid_jade_hs;
        break;
    case USB_MODE_HID_NCM:
        tusb_cfg.descriptor.high_speed_config = s_usb_configuration_descriptor_hid_ncm_hs;
        break;
    case USB_MODE_HID_CCID:
        tusb_cfg.descriptor.high_speed_config = s_usb_configuration_descriptor_ccid_hs;
        break;
    case USB_MODE_HID_MSC:
#if USB_MSC_DEFAULT_ENABLED
        tusb_cfg.descriptor.high_speed_config = s_usb_configuration_descriptor_msc_hs;
        break;
#else
        return ESP_ERR_NOT_SUPPORTED;
#endif
    case USB_MODE_HID_ONLY:
    default:
        tusb_cfg.descriptor.high_speed_config = s_usb_configuration_descriptor_hid_hs;
        break;
    }
#endif

    ESP_RETURN_ON_ERROR(tinyusb_driver_install(&tusb_cfg), TAG, "TinyUSB init failed");
    if (mode == USB_MODE_HID_CCID) {
        usb_log_ccid_functional_descriptor();
    }
    canokey_esp32p4_ctaphid_usb_init();
    ESP_LOGI(TAG, "TinyUSB driver installed: mode=%d VID=%04X PID=%04X mounted=%u hid0_ready=%u ctaphid_ready=%u",
             (int)mode,
             s_usb_active_device_descriptor.idVendor,
             s_usb_active_device_descriptor.idProduct,
             tud_mounted() ? 1 : 0,
             tud_hid_n_ready(HID_ITF_NUM_KEYBOARD) ? 1 : 0,
             tud_hid_n_ready(HID_ITF_NUM_CTAPHID) ? 1 : 0);
    s_usb_mode = mode;
    return ESP_OK;
}

static esp_err_t reinstall_tinyusb_driver(usb_mode_t mode)
{
    if (s_usb_mode == mode && tusb_inited()) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Switching TinyUSB mode %d -> %d", (int)s_usb_mode, (int)mode);
    if (tusb_inited()) {
        (void)tud_disconnect();
        vTaskDelay(pdMS_TO_TICKS(USB_DISCONNECT_DELAY_MS));
        esp_err_t ret = tinyusb_driver_uninstall();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "TinyUSB uninstall failed: %s", esp_err_to_name(ret));
            return ret;
        }
        vTaskDelay(pdMS_TO_TICKS(USB_REINSTALL_DELAY_MS));
    }

    esp_err_t ret = install_tinyusb_driver(mode);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "TinyUSB mode %d install failed: %s", (int)mode, esp_err_to_name(ret));
        return ret;
    }
    (void)tud_connect();
    vTaskDelay(pdMS_TO_TICKS(USB_ENUM_SETTLE_MS));
    return ESP_OK;
}

esp_err_t usb_hid_keyboard_init(void)
{
    if (s_hid_started) {
        return ESP_OK;
    }

    load_hid_keyboard_layout();

    if (!s_hid_queue) {
        s_hid_queue = xQueueCreate(HID_QUEUE_DEPTH, sizeof(hid_text_msg_t));
        ESP_RETURN_ON_FALSE(s_hid_queue, ESP_ERR_NO_MEM, TAG, "Failed to create HID queue");
    }
#if USB_MSC_DEFAULT_ENABLED
    ESP_LOGI(TAG, "USB MSC descriptor enabled; storage init is handled separately");
#else
    ESP_LOGI(TAG, "USB MSC disk mode disabled by default");
#endif

    if (!tusb_inited()) {
        ESP_RETURN_ON_ERROR(install_tinyusb_driver(USB_MODE_HID_ONLY), TAG, "TinyUSB init failed");
    }
    ESP_RETURN_ON_FALSE(xTaskCreate(hid_task, "usb_hid", HID_TASK_STACK_SIZE, NULL, HID_TASK_PRIORITY, NULL) == pdPASS,
                        ESP_ERR_NO_MEM, TAG, "Failed to create HID task");

    s_hid_started = true;
    ESP_LOGI(TAG, "USB HID keyboard initialized on %s USB",
#if CONFIG_IDF_TARGET_ESP32P4 && USB_USE_ESP32P4_HIGH_SPEED
             "ESP32-P4 high-speed USB1_N/USB1_P"
#else
             "TinyUSB default"
#endif
    );
    return ESP_OK;
}

esp_err_t usb_hid_enable_smartcard_reader(void)
{
    if (!s_hid_started) {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_ncm_enabled) {
        ESP_RETURN_ON_ERROR(usb_hid_disable_ncm(), TAG, "disable NCM before smartcard failed");
    }
    ESP_RETURN_ON_FALSE(!s_jade_cdc_enabled, ESP_ERR_INVALID_STATE, TAG,
                        "disable Jade protocol before smartcard");

    if (s_usbstore_exposed_to_usb) {
        ESP_RETURN_ON_ERROR(usb_hid_disable_usbstore(),
                            TAG,
                            "disable usbstore before smartcard failed");
    }

    s_smartcard_reader_enabled = true;
    ESP_RETURN_ON_ERROR(reinstall_tinyusb_driver(USB_MODE_HID_CCID), TAG, "TinyUSB smartcard mode failed");
    s_usbstore_exposed_to_usb = false;
    (void)usb_ccid_start();
    ESP_LOGI(TAG, "SmartCard reader enabled");
    return ESP_OK;
}

esp_err_t usb_hid_disable_smartcard_reader(void)
{
    if (!s_hid_started) {
        return ESP_ERR_INVALID_STATE;
    }

    if (!s_smartcard_reader_enabled) {
        return ESP_OK;
    }

    usb_ccid_deactivate_card();
    ESP_RETURN_ON_ERROR(reinstall_tinyusb_driver(USB_MODE_HID_ONLY), TAG, "TinyUSB HID-only mode failed");
    s_smartcard_reader_enabled = false;
    ESP_LOGI(TAG, "SmartCard reader disabled");
    return ESP_OK;
}

esp_err_t usb_hid_enable_usbstore(void)
{
    if (!s_hid_started) {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_ncm_enabled) {
        ESP_RETURN_ON_ERROR(usb_hid_disable_ncm(), TAG, "disable NCM before usbstore failed");
    }
    ESP_RETURN_ON_FALSE(!s_jade_cdc_enabled, ESP_ERR_INVALID_STATE, TAG,
                        "disable Jade protocol before usbstore");

    if (s_smartcard_reader_enabled) {
        usb_ccid_deactivate_card();
        s_smartcard_reader_enabled = false;
    }

    esp_err_t ret = reinstall_tinyusb_driver(USB_MODE_HID_MSC);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = usbstore_set_mount_point(TINYUSB_MSC_STORAGE_MOUNT_USB);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to release usbstore from APP: %s", esp_err_to_name(ret));
        esp_err_t restore_ret = reinstall_tinyusb_driver(USB_MODE_HID_ONLY);
        if (restore_ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to restore HID-only mode: %s",
                     esp_err_to_name(restore_ret));
        }
        return ret;
    }

    s_usbstore_exposed_to_usb = true;
    return ESP_OK;
}

esp_err_t usb_hid_disable_usbstore(void)
{
    if (!s_hid_started) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = reinstall_tinyusb_driver(USB_MODE_HID_ONLY);
    if (ret != ESP_OK) {
        return ret;
    }

    ESP_RETURN_ON_ERROR(usbstore_set_mount_point(TINYUSB_MSC_STORAGE_MOUNT_APP),
                        TAG,
                        "mount usbstore to APP failed");
    s_usbstore_exposed_to_usb = false;
    return ESP_OK;
}

esp_err_t usb_hid_toggle_usbstore(void)
{
    if (!s_hid_started) {
        return ESP_ERR_INVALID_STATE;
    }

    return s_usbstore_exposed_to_usb ? usb_hid_disable_usbstore() : usb_hid_enable_usbstore();
}

bool usb_hid_usbstore_is_exposed_to_usb(void)
{
    return s_usbstore_exposed_to_usb;
}

esp_err_t usb_hid_enable_ncm(void)
{
    if (!s_hid_started) {
        ESP_LOGW(TAG, "USB HID was not started; retrying FIDO initialization before NCM");
        ESP_RETURN_ON_ERROR(usb_hid_keyboard_init(), TAG, "FIDO recovery init failed");
    }
    if (s_ncm_enabled) {
        return ESP_OK;
    }
    ESP_RETURN_ON_FALSE(!s_jade_cdc_enabled, ESP_ERR_INVALID_STATE, TAG,
                        "disable Jade protocol before NCM");
    if (s_usbstore_exposed_to_usb) {
        ESP_RETURN_ON_ERROR(usb_hid_disable_usbstore(),
                            TAG,
                            "disable usbstore before NCM failed");
    }

    ESP_RETURN_ON_ERROR(usb_ncm_init(), TAG, "NCM network init failed");
    esp_err_t ret = reinstall_tinyusb_driver(USB_MODE_HID_NCM);
    if (ret != ESP_OK) {
        usb_ncm_deinit();
        esp_err_t restore_ret = reinstall_tinyusb_driver(USB_MODE_HID_ONLY);
        if (restore_ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to restore FIDO after NCM error: %s",
                     esp_err_to_name(restore_ret));
        }
        return ret;
    }

    s_ncm_enabled = true;
    ESP_LOGI(TAG, "USB FIDO+NCM mode enabled");
    return ESP_OK;
}

esp_err_t usb_hid_disable_ncm(void)
{
    if (!s_hid_started) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_ncm_enabled) {
        return ESP_OK;
    }

    esp_err_t ret = reinstall_tinyusb_driver(USB_MODE_HID_ONLY);
    if (ret != ESP_OK) {
        esp_err_t restore_ret = reinstall_tinyusb_driver(USB_MODE_HID_NCM);
        if (restore_ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to restore NCM mode after FIDO error: %s",
                     esp_err_to_name(restore_ret));
        }
        return ret;
    }
    s_ncm_enabled = false;
    usb_ncm_deinit();
    ESP_LOGI(TAG, "USB NCM disabled; FIDO-only PID restored");
    return ESP_OK;
}

bool usb_hid_ncm_is_enabled(void)
{
    return s_ncm_enabled;
}

esp_err_t usb_hid_enable_jade(void)
{
    if (!s_hid_started) {
        ESP_RETURN_ON_ERROR(usb_hid_keyboard_init(), TAG, "USB HID init before Jade failed");
    }
    if (s_jade_cdc_enabled) {
        return ESP_OK;
    }
    if (s_ncm_enabled) {
        ESP_RETURN_ON_ERROR(usb_hid_disable_ncm(), TAG, "disable NCM before Jade failed");
    }
    if (s_usbstore_exposed_to_usb) {
        ESP_RETURN_ON_ERROR(usb_hid_disable_usbstore(), TAG, "disable usbstore before Jade failed");
    }
    if (s_smartcard_reader_enabled) {
        ESP_RETURN_ON_ERROR(usb_hid_disable_smartcard_reader(), TAG,
                            "disable smartcard before Jade failed");
    }

    ESP_RETURN_ON_ERROR(reinstall_tinyusb_driver(USB_MODE_HID_JADE_CDC), TAG,
                        "TinyUSB Jade CDC mode failed");
    s_jade_cdc_enabled = true;
    ESP_LOGI(TAG, "USB FIDO+Jade CDC mode enabled");
    return ESP_OK;
}

esp_err_t usb_hid_disable_jade(void)
{
    if (!s_hid_started) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_jade_cdc_enabled) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(reinstall_tinyusb_driver(USB_MODE_HID_ONLY), TAG,
                        "TinyUSB HID-only mode failed");
    s_jade_cdc_enabled = false;
    ESP_LOGI(TAG, "USB Jade CDC disabled; FIDO-only PID restored");
    return ESP_OK;
}

bool usb_hid_jade_is_enabled(void)
{
    return s_jade_cdc_enabled;
}

esp_err_t usb_hid_keyboard_set_layout(hid_keyboard_layout_t layout)
{
    if ((unsigned int)layout >= HID_LAYOUT_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    keyboard_layout_set(layout);
    return ESP_OK;
}

hid_keyboard_layout_t usb_hid_keyboard_get_layout(void)
{
    return keyboard_layout_get();
}

esp_err_t usb_hid_keyboard_type_text(const char *text)
{
    if (!s_hid_started || !s_hid_queue) {
        return ESP_ERR_INVALID_STATE;
    }

    if (!text || text[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    hid_text_msg_t msg = {
        .type = HID_MESSAGE_TEXT,
    };
    strlcpy(msg.text, text, sizeof(msg.text));

    if (xQueueSend(s_hid_queue, &msg, 0) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    return ESP_OK;
}

esp_err_t usb_hid_keyboard_key_press(uint8_t key)
{
    return usb_hid_keyboard_report_press(0, key);
}

esp_err_t usb_hid_keyboard_report_press(uint8_t modifier, uint8_t key)
{
    if (!s_hid_started || !s_hid_queue) {
        return ESP_ERR_INVALID_STATE;
    }
    if (key == 0 && modifier == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    hid_text_msg_t msg = {
        .type = HID_MESSAGE_KEY_PRESS,
        .key = key,
        .modifier = modifier,
    };
    return xQueueSend(s_hid_queue, &msg, 0) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t usb_hid_keyboard_key_release(void)
{
    if (!s_hid_started || !s_hid_queue) {
        return ESP_ERR_INVALID_STATE;
    }

    hid_text_msg_t msg = {
        .type = HID_MESSAGE_KEY_RELEASE,
    };
    return xQueueSend(s_hid_queue, &msg, 0) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t usb_hid_keyboard_delay(uint32_t delay_ms)
{
    if (!s_hid_started || !s_hid_queue) {
        return ESP_ERR_INVALID_STATE;
    }

    hid_text_msg_t msg = {
        .type = HID_MESSAGE_DELAY,
        .delay_ms = delay_ms,
    };
    return xQueueSend(s_hid_queue, &msg, 0) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t usb_hid_keyboard_wait_idle(void)
{
    if (!s_hid_started || !s_hid_queue) {
        return ESP_ERR_INVALID_STATE;
    }

    hid_text_msg_t msg = {
        .type = HID_MESSAGE_SYNC,
        .waiter = xTaskGetCurrentTaskHandle(),
    };
    if (xQueueSend(s_hid_queue, &msg, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    return ESP_OK;
}

bool usb_hid_keyboard_is_ready(void)
{
    return s_hid_started && tud_mounted();
}

static bool usbstore_app_path_ready(void)
{
    errno = 0;
    DIR *dir = opendir(MSC_MOUNT_BASE_PATH);
    if (!dir) {
        return false;
    }

    closedir(dir);
    return true;
}

static esp_err_t check_usbstore_fat_boot_sector(void)
{
    s_msc_fat_partition_number = 0;

    uint8_t boot[512];
    esp_err_t ret = wl_read(s_msc_wl_handle, 0, boot, sizeof(boot));
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "usbstore FAT check failed: wl_read boot sector: %s", esp_err_to_name(ret));
        return ret;
    }

    uint16_t bytes_per_sector = (uint16_t)boot[11] | ((uint16_t)boot[12] << 8);
    uint8_t sectors_per_cluster = boot[13];
    uint16_t reserved_sectors = (uint16_t)boot[14] | ((uint16_t)boot[15] << 8);
    uint8_t fat_count = boot[16];
    uint16_t root_entry_count = (uint16_t)boot[17] | ((uint16_t)boot[18] << 8);
    uint16_t total_sectors_16 = (uint16_t)boot[19] | ((uint16_t)boot[20] << 8);
    uint16_t fat_size_16 = (uint16_t)boot[22] | ((uint16_t)boot[23] << 8);
    uint32_t total_sectors_32 = (uint32_t)boot[32] | ((uint32_t)boot[33] << 8) |
                                ((uint32_t)boot[34] << 16) | ((uint32_t)boot[35] << 24);
    uint16_t signature = (uint16_t)boot[510] | ((uint16_t)boot[511] << 8);
    char fs_type[9] = {0};
    memcpy(fs_type, &boot[54], 8);

    if (signature != 0xAA55) {
        ESP_LOGW(TAG, "usbstore FAT check failed: boot signature=0x%04x, expected 0xAA55", signature);
        return ESP_ERR_NOT_FOUND;
    }

    if (bytes_per_sector != wl_sector_size(s_msc_wl_handle)) {
        uint8_t partition_type = boot[0x1BE + 4];
        uint32_t partition_lba = (uint32_t)boot[0x1BE + 8] | ((uint32_t)boot[0x1BE + 9] << 8) |
                                 ((uint32_t)boot[0x1BE + 10] << 16) | ((uint32_t)boot[0x1BE + 11] << 24);
        uint32_t partition_sectors = (uint32_t)boot[0x1BE + 12] | ((uint32_t)boot[0x1BE + 13] << 8) |
                                     ((uint32_t)boot[0x1BE + 14] << 16) | ((uint32_t)boot[0x1BE + 15] << 24);
        bool is_fat_partition = partition_type == 0x01 || partition_type == 0x04 || partition_type == 0x06 ||
                                partition_type == 0x0B || partition_type == 0x0C || partition_type == 0x0E;

        if (!is_fat_partition || partition_lba == 0) {
            ESP_LOGW(TAG,
                     "usbstore FAT check failed: BPB sector=%u != WL sector=%u and MBR partition type=0x%02x lba=%lu sectors=%lu",
                     bytes_per_sector,
                     (unsigned int)wl_sector_size(s_msc_wl_handle),
                     partition_type,
                     (unsigned long)partition_lba,
                     (unsigned long)partition_sectors);
            return ESP_ERR_INVALID_SIZE;
        }

        size_t partition_offset = (size_t)partition_lba * wl_sector_size(s_msc_wl_handle);
        ESP_LOGI(TAG,
                 "usbstore MBR FAT partition detected: type=0x%02x lba=%lu sectors=%lu offset=%u",
                 partition_type,
                 (unsigned long)partition_lba,
                 (unsigned long)partition_sectors,
                 (unsigned int)partition_offset);
        s_msc_fat_partition_number = 1;

        ret = wl_read(s_msc_wl_handle, partition_offset, boot, sizeof(boot));
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "usbstore FAT check failed: wl_read partition boot sector: %s", esp_err_to_name(ret));
            return ret;
        }

        bytes_per_sector = (uint16_t)boot[11] | ((uint16_t)boot[12] << 8);
        sectors_per_cluster = boot[13];
        reserved_sectors = (uint16_t)boot[14] | ((uint16_t)boot[15] << 8);
        fat_count = boot[16];
        root_entry_count = (uint16_t)boot[17] | ((uint16_t)boot[18] << 8);
        total_sectors_16 = (uint16_t)boot[19] | ((uint16_t)boot[20] << 8);
        fat_size_16 = (uint16_t)boot[22] | ((uint16_t)boot[23] << 8);
        total_sectors_32 = (uint32_t)boot[32] | ((uint32_t)boot[33] << 8) |
                           ((uint32_t)boot[34] << 16) | ((uint32_t)boot[35] << 24);
        signature = (uint16_t)boot[510] | ((uint16_t)boot[511] << 8);
        memset(fs_type, 0, sizeof(fs_type));
        memcpy(fs_type, &boot[54], 8);

        if (signature != 0xAA55) {
            ESP_LOGW(TAG, "usbstore FAT check failed: partition boot signature=0x%04x, expected 0xAA55", signature);
            return ESP_ERR_NOT_FOUND;
        }

        if (bytes_per_sector != wl_sector_size(s_msc_wl_handle)) {
            ESP_LOGW(TAG, "usbstore FAT check failed: partition BPB sector=%u != WL sector=%u",
                     bytes_per_sector,
                     (unsigned int)wl_sector_size(s_msc_wl_handle));
            return ESP_ERR_INVALID_SIZE;
        }
    }

    if (sectors_per_cluster == 0 || reserved_sectors == 0 || fat_count == 0) {
        ESP_LOGW(TAG,
                 "usbstore FAT check failed: invalid BPB spc=%u reserved=%u fats=%u",
                 sectors_per_cluster,
                 reserved_sectors,
                 fat_count);
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGI(TAG,
             "usbstore FAT boot OK: bps=%u spc=%u reserved=%u fats=%u root_entries=%u total16=%u total32=%lu fatsz16=%u type='%.8s'",
             bytes_per_sector,
             sectors_per_cluster,
             reserved_sectors,
             fat_count,
             root_entry_count,
             total_sectors_16,
             (unsigned long)total_sectors_32,
             fat_size_16,
             fs_type);
    return ESP_OK;
}

static esp_err_t prepare_usbstore_fatfs_mapping(void)
{
#if FF_MULTI_PARTITION
    if (s_msc_fat_partition_number == 0) {
        return ESP_OK;
    }

    BYTE pdrv = 0xFF;
    esp_err_t ret = ff_diskio_get_drive(&pdrv);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to predict FatFs pdrv for usbstore: %s", esp_err_to_name(ret));
        return ret;
    }

    VolToPart[pdrv].pd = pdrv;
    VolToPart[pdrv].pt = s_msc_fat_partition_number;
    ESP_LOGI(TAG, "FatFs mapping for usbstore: logical=%u physical=%u partition=%u",
             pdrv,
             VolToPart[pdrv].pd,
             VolToPart[pdrv].pt);
#endif
    return ESP_OK;
}

static esp_err_t check_usbstore_config(void)
{
    if (!s_msc_storage) {
        ESP_LOGW(TAG, "usbstore config check failed: MSC storage handle is NULL");
        return ESP_ERR_INVALID_STATE;
    }

    if (s_msc_wl_handle == WL_INVALID_HANDLE) {
        ESP_LOGW(TAG, "usbstore config check failed: WL handle is invalid");
        return ESP_ERR_INVALID_STATE;
    }

    if (MSC_MOUNT_BASE_PATH[0] != '/') {
        ESP_LOGW(TAG, "usbstore config check failed: mount path must start with '/': %s", MSC_MOUNT_BASE_PATH);
        return ESP_ERR_INVALID_ARG;
    }

    const esp_partition_t *storage_partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                                                        ESP_PARTITION_SUBTYPE_DATA_FAT,
                                                                        MSC_PARTITION_LABEL);
    if (!storage_partition) {
        ESP_LOGW(TAG, "usbstore config check failed: FAT partition '%s' not found", MSC_PARTITION_LABEL);
        return ESP_ERR_NOT_FOUND;
    }

    size_t wl_sector_size_bytes = wl_sector_size(s_msc_wl_handle);
    size_t wl_size_bytes = wl_size(s_msc_wl_handle);
    if (wl_sector_size_bytes == 0 || wl_size_bytes == 0) {
        ESP_LOGW(TAG, "usbstore config check failed: invalid WL size=%u sector=%u",
                 (unsigned int)wl_size_bytes,
                 (unsigned int)wl_sector_size_bytes);
        return ESP_ERR_INVALID_SIZE;
    }

    esp_err_t ret = check_usbstore_fat_boot_sector();
    if (ret != ESP_OK) {
        return ret;
    }

    if (CONFIG_TINYUSB_MSC_BUFSIZE < CONFIG_WL_SECTOR_SIZE) {
        ESP_LOGW(TAG, "usbstore config check failed: MSC buffer=%d smaller than WL sector=%d",
                 CONFIG_TINYUSB_MSC_BUFSIZE,
                 CONFIG_WL_SECTOR_SIZE);
        return ESP_ERR_INVALID_SIZE;
    }

    uint32_t sector_count = 0;
    uint32_t sector_size = 0;
    ret = tinyusb_msc_get_storage_capacity(s_msc_storage, &sector_count);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "usbstore config check failed: get capacity: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = tinyusb_msc_get_storage_sector_size(s_msc_storage, &sector_size);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "usbstore config check failed: get sector size: %s", esp_err_to_name(ret));
        return ret;
    }

    if (sector_count == 0 || sector_size == 0) {
        ESP_LOGW(TAG, "usbstore config check failed: invalid MSC capacity sectors=%lu sector_size=%lu",
                 (unsigned long)sector_count,
                 (unsigned long)sector_size);
        return ESP_ERR_INVALID_SIZE;
    }

    if (sector_size != wl_sector_size_bytes) {
        ESP_LOGW(TAG, "usbstore config check failed: MSC sector=%lu != WL sector=%u",
                 (unsigned long)sector_size,
                 (unsigned int)wl_sector_size_bytes);
        return ESP_ERR_INVALID_SIZE;
    }

    uint64_t msc_size_bytes = (uint64_t)sector_count * sector_size;
    if (msc_size_bytes > wl_size_bytes) {
        ESP_LOGW(TAG, "usbstore config check failed: MSC size=%llu > WL size=%u",
                 (unsigned long long)msc_size_bytes,
                 (unsigned int)wl_size_bytes);
        return ESP_ERR_INVALID_SIZE;
    }

    ESP_LOGI(TAG,
             "usbstore config OK: partition=%s offset=0x%lx size=%lu mount=%s wl_size=%u wl_sector=%u msc_sectors=%lu msc_sector=%lu",
             storage_partition->label,
             (unsigned long)storage_partition->address,
             (unsigned long)storage_partition->size,
             MSC_MOUNT_BASE_PATH,
             (unsigned int)wl_size_bytes,
             (unsigned int)wl_sector_size_bytes,
             (unsigned long)sector_count,
             (unsigned long)sector_size);
    return ESP_OK;
}

esp_err_t usb_hid_mount_usbstore_app(void)
{
    if (usbstore_app_path_ready()) {
        log_storage_resume();
        ESP_LOGI(TAG, "usbstore already mounted to APP: %s", MSC_MOUNT_BASE_PATH);
        return ESP_OK;
    }

    int first_errno = errno;
    tinyusb_msc_mount_point_t current_mount_point = TINYUSB_MSC_STORAGE_MOUNT_USB;
    esp_err_t ret = tinyusb_msc_get_storage_mount_point(s_msc_storage, &current_mount_point);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to get usbstore mount point before APP mount: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGW(TAG, "usbstore path '%s' not ready before APP mount: errno=%d current=%s",
             MSC_MOUNT_BASE_PATH,
             first_errno,
             current_mount_point == TINYUSB_MSC_STORAGE_MOUNT_USB ? "USB" : "APP");

    if (current_mount_point == TINYUSB_MSC_STORAGE_MOUNT_USB) {
        ESP_LOGI(TAG, "Detach USB MSC before mounting usbstore to APP");
        ret = usb_hid_disable_usbstore();
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to detach USB MSC before APP mount: %s",
                     esp_err_to_name(ret));
            return ret;
        }
        if (usbstore_app_path_ready()) {
            log_storage_resume();
            ESP_LOGI(TAG, "usbstore APP mount verified after USB MSC detach: %s",
                     MSC_MOUNT_BASE_PATH);
            return ESP_OK;
        }
    }

    ret = check_usbstore_config();
    if (ret != ESP_OK) {
        return ret;
    }

    if (s_msc_fat_partition_number != 0) {
        ESP_LOGW(TAG, "usbstore uses MBR partition layout; reformat to ESP FatFs layout");
        ret = erase_and_format_usbstore();
        if (ret != ESP_OK) {
            return ret;
        }

        if (usbstore_app_path_ready()) {
            log_storage_resume();
            ESP_LOGI(TAG, "usbstore APP mount verified after format: %s", MSC_MOUNT_BASE_PATH);
            return ESP_OK;
        }

        ESP_LOGW(TAG, "usbstore path '%s' still not ready after format: errno=%d", MSC_MOUNT_BASE_PATH, errno);
    }

    ret = prepare_usbstore_fatfs_mapping();
    if (ret != ESP_OK) {
        return ret;
    }

    for (int attempt = 1; attempt <= 2; attempt++) {
        ESP_LOGI(TAG, "Mount usbstore to APP attempt %d", attempt);
        ret = set_storage_mount_point_wait(TINYUSB_MSC_STORAGE_MOUNT_APP);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Mount usbstore to APP attempt %d failed: %s", attempt, esp_err_to_name(ret));
        } else if (usbstore_app_path_ready()) {
            ESP_LOGI(TAG, "usbstore APP mount verified: %s", MSC_MOUNT_BASE_PATH);
            return ESP_OK;
        } else {
            ESP_LOGW(TAG, "usbstore APP mount attempt %d has no path '%s': errno=%d",
                     attempt,
                     MSC_MOUNT_BASE_PATH,
                     errno);
        }

        ESP_LOGW(TAG, "Keep usbstore on APP side after failed APP mount attempt %d", attempt);
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    ESP_LOGW(TAG, "usbstore APP mount verification failed after retries");
    return ESP_ERR_NOT_FOUND;
}

esp_err_t usb_hid_dump_storage_files(void)
{
    if (!s_msc_storage) {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_dump_in_progress) {
        return ESP_ERR_INVALID_STATE;
    }

    s_dump_in_progress = true;
    if (xTaskCreate(storage_dump_task, "usb_dump", MSC_DUMP_TASK_STACK, NULL, MSC_DUMP_TASK_PRIORITY, NULL) != pdPASS) {
        s_dump_in_progress = false;
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

static esp_err_t usbstore_set_mount_point(tinyusb_msc_mount_point_t mount_point)
{
    if (!s_msc_storage) {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_dump_in_progress) {
        return ESP_ERR_INVALID_STATE;
    }

    tinyusb_msc_mount_point_t current_mount_point;
    esp_err_t ret = tinyusb_msc_get_storage_mount_point(s_msc_storage, &current_mount_point);
    if (ret != ESP_OK) {
        return ret;
    }

    if (current_mount_point == mount_point) {
        if (mount_point == TINYUSB_MSC_STORAGE_MOUNT_APP) {
            log_storage_resume();
        }
        return ESP_OK;
    }

    ret = set_storage_mount_point_wait(mount_point);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to switch usbstore to %s: %s",
                 mount_point == TINYUSB_MSC_STORAGE_MOUNT_USB ? "USB" : "APP",
                 esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "usbstore switched to %s mode",
             mount_point == TINYUSB_MSC_STORAGE_MOUNT_USB ? "USB disk" : "APP/ejected");
    return ESP_OK;
}

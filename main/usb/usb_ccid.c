#include "usb_ccid.h"

#include <string.h>

#include "canokey_esp32p4.h"
#include "device/usbd_pvt.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tusb.h"

#define CCID_HEADER_SIZE 10u
#define CCID_PC_TO_RDR_ICCPOWERON 0x62u
#define CCID_PC_TO_RDR_ICCPOWEROFF 0x63u
#define CCID_PC_TO_RDR_GETSLOTSTATUS 0x65u  // 0x65  0x63
#define CCID_PC_TO_RDR_SETPARAMETERS 0x61u
#define CCID_PC_TO_RDR_GETPARAMETERS 0x6Cu
#define CCID_PC_TO_RDR_RESETPARAMETERS 0x6Du
#define CCID_PC_TO_RDR_XFRBLOCK 0x6Fu
#define CCID_PC_TO_RDR_SETDATARATEANDCLOCKFREQUENCY 0x73u
#define CCID_RDR_TO_PC_DATABLOCK 0x80u
#define CCID_RDR_TO_PC_SLOTSTATUS 0x81u
#define CCID_RDR_TO_PC_PARAMETERS 0x82u
#define CCID_RDR_TO_PC_DATARATEANDCLOCKFREQUENCY 0x84u
#define CCID_RX_SIZE 1024u
#define CCID_DUMP_MAX_BYTES 128u
#define CCID_INT_MSG_SIZE 2u
#define CCID_WORKER_STACK_SIZE 24576u
#define CCID_WORKER_PRIORITY 5u
#define CCID_SLOT_NO_ERROR 0x00u
#define CCID_SLOT_ERROR_CMD_NOT_SUPPORTED 0x00u
#define CCID_STATUS_ACTIVE 0x00u   /* ICC present and active */
#define CCID_STATUS_INACTIVE 0x01u /* ICC present but inactive */
#define CCID_STATUS_NOT_PRESENT 0x02u
#define CCID_STATUS_FAILED 0x40u

static const uint8_t s_ccid_default_t1_params[] = {
    0x11, /* Fi=372, Di=1 */
    0x10, /* direct convention, LRC */
    0x00, /* extra guard time */
    0x65, /* BWI=6, CWI=5 */
    0x00, /* clock stop not allowed */
    0xFE, /* IFSC */
    0x00, /* NAD */
};
static uint8_t s_ccid_rx[CCID_RX_SIZE] __attribute__((aligned(64)));
static uint8_t s_ccid_tx[CCID_RX_SIZE] __attribute__((aligned(64)));
static uint8_t s_ccid_worker_rx[CCID_RX_SIZE] __attribute__((aligned(64)));
static uint8_t s_ccid_t1_params[sizeof(s_ccid_default_t1_params)] = {
    0x11, 0x10, 0x00, 0x65, 0x00, 0xFE, 0x00,
};
static uint8_t s_ccid_int_msg[CCID_INT_MSG_SIZE] __attribute__((aligned(64))) = {
    0x50,       /* RDR_to_PC_NotifySlotChange */
    0x03,       /* slot 0: card present and status changed */
};
static uint8_t s_ccid_ep_out;
static uint8_t s_ccid_ep_in;
static uint8_t s_ccid_ep_int_in;
static bool s_ccid_card_active;
static bool s_ccid_card_inserted;
static bool s_ccid_rx_armed;
static bool s_ccid_int_in_busy;
static bool s_ccid_int_task_started;
static bool s_ccid_ready_task_started;
static bool s_ccid_worker_busy;
static uint8_t s_ccid_worker_rhport;
static uint32_t s_ccid_worker_rx_len;
static TaskHandle_t s_ccid_worker_task_handle;
static const char *TAG = "usb_ccid";

static bool ccid_start_rx(uint8_t rhport);

static void ccid_log_response(const uint8_t *response, size_t response_len)
{
    if (response_len >= CCID_HEADER_SIZE) {
        size_t payload_len = (uint32_t)response[1] |
                             ((uint32_t)response[2] << 8) |
                             ((uint32_t)response[3] << 16) |
                             ((uint32_t)response[4] << 24);
        ESP_LOGI(TAG, "CCID IN msg=0x%02x len=%lu slot=%u seq=%u status=0x%02x error=0x%02x specific=0x%02x total=%u",
                 response[0],
                 (unsigned long)payload_len,
                 response[5],
                 response[6],
                 response[7],
                 response[8],
                 response[9],
                 (unsigned)response_len);
        if (payload_len > 0 && response_len > CCID_HEADER_SIZE) {
            ESP_LOG_BUFFER_HEX_LEVEL(TAG,
                                     response + CCID_HEADER_SIZE,
                                     payload_len > 32 ? 32 : payload_len,
                                     ESP_LOG_INFO);
        }
    }
}

static void ccid_worker_task(void *arg)
{
    (void)arg;

    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        size_t response_len = usb_ccid_process_message(s_ccid_worker_rx,
                                                       s_ccid_worker_rx_len,
                                                       s_ccid_tx,
                                                       sizeof(s_ccid_tx));
        ccid_log_response(s_ccid_tx, response_len);
        if (response_len) {
            if (!usbd_edpt_xfer(s_ccid_worker_rhport, s_ccid_ep_in, s_ccid_tx, (uint16_t)response_len)) {
                ESP_LOGW(TAG, "CCID worker IN submit failed: ep=0x%02x", s_ccid_ep_in);
                s_ccid_worker_busy = false;
                (void)ccid_start_rx(s_ccid_worker_rhport);
            }
        } else {
            ESP_LOGW(TAG, "CCID worker produced empty response");
            s_ccid_worker_busy = false;
            (void)ccid_start_rx(s_ccid_worker_rhport);
        }
    }
}

static void ccid_start_worker_task(void)
{
    if (s_ccid_worker_task_handle) {
        return;
    }
    if (xTaskCreate(ccid_worker_task,
                    "ccid_worker",
                    CCID_WORKER_STACK_SIZE,
                    NULL,
                    CCID_WORKER_PRIORITY,
                    &s_ccid_worker_task_handle) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create CCID worker task");
    }
}

static void ccid_interrupt_task(void *arg)
{
    (void)arg;

    /* Notify insertion once after the device is mounted. Do not repeatedly
     * report a stable state: 0x03 is the insertion/change event. */
    vTaskDelay(pdMS_TO_TICKS(100));
    if (tud_mounted() && s_ccid_ep_int_in && !s_ccid_int_in_busy) {
        if (usbd_edpt_xfer(0, s_ccid_ep_int_in, s_ccid_int_msg, sizeof(s_ccid_int_msg))) {
            s_ccid_int_in_busy = true;
            ESP_LOGI(TAG, "CCID INT IN msg=0x%02x state=0x%02x",
                     s_ccid_int_msg[0], s_ccid_int_msg[1]);
        } else {
            ESP_LOGW(TAG, "CCID INT IN submit failed: ep=0x%02x", s_ccid_ep_int_in);
        }
    }
    s_ccid_int_task_started = false;
    vTaskDelete(NULL);
}

static void ccid_start_interrupt_task(void)
{
    if (s_ccid_int_task_started) {
        return;
    }
    if (xTaskCreate(ccid_interrupt_task, "ccid_int", 2048, NULL, 4, NULL) == pdPASS) {
        s_ccid_int_task_started = true;
    } else {
        ESP_LOGW(TAG, "Failed to create CCID interrupt notify task");
    }
}

static void ccid_ready_task(void *arg)
{
    (void)arg;

    for (int i = 0; i < 100 && (!tud_mounted() || !canokey_esp32p4_is_ready() || !s_ccid_ep_out || !s_ccid_ep_in); i++) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    if (tud_mounted() && canokey_esp32p4_is_ready() && s_ccid_ep_out && s_ccid_ep_in) {
        /* Wait until CanoKey can really answer IccPowerOn. Otherwise Windows
         * may send 0x62 during boot and cache the early 6F00 failure. */
        ESP_LOGI(TAG, "CanoKey ready; enabling CCID endpoints");
        ccid_start_interrupt_task();
        (void)ccid_start_rx(0);
    } else {
        ESP_LOGW(TAG, "CCID enable skipped: mounted=%u ready=%u ep_out=0x%02x ep_in=0x%02x",
                 tud_mounted() ? 1 : 0,
                 canokey_esp32p4_is_ready() ? 1 : 0,
                 s_ccid_ep_out,
                 s_ccid_ep_in);
    }

    s_ccid_ready_task_started = false;
    vTaskDelete(NULL);
}

static void ccid_start_ready_task(void)
{
    if (s_ccid_ready_task_started) {
        return;
    }
    if (xTaskCreate(ccid_ready_task, "ccid_ready", 2048, NULL, 4, NULL) == pdPASS) {
        s_ccid_ready_task_started = true;
    } else {
        ESP_LOGW(TAG, "Failed to create CCID ready task");
    }
}

static bool ccid_start_rx(uint8_t rhport)
{
    if (!canokey_esp32p4_is_ready()) {
        ESP_LOGI(TAG, "CCID OUT arm delayed: CanoKey not ready");
        ccid_start_ready_task();
        return false;
    }
    if (!s_ccid_ep_out || !s_ccid_ep_in) {
        ESP_LOGW(TAG, "CCID OUT arm skipped: ep_out=0x%02x ep_in=0x%02x", s_ccid_ep_out, s_ccid_ep_in);
        ccid_start_ready_task();
        return false;
    }
    if (s_ccid_rx_armed) {
        ESP_LOGD(TAG, "CCID OUT already armed");
        return true;
    }

    bool ok = usbd_edpt_xfer(rhport, s_ccid_ep_out, s_ccid_rx, sizeof(s_ccid_rx));
    s_ccid_rx_armed = ok;
    ESP_LOGI(TAG, "CCID OUT arm %s: ep=0x%02x size=%u", ok ? "ok" : "failed", s_ccid_ep_out, (unsigned)sizeof(s_ccid_rx));
    return ok;
}

static void ccid_driver_init(void) { }
static bool ccid_driver_deinit(void) { return true; }
static void ccid_driver_reset(uint8_t rhport)
{
    /* USB bus reset/detach is equivalent to card removal for our virtual card:
     * drop the held CCID applet session so volatile PIN state does not leak. */
    canokey_esp32p4_ccid_power_off();
    s_ccid_ep_out = 0;
    s_ccid_ep_in = 0;
    s_ccid_ep_int_in = 0;
    s_ccid_rx_armed = false;
    s_ccid_int_in_busy = false;
    s_ccid_ready_task_started = false;
    s_ccid_worker_busy = false;
    s_ccid_card_active = false;
    (void)rhport;
}

static uint16_t ccid_driver_open(uint8_t rhport, tusb_desc_interface_t const *intf, uint16_t max_len)
{
    if (intf->bInterfaceClass != TUSB_CLASS_SMART_CARD) {
        return 0;
    }

    s_ccid_ep_out = 0;
    s_ccid_ep_in = 0;
    s_ccid_ep_int_in = 0;
    s_ccid_rx_armed = false;
    s_ccid_int_in_busy = false;

    const uint8_t *p = tu_desc_next(intf);
    const uint8_t *end = (const uint8_t *)intf + max_len;
    while (p < end && p[1] != TUSB_DESC_INTERFACE && p[1] != TUSB_DESC_INTERFACE_ASSOCIATION) {
        if (p[0] == 0) {
            return 0;
        }
        if (p[1] == TUSB_DESC_ENDPOINT) {
            const tusb_desc_endpoint_t *ep = (const tusb_desc_endpoint_t *)p;
            ESP_LOGI(TAG, "CCID opening endpoint: addr=0x%02x xfer=%u size=%u",
                     ep->bEndpointAddress, ep->bmAttributes.xfer, (unsigned)tu_edpt_packet_size(ep));
            if (!usbd_edpt_open(rhport, ep)) {
                ESP_LOGE(TAG, "CCID endpoint open failed: addr=0x%02x xfer=%u size=%u",
                         ep->bEndpointAddress, ep->bmAttributes.xfer, (unsigned)tu_edpt_packet_size(ep));
                return 0;
            }
            uint8_t xfer = ep->bmAttributes.xfer;
            if (tu_edpt_dir(ep->bEndpointAddress) == TUSB_DIR_OUT) {
                s_ccid_ep_out = ep->bEndpointAddress;
            } else if (xfer == TUSB_XFER_INTERRUPT) {
                s_ccid_ep_int_in = ep->bEndpointAddress;
            } else {
                s_ccid_ep_in = ep->bEndpointAddress;
            }
        }
        p += p[0];
    }
    if (!s_ccid_ep_out || !s_ccid_ep_in) {
        return 0;
    }
    return (uint16_t)(p - (const uint8_t *)intf);
}

static bool ccid_driver_control(uint8_t rhport, uint8_t stage, tusb_control_request_t const *request)
{
    (void)rhport;
    (void)stage;
    (void)request;
    return false;
}

static bool ccid_driver_xfer(uint8_t rhport, uint8_t ep_addr, xfer_result_t result, uint32_t bytes)
{
    if (result != XFER_RESULT_SUCCESS) {
        if (ep_addr == s_ccid_ep_out) {
            s_ccid_rx_armed = false;
        }
        ESP_LOGW(TAG, "CCID transfer failed: ep=0x%02x result=%u bytes=%lu", ep_addr, result, (unsigned long)bytes);
        return true;
    }
    if (ep_addr == s_ccid_ep_out) {
        s_ccid_rx_armed = false;
        ESP_LOGI(TAG, "CCID OUT raw bytes=%lu dump_len=%u",
                 (unsigned long)bytes,
                 (unsigned)(bytes > CCID_DUMP_MAX_BYTES ? CCID_DUMP_MAX_BYTES : bytes));
        ESP_LOG_BUFFER_HEX_LEVEL(TAG,
                                 s_ccid_rx,
                                 bytes > CCID_DUMP_MAX_BYTES ? CCID_DUMP_MAX_BYTES : bytes,
                                 ESP_LOG_INFO);
        if (bytes >= CCID_HEADER_SIZE && s_ccid_rx[0] == CCID_PC_TO_RDR_XFRBLOCK) {
            if (!s_ccid_worker_task_handle) {
                ESP_LOGW(TAG, "CCID worker unavailable");
                return ccid_start_rx(rhport);
            }
            if (s_ccid_worker_busy) {
                ESP_LOGW(TAG, "CCID worker busy; dropping XfrBlock");
                return ccid_start_rx(rhport);
            }
            memcpy(s_ccid_worker_rx, s_ccid_rx, bytes > sizeof(s_ccid_worker_rx) ? sizeof(s_ccid_worker_rx) : bytes);
            s_ccid_worker_rx_len = bytes > sizeof(s_ccid_worker_rx) ? sizeof(s_ccid_worker_rx) : bytes;
            s_ccid_worker_rhport = rhport;
            s_ccid_worker_busy = true;
            ESP_LOGI(TAG, "CCID XfrBlock queued to worker: bytes=%lu", (unsigned long)s_ccid_worker_rx_len);
            xTaskNotifyGive(s_ccid_worker_task_handle);
            return true;
        }
        size_t response_len = usb_ccid_process_message(s_ccid_rx, bytes, s_ccid_tx, sizeof(s_ccid_tx));
        ccid_log_response(s_ccid_tx, response_len);
        if (response_len && !usbd_edpt_xfer(rhport, s_ccid_ep_in, s_ccid_tx, (uint16_t)response_len)) {
            return false;
        }
        return true;
    }
    if (ep_addr == s_ccid_ep_in) {
        s_ccid_worker_busy = false;
        ESP_LOGI(TAG, "CCID IN complete; re-arm OUT");
        return ccid_start_rx(rhport);
    }
    if (ep_addr == s_ccid_ep_int_in) {
        s_ccid_int_in_busy = false;
        return true;
    }
    return false;
}

static usbd_class_driver_t const s_ccid_driver __attribute__((unused)) = {
    .name = "CCID",
    .init = ccid_driver_init,
    .deinit = ccid_driver_deinit,
    .reset = ccid_driver_reset,
    .open = ccid_driver_open,
    .control_xfer_cb = ccid_driver_control,
    .xfer_cb = ccid_driver_xfer,
};

/* Enable this callback only after the CCID interface is enabled in the USB
 * configuration descriptor. */
#if USB_CCID_ENABLED
usbd_class_driver_t const *usbd_app_driver_get_cb(uint8_t *driver_count)
{
    *driver_count = 1;
    return &s_ccid_driver;
}
#endif

bool usb_ccid_start(void)
{
    ccid_start_worker_task();
    if (!canokey_esp32p4_is_ready()) {
        ESP_LOGI(TAG, "CCID start delayed: CanoKey not ready");
        ccid_start_ready_task();
        return false;
    }
    if (s_ccid_card_inserted) {
        ccid_start_interrupt_task();
    }
    return ccid_start_rx(0);
}

bool usb_ccid_activate_card(void)
{
    s_ccid_card_inserted = true;
    s_ccid_card_active = false;
    s_ccid_int_msg[1] = 0x03;
    ccid_start_interrupt_task();
    return usb_ccid_start();
}

bool usb_ccid_activate_openpgp_card(void)
{
    return usb_ccid_activate_card();
}

void usb_ccid_deactivate_card(void)
{
    bool notify_removal = s_ccid_card_inserted || s_ccid_card_active;

    canokey_esp32p4_ccid_power_off();
    s_ccid_card_inserted = false;
    s_ccid_card_active = false;

    if (notify_removal) {
        s_ccid_int_msg[1] = 0x02;
        ccid_start_interrupt_task();
    }
}

static void write_u32_le(uint8_t *d, uint32_t v)
{
    d[0] = (uint8_t)v;
    d[1] = (uint8_t)(v >> 8);
    d[2] = (uint8_t)(v >> 16);
    d[3] = (uint8_t)(v >> 24);
}

static void ccid_write_header(uint8_t *o, uint8_t msg, uint32_t len, uint8_t slot, uint8_t seq)
{
    memset(o, 0, CCID_HEADER_SIZE);
    o[0] = msg;
    write_u32_le(o + 1, len);
    o[5] = slot;
    o[6] = seq;
    o[7] = !s_ccid_card_inserted ? CCID_STATUS_NOT_PRESENT :
           (s_ccid_card_active ? CCID_STATUS_ACTIVE : CCID_STATUS_INACTIVE);
    o[8] = CCID_SLOT_NO_ERROR;
}

static size_t ccid_reply_slot_status(uint8_t *o, uint8_t slot, uint8_t seq)
{
    ccid_write_header(o, CCID_RDR_TO_PC_SLOTSTATUS, 0, slot, seq);
    /* Keep Windows enumeration behavior stable: the virtual ICC is always
     * present, and older working logs showed this status on 0x63 polling. */
    o[7] = s_ccid_card_inserted ? CCID_STATUS_INACTIVE : CCID_STATUS_NOT_PRESENT;
    return CCID_HEADER_SIZE;
}

static size_t ccid_reply_unsupported(uint8_t *o, uint8_t slot, uint8_t seq, uint8_t cmd)
{
    ccid_write_header(o, CCID_RDR_TO_PC_SLOTSTATUS, 0, slot, seq);
    o[7] = (uint8_t)(CCID_STATUS_FAILED | (s_ccid_card_active ? CCID_STATUS_ACTIVE : CCID_STATUS_INACTIVE));
    o[8] = CCID_SLOT_ERROR_CMD_NOT_SUPPORTED;
    ESP_LOGW(TAG, "Unsupported CCID command: 0x%02x", cmd);
    return CCID_HEADER_SIZE;
}

static size_t ccid_reply_parameters(uint8_t *o, uint8_t slot, uint8_t seq, size_t cap)
{
    if (cap < CCID_HEADER_SIZE + sizeof(s_ccid_t1_params)) {
        return 0;
    }

    ccid_write_header(o, CCID_RDR_TO_PC_PARAMETERS, sizeof(s_ccid_t1_params), slot, seq);
    o[9] = 0x01;
    memcpy(o + CCID_HEADER_SIZE, s_ccid_t1_params, sizeof(s_ccid_t1_params));
    return CCID_HEADER_SIZE + sizeof(s_ccid_t1_params);
}

static size_t ccid_reply_parameters_failed(uint8_t *o, uint8_t slot, uint8_t seq, uint8_t error)
{
    ccid_write_header(o, CCID_RDR_TO_PC_PARAMETERS, 0, slot, seq);
    o[7] = (uint8_t)(CCID_STATUS_FAILED | (s_ccid_card_active ? CCID_STATUS_ACTIVE : CCID_STATUS_INACTIVE));
    o[8] = error;
    o[9] = 0x01;
    return CCID_HEADER_SIZE;
}

static size_t ccid_reply_data_rate(uint8_t *o, uint8_t slot, uint8_t seq, size_t cap)
{
    if (cap < CCID_HEADER_SIZE + 8) {
        return 0;
    }

    ccid_write_header(o, CCID_RDR_TO_PC_DATARATEANDCLOCKFREQUENCY, 8, slot, seq);
    write_u32_le(o + CCID_HEADER_SIZE, 4000);
    write_u32_le(o + CCID_HEADER_SIZE + 4, 9600);
    ESP_LOGI(TAG, "CCID SetDataRate fixed response: clock=4000 data_rate=9600");
    return CCID_HEADER_SIZE + 8;
}

static size_t ccid_reply_status_word(uint8_t *o, size_t cap, uint8_t slot, uint8_t seq, uint16_t sw)
{
    if (cap < CCID_HEADER_SIZE + 2) {
        return 0;
    }

    s_ccid_card_active = true;
    ccid_write_header(o, CCID_RDR_TO_PC_DATABLOCK, 2, slot, seq);
    o[CCID_HEADER_SIZE] = (uint8_t)(sw >> 8);
    o[CCID_HEADER_SIZE + 1] = (uint8_t)sw;
    return CCID_HEADER_SIZE + 2;
}

size_t usb_ccid_process_message(const uint8_t *r, size_t n, uint8_t *o, size_t cap)
{
    if (!r || !o || n < CCID_HEADER_SIZE || cap < CCID_HEADER_SIZE) {
        return 0;
    }
    uint8_t cmd = r[0];
    uint8_t slot = r[5];
    uint8_t seq = r[6];
    uint32_t len = (uint32_t)r[1] | ((uint32_t)r[2] << 8) |
                   ((uint32_t)r[3] << 16) | ((uint32_t)r[4] << 24);
    ESP_LOGI(TAG, "CCID OUT cmd=0x%02x slot=%u seq=%u len=%lu bytes=%u",
             cmd,
             slot,
             seq,
             (unsigned long)len,
             (unsigned)n);
    if (slot != 0) {
        ccid_write_header(o, CCID_RDR_TO_PC_SLOTSTATUS, 0, slot, seq);
        o[7] = CCID_STATUS_FAILED;
        o[8] = 5;
        return CCID_HEADER_SIZE;
    }
    if (cmd == CCID_PC_TO_RDR_ICCPOWEROFF) {
        /* Windows/scdaemon sends 0x63 repeatedly in this virtual-reader path.
         * Treat it as a harmless slot-status poll; dropping the applet session
         * here makes the card disappear and clears PW3 between APDUs. */
        return ccid_reply_slot_status(o, slot, seq);
    }
    if (cmd == CCID_PC_TO_RDR_GETSLOTSTATUS) {
        return ccid_reply_slot_status(o, slot, seq);
    }
    if (cmd == CCID_PC_TO_RDR_ICCPOWERON) {
        static const uint8_t atr[] = {0x3B,0xF7,0x11,0x00,0x00,0x81,0x31,0xFE,0x65,0x43,0x61,0x6E,0x6F,0x6B,0x65,0x79,0x99};
        //static const uint8_t atr[] = {0x3B,0x80,0x80,0x01,0x01};
        if (!s_ccid_card_inserted) {
            ESP_LOGI(TAG, "CCID PowerOn rejected: no virtual card inserted");
            return ccid_reply_slot_status(o, slot, seq);
        }
        if (cap < CCID_HEADER_SIZE + sizeof(atr)) {
            return 0;
        }
        /* IccPowerOn starts a fresh CCID card session and keeps OpenPGP volatile
         * state alive until a later PowerOff or USB reset. */
        if (canokey_esp32p4_ccid_power_on() != ESP_OK) {
            return ccid_reply_status_word(o, cap, slot, seq, 0x6F00);
        }
        s_ccid_card_active = true;
        memcpy(s_ccid_t1_params, s_ccid_default_t1_params, sizeof(s_ccid_t1_params));
        ccid_write_header(o, CCID_RDR_TO_PC_DATABLOCK, sizeof(atr), slot, seq);
        memcpy(o + CCID_HEADER_SIZE, atr, sizeof(atr));
        return CCID_HEADER_SIZE + sizeof(atr);
    }
    if (cmd == CCID_PC_TO_RDR_GETPARAMETERS) {
        s_ccid_card_active = true;
        return ccid_reply_parameters(o, slot, seq, cap);
    }
    if (cmd == CCID_PC_TO_RDR_RESETPARAMETERS) {
        s_ccid_card_active = true;
        memcpy(s_ccid_t1_params, s_ccid_default_t1_params, sizeof(s_ccid_t1_params));
        return ccid_reply_parameters(o, slot, seq, cap);
    }
    if (cmd == CCID_PC_TO_RDR_SETPARAMETERS) {
        s_ccid_card_active = true;
        if (len != sizeof(s_ccid_t1_params) || len > n - CCID_HEADER_SIZE) {
            ESP_LOGW(TAG, "Invalid SetParameters length: len=%lu bytes=%u", (unsigned long)len, (unsigned)n);
            return ccid_reply_parameters_failed(o, slot, seq, 0x01);
        }
        memcpy(s_ccid_t1_params, r + CCID_HEADER_SIZE, sizeof(s_ccid_t1_params));
        ESP_LOGI(TAG, "CCID SetParameters accepted");
        ESP_LOG_BUFFER_HEX_LEVEL(TAG, s_ccid_t1_params, sizeof(s_ccid_t1_params), ESP_LOG_INFO);
        return ccid_reply_parameters(o, slot, seq, cap);
    }
    if (cmd == CCID_PC_TO_RDR_SETDATARATEANDCLOCKFREQUENCY) {
        s_ccid_card_active = true;
        if (len != 8 || len > n - CCID_HEADER_SIZE) {
            ESP_LOGW(TAG, "Invalid SetDataRate length: len=%lu bytes=%u",
                     (unsigned long)len, (unsigned)n);
            return 0;
        }
        return ccid_reply_data_rate(o, slot, seq, cap);
    }
    if (cmd != CCID_PC_TO_RDR_XFRBLOCK) {
        return ccid_reply_unsupported(o, slot, seq, cmd);
    }
    if (!s_ccid_card_inserted) {
        ESP_LOGI(TAG, "CCID XfrBlock rejected: no virtual card inserted");
        return ccid_reply_slot_status(o, slot, seq);
    }
    if (len > n - CCID_HEADER_SIZE || len > 65535u) {
        ESP_LOGW(TAG, "Invalid CCID XfrBlock length: len=%lu bytes=%u",
                 (unsigned long)len,
                 (unsigned)n);
        return 0;
    }
    uint8_t data[1024];
    uint16_t response_len = 0;
    uint16_t status_word = 0;
    if (canokey_esp32p4_process_ccid_apdu(r + CCID_HEADER_SIZE, (uint16_t)len,
                                          data, sizeof(data), &response_len, &status_word) != 0) {
        ESP_LOGW(TAG, "APDU dispatch failed");
        return ccid_reply_status_word(o, cap, slot, seq, 0x6F00);
    }
    ESP_LOGI(TAG, "APDU response len=%u sw=%04x", response_len, status_word);
    size_t total = CCID_HEADER_SIZE + response_len + 2;
    if (total > cap) {
        return 0;
    }
    s_ccid_card_active = true;
    ccid_write_header(o, CCID_RDR_TO_PC_DATABLOCK, response_len + 2, slot, seq);
    memcpy(o + CCID_HEADER_SIZE, data, response_len);
    o[CCID_HEADER_SIZE + response_len] = (uint8_t)(status_word >> 8);
    o[CCID_HEADER_SIZE + response_len + 1] = (uint8_t)status_word;
    return total;
}

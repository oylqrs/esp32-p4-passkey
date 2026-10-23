#include "setup_ui.h"

#include <dirent.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "admin.h"
#include "canokey_esp32p4.h"
#include "canokey_nfc.h"
#include "device-config.h"
#include "esp_err.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "chameleon_uart/chameleon_commands.h"
#include "chameleon_uart/chameleon_ble_hid.h"
#include "browser_autofill/browser_pairing.h"
#include "display_idle.h"
#include "date_time_setting_ui.h"
#include "firmware_update.h"
#include "hide_menu_item_ui.h"
#include "ip_setting_ui.h"
#include "timezone_setup_ui.h"
#include "ui_usb_vid_setup.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lcd/lcd_driver.h"
#include "lvgl.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "storage_operation_guard.h"
#include "ui/i18n/ui_i18n.h"
#include "ui_nav.h"
#include "ui_password_manager.h"
#include "ui_status_bar.h"
#include "usb_ccid.h"
#include "usb_hid_keyboard.h"
#include "hid_keyboard.h"
#include "usb_nrf/USB_NRF_SWITCH.h"
#include "usb_msc.h"
#include "usb_ncm/usb_ncm.h"

#define SETUP_UI_WIDTH  480
#define SETUP_UI_HEIGHT 640
#define DEVICE_PASSWORD_MAX 12
#define DEVICE_PASSWORD_DEFAULT "1234"
#define DEVICE_PASSWORD_NVS_NAMESPACE "device_auth"
#define DEVICE_PASSWORD_NVS_KEY "pin"
#define DISPLAY_SETTINGS_NVS_NAMESPACE "display_ui"
#define DISPLAY_CUSTOM_BG_NVS_KEY "custom_bg"
#define CONNECT_SETTINGS_NVS_NAMESPACE "connect_ui"
#define CONNECT_TRUST_BROWSER_NVS_KEY "trust_browser"
#define CONNECT_HID_LAYOUT_NVS_KEY "hid_layout"
#define CONNECT_HID_OUTPUT_NVS_KEY "hid_output"
#define CONNECT_NEWLINE_NVS_KEY "newline"
#define CONNECT_PW_ENTER_NVS_KEY "pw_enter"
#define CONNECT_HID_SPEED_NVS_KEY "hid_speed"
#define CONNECT_QR_XFER_NVS_KEY "qr_xfer"
#define CONNECT_NFC_XFER_NVS_KEY "nfc_xfer"
#define CONNECT_XFER_ALL_NVS_KEY "xfer_all"
#define SYSTEM_SETTINGS_NVS_NAMESPACE "system_ui"
#define SYSTEM_BATTERY_SAVE_NVS_KEY "battery_save"
#define SYSTEM_AUTO_ADJUST_NVS_KEY "auto_adjust"
#define SYSTEM_LOCK_CLOCK_NVS_KEY "lock_clock"
#define NRF_SETTINGS_NVS_NAMESPACE "nrf_ui"
#define NRF_MODE_NVS_KEY "mode"
#define NRF_CHAMELEON_EMU_NVS_KEY "cham_emu"
#define NRF_CHAMELEON_LOCK_NVS_KEY "cham_lock"
#define NRF_LOCK_POLL_NVS_KEY "lock_poll"
#define SECURITY_SETTINGS_NVS_NAMESPACE "security_ui"
#define SECURITY_MODE_NVS_KEY "mode"
#define SECURITY_EMERGENCY_WAIT_NVS_KEY "emerg_wait"
#define SECURITY_PIN_LIMIT_NVS_KEY "pin_limit"
#define SECURITY_PIN_MASKING_NVS_KEY "pin_mask"
#define SECURITY_PIN_SCRAMBLE_NVS_KEY "pin_scramble"
#define SECURITY_HIDE_PASSWORD_NVS_KEY "hide_password"
#define RESTORE_CLEAR_CARDS_NVS_KEY "clear_cards"
#define RESTORE_FORMAT_USB_NVS_KEY "format_usb"
#define RESTORE_CLEAR_WALLET_NVS_KEY "clear_wallet"
#define SC_SETTINGS_NVS_NAMESPACE "sc_ui"
#define SC_OPENPGP_ON_NVS_KEY "openpgp_on"
#define SC_PIV_ON_NVS_KEY "piv_on"
#define SC_NDEF_WP_NVS_KEY "ndef_wp"
#define USB_NCM_SWITCH_TASK_STACK 4096
#define USB_NCM_SWITCH_TASK_PRIORITY 4
#define USB_DISK_SWITCH_TASK_STACK 4096
#define USB_DISK_SWITCH_TASK_PRIORITY 4
#define TEMPERATURE_TASK_STACK 3072
#define TEMPERATURE_TASK_PRIORITY 4
#define TEMPERATURE_MIN_INTERVAL_US (5LL * 1000 * 1000)
#define FACTORY_RESET_TASK_STACK 4096
#define FACTORY_RESET_TASK_PRIORITY 5
#define FACTORY_RESET_CLEAR_CARDS (1U << 0)
#define FACTORY_RESET_FORMAT_USB  (1U << 1)
#define FACTORY_RESET_CLEAR_WALLET (1U << 2)

static const uint16_t s_display_timeout_options[] = {
    120, 60, 30, 20, 10, DISPLAY_IDLE_TIMEOUT_DISABLED_SECONDS
};

static const char *TAG = "setup_ui";
static lv_obj_t *s_password_status_label;
static lv_obj_t *s_password_fields[3];
static uint8_t s_password_active_field;
static lv_obj_t *s_usb_ncm_switch;
static lv_timer_t *s_usb_ncm_switch_timer;
static atomic_bool s_usb_ncm_switch_done;
static esp_err_t s_usb_ncm_switch_result;
static bool s_usb_ncm_switch_target;
static lv_obj_t *s_usb_disk_switch;
static lv_timer_t *s_usb_disk_switch_timer;
static atomic_bool s_usb_disk_switch_done;
static esp_err_t s_usb_disk_switch_result;
static bool s_usb_disk_switch_target;
static lv_obj_t *s_trust_browser_switch;
static lv_obj_t *s_about_temperature_value;
static lv_timer_t *s_temperature_timer;
static atomic_bool s_temperature_task_running;
static atomic_bool s_temperature_read_done;
static esp_err_t s_temperature_read_result;
static int16_t s_temperature_read_quarters;
static int16_t s_temperature_cached_quarters;
static int64_t s_temperature_last_request_us;
static bool s_temperature_cache_valid;
static lv_obj_t *s_factory_reset_switch;
static lv_obj_t *s_restore_clear_cards_switch;
static lv_obj_t *s_restore_format_usb_switch;
static lv_obj_t *s_restore_clear_wallet_switch;
static lv_obj_t *s_restore_dialog;
static lv_obj_t *s_restore_pin_textarea;
static lv_obj_t *s_restore_status_label;
static atomic_bool s_factory_reset_running;

typedef struct {
    lv_obj_t *status;
    lv_obj_t *partition;
    lv_obj_t *version;
    lv_obj_t *progress;
    lv_obj_t *hash;
    lv_obj_t *start_button;
    lv_obj_t *button_label;
    lv_timer_t *timer;
} firmware_update_ui_t;

static void firmware_update_timer_cb(lv_timer_t *timer)
{
    firmware_update_ui_t *ui = lv_timer_get_user_data(timer);
    firmware_update_status_t update;
    firmware_update_get_status(&update);

    const char *text = "Ready";
    lv_color_t color = lv_color_hex(0x9EA3AD);
    bool busy = false;
    switch (update.state) {
    case FIRMWARE_UPDATE_MOUNTING: text = "Mounting USB storage..."; busy = true; break;
    case FIRMWARE_UPDATE_VERIFYING: text = "Verifying image..."; busy = true; break;
    case FIRMWARE_UPDATE_REJECTED_SAME_VERSION: text = "Same version - update rejected"; color = lv_color_hex(0xF27A7A); break;
    case FIRMWARE_UPDATE_VERIFIED: text = "Verification passed"; color = lv_color_hex(0xA2ED8B); break;
    case FIRMWARE_UPDATE_WRITING: text = "Installing..."; busy = true; break;
    case FIRMWARE_UPDATE_COMPLETED: text = "Completed - restarting..."; color = lv_color_hex(0xA2ED8B); break;
    case FIRMWARE_UPDATE_FAILED: text = "Update failed"; color = lv_color_hex(0xF27A7A); break;
    default: break;
    }
    if (storage_operation_get() == STORAGE_OPERATION_KDBX_LOAD) {
        text = "Database loading...";
        busy = true;
    }
    if (busy) color = lv_color_hex(0xFFC958);

    lv_label_set_text(ui->status, text);
    lv_obj_set_style_text_color(ui->status, color, 0);
    lv_label_set_text_fmt(ui->partition, "Partition: %s -> %s",
                          update.running_partition, update.target_partition);
    lv_label_set_text_fmt(ui->version, "Version: %s", update.version[0] ? update.version : "-");
    unsigned percent = update.image_size ? (unsigned)(update.bytes_written * 100 / update.image_size) : 0;
    lv_label_set_text_fmt(ui->progress, "%u%%  (%u / %u bytes)", percent,
                          (unsigned)update.bytes_written, (unsigned)update.image_size);
    if (update.sha256[0]) {
        lv_label_set_text_fmt(ui->hash, "SHA-256:\n%.32s\n%.32s",
                              update.sha256, update.sha256 + 32);
    } else {
        lv_label_set_text(ui->hash, "SHA-256: -");
    }
    lv_label_set_text(ui->button_label,
                      update.state == FIRMWARE_UPDATE_VERIFIED ? "Install" : "Verify");
    if (busy || update.state == FIRMWARE_UPDATE_COMPLETED) lv_obj_add_state(ui->start_button, LV_STATE_DISABLED);
    else lv_obj_clear_state(ui->start_button, LV_STATE_DISABLED);
}

static void firmware_update_ui_delete_cb(lv_event_t *event)
{
    firmware_update_ui_t *ui = lv_event_get_user_data(event);
    if (ui->timer) lv_timer_delete(ui->timer);
    lv_free(ui);
}

static void firmware_update_start_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) return;
    firmware_update_ui_t *ui = lv_event_get_user_data(event);
    firmware_update_status_t update;
    firmware_update_get_status(&update);
    esp_err_t ret = update.state == FIRMWARE_UPDATE_VERIFIED
                        ? firmware_update_install()
                        : firmware_update_verify();
    if (ret != ESP_OK) {
        lv_label_set_text_fmt(ui->status, "Start failed: %s", esp_err_to_name(ret));
        lv_obj_set_style_text_color(ui->status, lv_color_hex(0xF27A7A), 0);
    }
    firmware_update_timer_cb(ui->timer);
}

static void firmware_update_page_show(void)
{
    lv_obj_t *screen = lv_screen_active();
    lv_obj_clean(screen);
    ui_nav_enable_right_swipe(screen);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x11141A), 0);
    lv_obj_set_style_pad_all(screen, 0, 0);

    lv_obj_t *title = lv_label_create(screen);
    lv_label_set_text(title, ui_i18n_get(UI_STR_FIRMWARE_UPDATE));
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_set_style_text_font(title, ui_i18n_font_32(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 25, 32);

    lv_obj_t *panel = lv_obj_create(screen);
    lv_obj_set_size(panel, SETUP_UI_WIDTH - 50, 460);
    lv_obj_align(panel, LV_ALIGN_TOP_MID, 0, 94);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x292A32), 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_style_radius(panel, 6, 0);
    lv_obj_set_style_pad_all(panel, 20, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

    firmware_update_ui_t *ui = lv_malloc(sizeof(*ui));
    if (!ui) { lv_obj_delete(panel); return; }

    lv_obj_t *path = lv_label_create(panel);
    lv_label_set_text(path, FIRMWARE_UPDATE_IMAGE_PATTERN);
    lv_obj_set_width(path, SETUP_UI_WIDTH - 90);
    lv_label_set_long_mode(path, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(path, lv_color_hex(0xD5D8DE), 0);
    lv_obj_set_style_text_font(path, &lv_font_montserrat_24, 0);
    lv_obj_align(path, LV_ALIGN_TOP_LEFT, 0, 0);

    ui->status = lv_label_create(panel);
    lv_obj_set_style_text_font(ui->status, &lv_font_montserrat_28, 0);
    lv_obj_align(ui->status, LV_ALIGN_TOP_LEFT, 0, 54);
    ui->partition = lv_label_create(panel);
    lv_obj_set_style_text_color(ui->partition, lv_color_hex(0xD5D8DE), 0);
    lv_obj_set_style_text_font(ui->partition, &lv_font_montserrat_24, 0);
    lv_obj_align(ui->partition, LV_ALIGN_TOP_LEFT, 0, 108);
    ui->version = lv_label_create(panel);
    lv_obj_set_style_text_color(ui->version, lv_color_hex(0xD5D8DE), 0);
    lv_obj_set_style_text_font(ui->version, &lv_font_montserrat_24, 0);
    lv_obj_align(ui->version, LV_ALIGN_TOP_LEFT, 0, 146);
    ui->progress = lv_label_create(panel);
    lv_obj_set_width(ui->progress, SETUP_UI_WIDTH - 90);
    lv_obj_set_style_text_color(ui->progress, lv_color_hex(0xD5D8DE), 0);
    lv_obj_set_style_text_font(ui->progress, &lv_font_montserrat_24, 0);
    lv_obj_align(ui->progress, LV_ALIGN_TOP_LEFT, 0, 184);
    ui->hash = lv_label_create(panel);
    lv_obj_set_width(ui->hash, SETUP_UI_WIDTH - 90);
    lv_obj_set_style_text_color(ui->hash, lv_color_hex(0xD5D8DE), 0);
    lv_obj_set_style_text_font(ui->hash, &lv_font_montserrat_16, 0);
    lv_obj_align(ui->hash, LV_ALIGN_TOP_LEFT, 0, 228);

    ui->start_button = lv_button_create(panel);
    lv_obj_set_size(ui->start_button, SETUP_UI_WIDTH - 90, 72);
    lv_obj_align(ui->start_button, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(ui->start_button, lv_color_hex(0x3A3C46), 0);
    lv_obj_set_style_bg_color(ui->start_button, lv_color_hex(0x4A4D59), LV_STATE_PRESSED);
    lv_obj_set_style_radius(ui->start_button, 5, 0);
    ui->button_label = lv_label_create(ui->start_button);
    lv_label_set_text(ui->button_label, "Verify");
    lv_obj_set_style_text_font(ui->button_label, &lv_font_montserrat_28, 0);
    lv_obj_center(ui->button_label);

    ui->timer = lv_timer_create(firmware_update_timer_cb, 100, ui);
    lv_obj_add_event_cb(ui->start_button, firmware_update_start_cb, LV_EVENT_CLICKED, ui);
    lv_obj_add_event_cb(panel, firmware_update_ui_delete_cb, LV_EVENT_DELETE, ui);
    firmware_update_timer_cb(ui->timer);
}

static esp_err_t setup_nvs_ready(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    return ret;
}

bool setup_ui_get_custom_background(void)
{
    uint8_t enabled = 0;
    if (setup_nvs_ready() != ESP_OK) {
        return false;
    }

    nvs_handle_t nvs;
    if (nvs_open(DISPLAY_SETTINGS_NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        return false;
    }
    (void)nvs_get_u8(nvs, DISPLAY_CUSTOM_BG_NVS_KEY, &enabled);
    nvs_close(nvs);
    return enabled != 0;
}

static esp_err_t write_display_setting_u8(const char *key, uint8_t value)
{
    esp_err_t ret = setup_nvs_ready();
    if (ret != ESP_OK) {
        return ret;
    }

    nvs_handle_t nvs;
    ret = nvs_open(DISPLAY_SETTINGS_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = nvs_set_u8(nvs, key, value);
    if (ret == ESP_OK) {
        ret = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return ret;
}

static uint8_t read_connect_setting_u8(const char *key, uint8_t default_value)
{
    if (setup_nvs_ready() != ESP_OK) {
        return default_value;
    }

    nvs_handle_t nvs;
    if (nvs_open(CONNECT_SETTINGS_NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        return default_value;
    }
    uint8_t value = default_value;
    (void)nvs_get_u8(nvs, key, &value);
    nvs_close(nvs);
    return value;
}

hid_keyboard_transport_t setup_ui_get_hid_transport(void)
{
    uint8_t transport = read_connect_setting_u8(
        CONNECT_HID_OUTPUT_NVS_KEY, HID_KEYBOARD_TRANSPORT_USB);
    if (transport >= HID_KEYBOARD_TRANSPORT_COUNT) {
        transport = HID_KEYBOARD_TRANSPORT_USB;
    }
    return (hid_keyboard_transport_t)transport;
}

static esp_err_t write_connect_setting_u8(const char *key, uint8_t value)
{
    esp_err_t ret = setup_nvs_ready();
    if (ret != ESP_OK) {
        return ret;
    }

    nvs_handle_t nvs;
    ret = nvs_open(CONNECT_SETTINGS_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = nvs_set_u8(nvs, key, value);
    if (ret == ESP_OK) {
        ret = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return ret;
}

static uint8_t read_system_setting_u8(const char *key, uint8_t default_value)
{
    if (setup_nvs_ready() != ESP_OK) {
        return default_value;
    }

    nvs_handle_t nvs;
    if (nvs_open(SYSTEM_SETTINGS_NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        return default_value;
    }
    uint8_t value = default_value;
    (void)nvs_get_u8(nvs, key, &value);
    nvs_close(nvs);
    return value;
}

setup_lock_clock_mode_t setup_ui_get_lock_clock_mode(void)
{
    uint8_t mode = read_system_setting_u8(SYSTEM_LOCK_CLOCK_NVS_KEY,
                                          SETUP_LOCK_CLOCK_HM_YMD);
    if (mode >= SETUP_LOCK_CLOCK_MODE_COUNT) {
        return SETUP_LOCK_CLOCK_HM_YMD;
    }
    return (setup_lock_clock_mode_t)mode;
}

static esp_err_t write_system_setting_u8(const char *key, uint8_t value)
{
    esp_err_t ret = setup_nvs_ready();
    if (ret != ESP_OK) {
        return ret;
    }

    nvs_handle_t nvs;
    ret = nvs_open(SYSTEM_SETTINGS_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = nvs_set_u8(nvs, key, value);
    if (ret == ESP_OK) {
        ret = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return ret;
}

static uint8_t read_nrf_setting_u8(const char *key, uint8_t default_value)
{
    if (setup_nvs_ready() != ESP_OK) {
        return default_value;
    }

    nvs_handle_t nvs;
    if (nvs_open(NRF_SETTINGS_NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        return default_value;
    }
    uint8_t value = default_value;
    (void)nvs_get_u8(nvs, key, &value);
    nvs_close(nvs);
    return value;
}

static esp_err_t write_nrf_setting_u8(const char *key, uint8_t value)
{
    esp_err_t ret = setup_nvs_ready();
    if (ret != ESP_OK) {
        return ret;
    }

    nvs_handle_t nvs;
    ret = nvs_open(NRF_SETTINGS_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (ret == ESP_OK) {
        ret = nvs_set_u8(nvs, key, value);
        if (ret == ESP_OK) ret = nvs_commit(nvs);
        nvs_close(nvs);
    }
    return ret;
}

setup_nrf_mode_t setup_ui_get_nrf_mode(void)
{
    uint8_t mode = read_nrf_setting_u8(NRF_MODE_NVS_KEY, SETUP_NRF_MODE_OFF);
    if (mode >= SETUP_NRF_MODE_COUNT) return SETUP_NRF_MODE_OFF;
    return (setup_nrf_mode_t)mode;
}

static uint8_t read_security_setting_u8(const char *key, uint8_t default_value)
{
    if (setup_nvs_ready() != ESP_OK) {
        return default_value;
    }

    nvs_handle_t nvs;
    if (nvs_open(SECURITY_SETTINGS_NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        return default_value;
    }
    uint8_t value = default_value;
    (void)nvs_get_u8(nvs, key, &value);
    nvs_close(nvs);
    return value;
}

static esp_err_t write_security_setting_u8(const char *key, uint8_t value)
{
    esp_err_t ret = setup_nvs_ready();
    if (ret != ESP_OK) {
        return ret;
    }

    nvs_handle_t nvs;
    ret = nvs_open(SECURITY_SETTINGS_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (ret == ESP_OK) {
        ret = nvs_set_u8(nvs, key, value);
        if (ret == ESP_OK) ret = nvs_commit(nvs);
        nvs_close(nvs);
    }
    return ret;
}

setup_security_mode_t setup_ui_get_security_mode(void)
{
    static bool latched;
    static setup_security_mode_t mode = SETUP_SECURITY_MODE_NORMAL;
    if (!latched) {
        uint8_t stored = read_security_setting_u8(SECURITY_MODE_NVS_KEY,
                                                  SETUP_SECURITY_MODE_NORMAL);
        if (stored < SETUP_SECURITY_MODE_COUNT) {
            mode = (setup_security_mode_t)stored;
        }
        latched = true;
    }
    return mode;
}

uint8_t setup_ui_get_pin_error_limit(void)
{
    static const uint8_t limits[] = {5, 10, 20};
    uint8_t selected = read_security_setting_u8(SECURITY_PIN_LIMIT_NVS_KEY, 0);
    return selected < sizeof(limits) ? limits[selected] : limits[0];
}

bool setup_ui_get_pin_masking(void)
{
    return read_security_setting_u8(SECURITY_PIN_MASKING_NVS_KEY, 1) != 0;
}

bool setup_ui_get_pin_scramble(void)
{
    return read_security_setting_u8(SECURITY_PIN_SCRAMBLE_NVS_KEY, 0) != 0;
}

bool setup_ui_get_hide_password(void)
{
    return read_security_setting_u8(SECURITY_HIDE_PASSWORD_NVS_KEY, 0) != 0;
}

static uint8_t read_sc_setting_u8(const char *key, uint8_t default_value)
{
    if (setup_nvs_ready() != ESP_OK) {
        return default_value;
    }

    nvs_handle_t nvs;
    if (nvs_open(SC_SETTINGS_NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        return default_value;
    }
    uint8_t value = default_value;
    (void)nvs_get_u8(nvs, key, &value);
    nvs_close(nvs);
    return value;
}

static esp_err_t write_sc_setting_u8(const char *key, uint8_t value)
{
    esp_err_t ret = setup_nvs_ready();
    if (ret != ESP_OK) {
        return ret;
    }

    nvs_handle_t nvs;
    ret = nvs_open(SC_SETTINGS_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = nvs_set_u8(nvs, key, value);
    if (ret == ESP_OK) {
        ret = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return ret;
}

static bool sync_sc_ccid_settings(void)
{
    if (!canokey_esp32p4_is_ready()) {
        return false;
    }

    uint8_t openpgp_enabled = read_sc_setting_u8(SC_OPENPGP_ON_NVS_KEY, 1);
    uint8_t piv_enabled = read_sc_setting_u8(SC_PIV_ON_NVS_KEY, 1);
    if (device_config_is_openpgp_ccid_enabled() == openpgp_enabled &&
        device_config_is_piv_ccid_enabled() == piv_enabled) {
        return true;
    }
    if (admin_device_config_set_ccid_enabled(openpgp_enabled, piv_enabled) < 0) {
        return false;
    }

    usb_ccid_deactivate_card();
    if (openpgp_enabled || piv_enabled) {
        (void)usb_ccid_activate_card();
    }
    return true;
}

static esp_err_t read_device_password(char *out, size_t out_len)
{
    if (!out || out_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    strlcpy(out, DEVICE_PASSWORD_DEFAULT, out_len);

    esp_err_t ret = setup_nvs_ready();
    if (ret != ESP_OK) {
        return ret;
    }

    nvs_handle_t nvs;
    ret = nvs_open(DEVICE_PASSWORD_NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (ret != ESP_OK) {
        return ret;
    }

    size_t len = out_len;
    ret = nvs_get_str(nvs, DEVICE_PASSWORD_NVS_KEY, out, &len);
    nvs_close(nvs);
    return ret == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : ret;
}

static esp_err_t write_device_password(const char *pin)
{
    if (!pin || pin[0] == '\0' || strlen(pin) > DEVICE_PASSWORD_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = setup_nvs_ready();
    if (ret != ESP_OK) {
        return ret;
    }

    nvs_handle_t nvs;
    ret = nvs_open(DEVICE_PASSWORD_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = nvs_set_str(nvs, DEVICE_PASSWORD_NVS_KEY, pin);
    if (ret == ESP_OK) {
        ret = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return ret;
}

bool setup_ui_device_password_matches(const char *pin)
{
    char stored[DEVICE_PASSWORD_MAX + 1];
    esp_err_t ret = read_device_password(stored, sizeof(stored));
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to read device password: %s", esp_err_to_name(ret));
        strlcpy(stored, DEVICE_PASSWORD_DEFAULT, sizeof(stored));
    }
    return pin && strcmp(pin, stored) == 0;
}

static void backlight_slider_event_cb(lv_event_t *event)
{
    lv_obj_t *slider = lv_event_get_target(event);
    uint8_t percent = (uint8_t)lv_slider_get_value(slider);

    if (lv_event_get_code(event) == LV_EVENT_VALUE_CHANGED) {
        lcd_driver_set_backlight(percent);
    } else if (lv_event_get_code(event) == LV_EVENT_RELEASED) {
        esp_err_t ret = lcd_driver_save_backlight_percent(percent);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to save backlight level: %s", esp_err_to_name(ret));
        }
    }
}

static void language_apply_async(void *user_data)
{
    ui_language_t language = (ui_language_t)(uintptr_t)user_data;
    esp_err_t ret = ui_i18n_set_language(language);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to save UI language: %s", esp_err_to_name(ret));
    }
    setup_ui_show();
}

static void language_dropdown_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_VALUE_CHANGED) {
        return;
    }

    lv_obj_t *dropdown = lv_event_get_target(event);
    ui_language_t language = lv_dropdown_get_selected(dropdown) == 0
                                 ? UI_LANGUAGE_ENGLISH
                                 : UI_LANGUAGE_CHINESE;
    lv_obj_add_state(dropdown, LV_STATE_DISABLED);
    if (lv_async_call(language_apply_async, (void *)(uintptr_t)language) != LV_RESULT_OK) {
        lv_obj_clear_state(dropdown, LV_STATE_DISABLED);
        ESP_LOGW(TAG, "Failed to schedule UI language change");
    }
}

static void add_backlight_slider(lv_obj_t *parent)
{
    static const lv_point_precise_t sun_rays[][2] = {
        {{18, 2}, {18, 7}}, {{18, 29}, {18, 34}},
        {{2, 18}, {7, 18}}, {{29, 18}, {34, 18}},
        {{7, 7}, {10, 10}}, {{26, 26}, {29, 29}},
        {{7, 29}, {10, 26}}, {{26, 10}, {29, 7}},
    };
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, SETUP_UI_WIDTH - 50, 96);
    lv_obj_set_style_bg_color(row, lv_color_hex(0x292A32), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(row, 4, 0);
    lv_obj_set_style_pad_all(row, 0, 0);

    lv_obj_t *icon = lv_obj_create(row);
    lv_obj_remove_style_all(icon);
    lv_obj_set_size(icon, 36, 36);
    lv_obj_align(icon, LV_ALIGN_LEFT_MID, 18, 0);
    lv_obj_t *sun_center = lv_obj_create(icon);
    lv_obj_remove_style_all(sun_center);
    lv_obj_set_size(sun_center, 13, 13);
    lv_obj_set_style_radius(sun_center, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(sun_center, lv_color_hex(0xD5D8DE), 0);
    lv_obj_set_style_bg_opa(sun_center, LV_OPA_COVER, 0);
    lv_obj_center(sun_center);
    for (size_t i = 0; i < sizeof(sun_rays) / sizeof(sun_rays[0]); i++) {
        lv_obj_t *ray = lv_line_create(icon);
        lv_line_set_points(ray, sun_rays[i], 2);
        lv_obj_set_style_line_color(ray, lv_color_hex(0xD5D8DE), 0);
        lv_obj_set_style_line_width(ray, 3, 0);
        lv_obj_set_style_line_rounded(ray, true, 0);
    }

    lv_obj_t *slider = lv_slider_create(row);
    lv_obj_set_size(slider, 340, 24);  //亮度条粗细
    lv_obj_align(slider, LV_ALIGN_LEFT_MID, 62, 0);
    lv_slider_set_range(slider, LCD_BACKLIGHT_MIN_PERCENT, LCD_BACKLIGHT_MAX_PERCENT);
    uint8_t percent = lcd_driver_get_backlight_percent();
    lv_slider_set_value(slider, percent, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(slider, lv_color_hex(0x3A3C46), LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider, lv_color_hex(0x6DD6F2), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, lv_color_white(), LV_PART_KNOB);
    lv_obj_set_style_width(slider, 28, LV_PART_KNOB);
    lv_obj_set_style_height(slider, 28, LV_PART_KNOB);
    lv_obj_add_event_cb(slider, backlight_slider_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(slider, backlight_slider_event_cb, LV_EVENT_RELEASED, NULL);
}

static uint16_t timeout_option_index(uint16_t seconds)
{
    for (uint16_t i = 0; i < sizeof(s_display_timeout_options) /
                               sizeof(s_display_timeout_options[0]); i++) {
        if (s_display_timeout_options[i] == seconds) {
            return i;
        }
    }
    return 0;
}

static void timeout_dropdown_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_VALUE_CHANGED) {
        return;
    }

    uint16_t index = lv_dropdown_get_selected(lv_event_get_target(event));
    if (index >= sizeof(s_display_timeout_options) /
                 sizeof(s_display_timeout_options[0])) {
        return;
    }
    esp_err_t ret =
        display_idle_set_timeout_seconds(s_display_timeout_options[index]);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to apply display timeout: %s", esp_err_to_name(ret));
    }
}

static void add_timeout_dropdown(lv_obj_t *parent)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, SETUP_UI_WIDTH - 50, 96);
    lv_obj_set_style_bg_color(row, lv_color_hex(0x292A32), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(row, 4, 0);
    lv_obj_set_style_pad_all(row, 0, 0);

    lv_obj_t *label = lv_label_create(row);
    lv_label_set_text(label, ui_i18n_get(UI_STR_TIMEOUT));
    lv_obj_set_width(label, 230);
    lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_set_style_text_font(label, ui_i18n_font_28(), 0);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, 20, 0);

    lv_obj_t *dropdown = lv_dropdown_create(row);
    lv_dropdown_set_options(dropdown, "120s\n60s\n30s\n20s\n10s\nnone");
    lv_dropdown_set_selected(
        dropdown, timeout_option_index(display_idle_get_timeout_seconds()));
    lv_obj_set_size(dropdown, 142, 58);
    lv_obj_align(dropdown, LV_ALIGN_RIGHT_MID, -16, 0);
    lv_obj_set_style_bg_color(dropdown, lv_color_hex(0x3A3C46), 0);
    lv_obj_set_style_border_width(dropdown, 0, 0);
    lv_obj_set_style_radius(dropdown, 4, 0);
    lv_obj_set_style_text_color(dropdown, lv_color_white(), 0);
    lv_obj_set_style_text_font(dropdown, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_font(lv_dropdown_get_list(dropdown), &lv_font_montserrat_24, 0);
    lv_obj_add_event_cb(dropdown, timeout_dropdown_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
}

static void custom_background_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_VALUE_CHANGED) {
        return;
    }

    lv_obj_t *sw = lv_event_get_target(event);
    uint8_t enabled = lv_obj_has_state(sw, LV_STATE_CHECKED) ? 1 : 0;
    esp_err_t ret = write_display_setting_u8(DISPLAY_CUSTOM_BG_NVS_KEY, enabled);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to save custom background setting: %s", esp_err_to_name(ret));
    }
}

static void add_custom_background_switch(lv_obj_t *parent)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, SETUP_UI_WIDTH - 50, 96);
    lv_obj_set_style_bg_color(row, lv_color_hex(0x292A32), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(row, 4, 0);
    lv_obj_set_style_pad_all(row, 0, 0);

    lv_obj_t *label = lv_label_create(row);
    lv_label_set_text(label, ui_i18n_get(UI_STR_CUSTOM_BACKGROUND));
    lv_obj_set_width(label, SETUP_UI_WIDTH - 190);
    lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_set_style_text_font(label, ui_i18n_font_28(), 0);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, 20, 0);

    lv_obj_t *sw = lv_switch_create(row);
    lv_obj_set_size(sw, 82, 44);
    lv_obj_align(sw, LV_ALIGN_RIGHT_MID, -18, 0);
    lv_obj_set_style_bg_color(sw, lv_color_hex(0x3A3C46), LV_PART_MAIN);
    lv_obj_set_style_bg_color(sw, lv_color_hex(0x2997E8),
                              LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(sw, lv_color_white(), LV_PART_KNOB);
    if (setup_ui_get_custom_background()) {
        lv_obj_add_state(sw, LV_STATE_CHECKED);
    }
    lv_obj_add_event_cb(sw, custom_background_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
}

static void add_setup_section_label(lv_obj_t *parent, const char *text)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_size(label, SETUP_UI_WIDTH - 50, 44);
    lv_obj_set_style_text_color(label, lv_color_hex(0x9EA3AD), 0);
    lv_obj_set_style_text_font(label, ui_i18n_font_28(), 0);
    lv_obj_set_style_pad_left(label, 2, 0);
}

static void usb_disk_switch_task(void *arg)
{
    bool enabled = (bool)(uintptr_t)arg;
    s_usb_disk_switch_result = enabled ? usb_hid_enable_usbstore()
                                       : usb_hid_disable_usbstore();
    atomic_store_explicit(&s_usb_disk_switch_done, true, memory_order_release);
    vTaskDelete(NULL);
}

static void usb_disk_switch_timer_cb(lv_timer_t *timer)
{
    if (!atomic_load_explicit(&s_usb_disk_switch_done, memory_order_acquire)) {
        return;
    }

    if (s_usb_disk_switch && lv_obj_is_valid(s_usb_disk_switch)) {
        if (usb_hid_usbstore_is_exposed_to_usb()) {
            lv_obj_add_state(s_usb_disk_switch, LV_STATE_CHECKED);
        } else {
            lv_obj_clear_state(s_usb_disk_switch, LV_STATE_CHECKED);
        }
        lv_obj_clear_state(s_usb_disk_switch, LV_STATE_DISABLED);
    }
    if (s_usb_disk_switch_result != ESP_OK) {
        ESP_LOGE(TAG, "Failed to switch USB MSC: %s",
                 esp_err_to_name(s_usb_disk_switch_result));
    }

    atomic_store_explicit(&s_usb_disk_switch_done, false, memory_order_relaxed);
    s_usb_disk_switch_timer = NULL;
    lv_timer_delete(timer);
}

static void usb_disk_switch_delete_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_DELETE &&
        lv_event_get_target(event) == s_usb_disk_switch) {
        s_usb_disk_switch = NULL;
    }
}

static void usb_disk_switch_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_VALUE_CHANGED || s_usb_disk_switch_timer) {
        return;
    }

    lv_obj_t *sw = lv_event_get_target(event);
    s_usb_disk_switch_target = lv_obj_has_state(sw, LV_STATE_CHECKED);
    lv_obj_add_state(sw, LV_STATE_DISABLED);
    atomic_store_explicit(&s_usb_disk_switch_done, false, memory_order_relaxed);

    s_usb_disk_switch_timer = lv_timer_create(usb_disk_switch_timer_cb, 50, NULL);
    if (!s_usb_disk_switch_timer) {
        lv_obj_clear_state(sw, LV_STATE_DISABLED);
        ESP_LOGE(TAG, "Failed to create USB MSC switch timer");
        return;
    }

    BaseType_t created = xTaskCreate(usb_disk_switch_task,
                                     "usb_msc_switch",
                                     USB_DISK_SWITCH_TASK_STACK,
                                     (void *)(uintptr_t)s_usb_disk_switch_target,
                                     USB_DISK_SWITCH_TASK_PRIORITY,
                                     NULL);
    if (created != pdPASS) {
        lv_timer_delete(s_usb_disk_switch_timer);
        s_usb_disk_switch_timer = NULL;
        lv_obj_clear_state(sw, LV_STATE_DISABLED);
        if (usb_hid_usbstore_is_exposed_to_usb()) {
            lv_obj_add_state(sw, LV_STATE_CHECKED);
        } else {
            lv_obj_clear_state(sw, LV_STATE_CHECKED);
        }
        ESP_LOGE(TAG, "Failed to start USB MSC switch task");
    }
}

static void add_usb_disk_switch(lv_obj_t *parent)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, SETUP_UI_WIDTH - 50, 116);
    lv_obj_set_style_bg_color(row, lv_color_hex(0x292A32), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(row, 4, 0);
    lv_obj_set_style_pad_all(row, 0, 0);

    lv_obj_t *label = lv_label_create(row);
    lv_label_set_text(label, ui_i18n_get(UI_STR_USB_DISK));
    lv_obj_set_width(label, SETUP_UI_WIDTH - 190);
    lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_set_style_text_font(label, ui_i18n_font_28(), 0);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, 20, 0);

    lv_obj_t *sw = lv_switch_create(row);
    s_usb_disk_switch = sw;
    lv_obj_set_size(sw, 82, 44);
    lv_obj_align(sw, LV_ALIGN_RIGHT_MID, -18, 0);
    lv_obj_set_style_bg_color(sw, lv_color_hex(0x3A3C46), LV_PART_MAIN);
    lv_obj_set_style_bg_color(sw, lv_color_hex(0xA2ED8B), LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(sw, lv_color_white(), LV_PART_KNOB);
    if (usb_hid_usbstore_is_exposed_to_usb()) {
        lv_obj_add_state(sw, LV_STATE_CHECKED);
    } else if (s_usb_disk_switch_timer && s_usb_disk_switch_target) {
        lv_obj_add_state(sw, LV_STATE_CHECKED);
    }
    if (s_usb_disk_switch_timer) {
        lv_obj_add_state(sw, LV_STATE_DISABLED);
    }
    lv_obj_add_event_cb(sw, usb_disk_switch_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(sw, usb_disk_switch_delete_cb, LV_EVENT_DELETE, NULL);
}

static lv_obj_t *create_connect_row(lv_obj_t *parent, int32_t height)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, SETUP_UI_WIDTH - 50, height);
    lv_obj_set_style_bg_color(row, lv_color_hex(0x292A32), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    return row;
}

static lv_obj_t *add_connect_row_label(lv_obj_t *row, const char *text)
{
    lv_obj_t *label = lv_label_create(row);
    lv_label_set_text(label, text);
    lv_obj_set_width(label, 270);
    lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_set_style_text_font(label, ui_i18n_font_28(), 0);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, 20, 0);
    return label;
}

static void connect_switch_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_VALUE_CHANGED) {
        return;
    }

    const char *key = lv_event_get_user_data(event);
    lv_obj_t *sw = lv_event_get_target(event);
    uint8_t enabled = lv_obj_has_state(sw, LV_STATE_CHECKED) ? 1 : 0;
    esp_err_t ret = write_connect_setting_u8(key, enabled);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to save connect switch %s: %s", key, esp_err_to_name(ret));
    }
}

static lv_obj_t *add_connect_switch(lv_obj_t *parent,
                                    const char *text,
                                    const char *key,
                                    bool default_enabled)
{
    lv_obj_t *row = create_connect_row(parent, 88);
    add_connect_row_label(row, text);

    lv_obj_t *sw = lv_switch_create(row);
    lv_obj_set_size(sw, 82, 44);
    lv_obj_align(sw, LV_ALIGN_RIGHT_MID, -18, 0);
    lv_obj_set_style_bg_color(sw, lv_color_hex(0x3A3C46), LV_PART_MAIN);
    lv_obj_set_style_bg_color(sw, lv_color_hex(0x2997E8),
                              LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(sw, lv_color_white(), LV_PART_KNOB);
    if (read_connect_setting_u8(key, default_enabled ? 1 : 0) != 0) {
        lv_obj_add_state(sw, LV_STATE_CHECKED);
    }
    lv_obj_add_event_cb(sw, connect_switch_event_cb, LV_EVENT_VALUE_CHANGED, (void *)key);
    return sw;
}

static void connect_dropdown_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_VALUE_CHANGED) {
        return;
    }

    const char *key = lv_event_get_user_data(event);
    uint8_t selected = (uint8_t)lv_dropdown_get_selected(lv_event_get_target(event));
    if (strcmp(key, CONNECT_HID_LAYOUT_NVS_KEY) == 0) {
        esp_err_t layout_ret = usb_hid_keyboard_set_layout(
            (hid_keyboard_layout_t)selected);
        if (layout_ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to switch keyboard layout: %s",
                     esp_err_to_name(layout_ret));
            return;
        }
    } else if (strcmp(key, CONNECT_HID_OUTPUT_NVS_KEY) == 0) {
        esp_err_t transport_ret = hid_keyboard_set_transport(
            (hid_keyboard_transport_t)selected);
        if (transport_ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to switch HID output: %s",
                     esp_err_to_name(transport_ret));
            return;
        }
    }
    esp_err_t ret = write_connect_setting_u8(key, selected);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to save connect option %s: %s", key, esp_err_to_name(ret));
    }
}

static uint16_t dropdown_option_count(const char *options)
{
    uint16_t count = 1;
    for (const char *cursor = options; *cursor; cursor++) {
        if (*cursor == '\n') {
            count++;
        }
    }
    return count;
}

static void add_connect_dropdown(lv_obj_t *parent,
                                 const char *text,
                                 const char *options,
                                 const char *key,
                                 uint8_t default_index)
{
    lv_obj_t *row = create_connect_row(parent, 88);
    add_connect_row_label(row, text);

    lv_obj_t *dropdown = lv_dropdown_create(row);
    lv_dropdown_set_options(dropdown, options);
    uint8_t selected = read_connect_setting_u8(key, default_index);
    if (selected >= dropdown_option_count(options)) {
        selected = default_index;
    }
    if (strcmp(key, CONNECT_HID_LAYOUT_NVS_KEY) == 0) {
        if (selected >= HID_LAYOUT_COUNT) {
            selected = HID_LAYOUT_US;
        }
        (void)usb_hid_keyboard_set_layout((hid_keyboard_layout_t)selected);
    } else if (strcmp(key, CONNECT_HID_OUTPUT_NVS_KEY) == 0) {
        if (selected >= HID_KEYBOARD_TRANSPORT_COUNT) {
            selected = HID_KEYBOARD_TRANSPORT_USB;
        }
        (void)hid_keyboard_set_transport((hid_keyboard_transport_t)selected);
    }
    lv_dropdown_set_selected(dropdown, selected);
    lv_obj_set_size(dropdown,
                    strcmp(key, CONNECT_HID_LAYOUT_NVS_KEY) == 0 ? 220 : 164,
                    58);
    lv_obj_align(dropdown, LV_ALIGN_RIGHT_MID, -14, 0);
    lv_obj_set_style_bg_color(dropdown, lv_color_hex(0x292A32), 0);
    lv_obj_set_style_border_width(dropdown, 0, 0);
    lv_obj_set_style_radius(dropdown, 0, 0);
    lv_obj_set_style_text_color(dropdown, lv_color_hex(0xD5D8DE), 0);
    lv_obj_set_style_text_font(dropdown, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_font(lv_dropdown_get_list(dropdown), &lv_font_montserrat_24, 0);
    lv_obj_add_event_cb(dropdown, connect_dropdown_event_cb,
                        LV_EVENT_VALUE_CHANGED, (void *)key);
}

static lv_obj_t *add_connect_action(lv_obj_t *parent,
                                    const char *text,
                                    const char *button_text,
                                    lv_event_cb_t event_cb)
{
    lv_obj_t *row = create_connect_row(parent, 88);
    add_connect_row_label(row, text);

    lv_obj_t *button = lv_button_create(row);
    lv_obj_set_size(button, 112, 56);
    lv_obj_align(button, LV_ALIGN_RIGHT_MID, -18, 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x2997E8), 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x197FC7), LV_STATE_PRESSED);
    lv_obj_set_style_radius(button, 6, 0);
    lv_obj_set_style_shadow_width(button, 0, 0);
    lv_obj_add_event_cb(button, event_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *label = lv_label_create(button);
    lv_label_set_text(label, button_text);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_set_style_text_font(label, ui_i18n_font_24(), 0);
    lv_obj_center(label);
    return button;
}

static void usb_vid_setup_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_CLICKED) {
        ui_nav_push(setup_ui_show, ui_usb_vid_setup_show);
    }
}

static void nic_ip_set_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_CLICKED) {
        ui_nav_push(setup_ui_show, ip_setting_ui_show);
    }
}

static void addon_pair_reset_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }

    esp_err_t ret = browser_pairing_clear();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to reset browser trust: %s", esp_err_to_name(ret));
        return;
    }
    if (s_trust_browser_switch && lv_obj_is_valid(s_trust_browser_switch)) {
        lv_obj_clear_state(s_trust_browser_switch, LV_STATE_CHECKED);
    }
}

static void add_trust_browser_switch(lv_obj_t *parent)
{
    lv_obj_t *row = create_connect_row(parent, 88);

    add_connect_row_label(row, ui_i18n_get(UI_STR_TRUST_BROWSER));

    lv_obj_t *sw = lv_switch_create(row);
    s_trust_browser_switch = sw;
    lv_obj_set_size(sw, 82, 44);
    lv_obj_align(sw, LV_ALIGN_RIGHT_MID, -18, 0);
    lv_obj_set_style_bg_color(sw, lv_color_hex(0x3A3C46), LV_PART_MAIN);
    lv_obj_set_style_bg_color(sw, lv_color_hex(0x2997E8),
                              LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(sw, lv_color_white(), LV_PART_KNOB);
    if (read_connect_setting_u8(CONNECT_TRUST_BROWSER_NVS_KEY, 0) != 0) {
        lv_obj_add_state(sw, LV_STATE_CHECKED);
    }
    lv_obj_add_event_cb(sw, connect_switch_event_cb, LV_EVENT_VALUE_CHANGED,
                        (void *)CONNECT_TRUST_BROWSER_NVS_KEY);
}

static void system_switch_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_VALUE_CHANGED) {
        return;
    }

    const char *key = lv_event_get_user_data(event);
    lv_obj_t *sw = lv_event_get_target(event);
    uint8_t enabled = lv_obj_has_state(sw, LV_STATE_CHECKED) ? 1 : 0;
    esp_err_t ret = write_system_setting_u8(key, enabled);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to save system switch %s: %s", key, esp_err_to_name(ret));
    }
}

static void add_system_switch(lv_obj_t *parent,
                              const char *text,
                              const char *key,
                              bool default_enabled)
{
    lv_obj_t *row = create_connect_row(parent, 88);
    add_connect_row_label(row, text);

    lv_obj_t *sw = lv_switch_create(row);
    lv_obj_set_size(sw, 82, 44);
    lv_obj_align(sw, LV_ALIGN_RIGHT_MID, -18, 0);
    lv_obj_set_style_bg_color(sw, lv_color_hex(0x3A3C46), LV_PART_MAIN);
    lv_obj_set_style_bg_color(sw, lv_color_hex(0x2997E8),
                              LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(sw, lv_color_white(), LV_PART_KNOB);
    if (read_system_setting_u8(key, default_enabled ? 1 : 0) != 0) {
        lv_obj_add_state(sw, LV_STATE_CHECKED);
    }
    lv_obj_add_event_cb(sw, system_switch_event_cb, LV_EVENT_VALUE_CHANGED, (void *)key);
}

static void system_dropdown_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_VALUE_CHANGED) {
        return;
    }

    const char *key = lv_event_get_user_data(event);
    uint8_t selected = (uint8_t)lv_dropdown_get_selected(lv_event_get_target(event));
    esp_err_t ret = write_system_setting_u8(key, selected);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to save system option %s: %s", key, esp_err_to_name(ret));
    }
}

static void add_system_dropdown(lv_obj_t *parent,
                                const char *text,
                                const char *options,
                                const char *key,
                                uint8_t default_index)
{
    lv_obj_t *row = create_connect_row(parent, 88);
    add_connect_row_label(row, text);

    lv_obj_t *dropdown = lv_dropdown_create(row);
    lv_dropdown_set_options(dropdown, options);
    uint8_t selected = read_system_setting_u8(key, default_index);
    if (selected >= dropdown_option_count(options)) {
        selected = default_index;
    }
    lv_dropdown_set_selected(dropdown, selected);
    lv_obj_set_size(dropdown, 164, 58);
    lv_obj_align(dropdown, LV_ALIGN_RIGHT_MID, -14, 0);
    lv_obj_set_style_bg_color(dropdown, lv_color_hex(0x292A32), 0);
    lv_obj_set_style_border_width(dropdown, 0, 0);
    lv_obj_set_style_radius(dropdown, 0, 0);
    lv_obj_set_style_text_color(dropdown, lv_color_hex(0xD5D8DE), 0);
    lv_obj_set_style_text_font(dropdown, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_font(lv_dropdown_get_list(dropdown), &lv_font_montserrat_24, 0);
    lv_obj_add_event_cb(dropdown, system_dropdown_event_cb,
                        LV_EVENT_VALUE_CHANGED, (void *)key);
}

static void add_system_language_dropdown(lv_obj_t *parent)
{
    lv_obj_t *row = create_connect_row(parent, 88);
    add_connect_row_label(row, ui_i18n_get(UI_STR_LANGUAGE));

    lv_obj_t *dropdown = lv_dropdown_create(row);
    lv_dropdown_set_options(dropdown, "en\ncn");
    lv_dropdown_set_selected(dropdown,
                             ui_i18n_get_language() == UI_LANGUAGE_CHINESE ? 1 : 0);
    lv_obj_set_size(dropdown, 164, 58);
    lv_obj_align(dropdown, LV_ALIGN_RIGHT_MID, -14, 0);
    lv_obj_set_style_bg_color(dropdown, lv_color_hex(0x292A32), 0);
    lv_obj_set_style_border_width(dropdown, 0, 0);
    lv_obj_set_style_radius(dropdown, 0, 0);
    lv_obj_set_style_text_color(dropdown, lv_color_hex(0xD5D8DE), 0);
    lv_obj_set_style_text_font(dropdown, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_font(lv_dropdown_get_list(dropdown), &lv_font_montserrat_24, 0);
    lv_obj_add_event_cb(dropdown, language_dropdown_event_cb,
                        LV_EVENT_VALUE_CHANGED, NULL);
}

static void reload_database_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }
    ui_password_manager_reload();
    ESP_LOGI(TAG, "Database reload requested");
}

static void timezone_setting_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_CLICKED) {
        ui_nav_push(setup_ui_show, timezone_setup_ui_show);
    }
}

static void date_time_setting_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_CLICKED) {
        ui_nav_push(setup_ui_show, date_time_setting_ui_show);
    }
}

static void hide_menu_item_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_CLICKED) {
        ui_nav_push(setup_ui_show, hide_menu_item_ui_show);
    }
}

static void sc_switch_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_VALUE_CHANGED) {
        return;
    }

    const char *key = lv_event_get_user_data(event);
    lv_obj_t *sw = lv_event_get_target(event);
    uint8_t enabled = lv_obj_has_state(sw, LV_STATE_CHECKED) ? 1 : 0;
    uint8_t previous = read_sc_setting_u8(key, enabled ? 0 : 1);
    esp_err_t ret = write_sc_setting_u8(key, enabled);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to save SC switch %s: %s", key, esp_err_to_name(ret));
        if (previous) {
            lv_obj_add_state(sw, LV_STATE_CHECKED);
        } else {
            lv_obj_remove_state(sw, LV_STATE_CHECKED);
        }
        return;
    }

    if (strcmp(key, SC_OPENPGP_ON_NVS_KEY) != 0 &&
        strcmp(key, SC_PIV_ON_NVS_KEY) != 0) {
        return;
    }

    if (!canokey_esp32p4_is_ready()) {
        ESP_LOGE(TAG, "Cannot update CCID feature configuration: CanoKey is not ready");
        (void)write_sc_setting_u8(key, previous);
        if (previous) {
            lv_obj_add_state(sw, LV_STATE_CHECKED);
        } else {
            lv_obj_remove_state(sw, LV_STATE_CHECKED);
        }
        return;
    }

    if (!sync_sc_ccid_settings()) {
        ESP_LOGE(TAG, "Failed to update CanoKey CCID feature configuration");
        (void)write_sc_setting_u8(key, previous);
        if (previous) {
            lv_obj_add_state(sw, LV_STATE_CHECKED);
        } else {
            lv_obj_remove_state(sw, LV_STATE_CHECKED);
        }
        return;
    }

}

static void add_sc_switch(lv_obj_t *parent,
                          const char *text,
                          const char *key,
                          bool default_enabled)
{
    lv_obj_t *row = create_connect_row(parent, 88);
    add_connect_row_label(row, text);

    lv_obj_t *sw = lv_switch_create(row);
    lv_obj_set_size(sw, 82, 44);
    lv_obj_align(sw, LV_ALIGN_RIGHT_MID, -18, 0);
    lv_obj_set_style_bg_color(sw, lv_color_hex(0x3A3C46), LV_PART_MAIN);
    lv_obj_set_style_bg_color(sw, lv_color_hex(0x2997E8),
                              LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(sw, lv_color_white(), LV_PART_KNOB);
    if (read_sc_setting_u8(key, default_enabled ? 1 : 0) != 0) {
        lv_obj_add_state(sw, LV_STATE_CHECKED);
    }
    lv_obj_add_event_cb(sw, sc_switch_event_cb, LV_EVENT_VALUE_CHANGED, (void *)key);
}

static void sc_reset_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_CLICKED) {
        ESP_LOGW(TAG, "SC reset requested; no erase backend is registered");
    }
}

static lv_obj_t *add_about_info_row(lv_obj_t *parent,
                                    const char *label_text,
                                    const char *value_text)
{
    lv_obj_t *row = create_connect_row(parent, 72);

    lv_obj_t *label = lv_label_create(row);
    lv_label_set_text(label, label_text);
    lv_obj_set_width(label, 180);
    lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_color(label, lv_color_hex(0xD5D8DE), 0);
    lv_obj_set_style_text_font(label, ui_i18n_font_28(), 0);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, 20, 0);

    lv_obj_t *value = lv_label_create(row);
    lv_label_set_text(value, value_text && value_text[0] ? value_text : "-");
    lv_obj_set_width(value, 210);
    lv_label_set_long_mode(value, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(value, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_color(value, lv_color_hex(0xE9EBEF), 0);
    lv_obj_set_style_text_font(value, &lv_font_montserrat_24, 0);
    lv_obj_align(value, LV_ALIGN_RIGHT_MID, -18, 0);
    return value;
}

static void set_about_temperature_text(int16_t quarter_degrees)
{
    if (!s_about_temperature_value || !lv_obj_is_valid(s_about_temperature_value)) {
        return;
    }

    int32_t value = quarter_degrees;
    bool negative = value < 0;
    int32_t magnitude = negative ? -value : value;
    int32_t whole = magnitude / 4;
    int32_t hundredths = (magnitude % 4) * 25;
    if (hundredths == 0) {
        lv_label_set_text_fmt(s_about_temperature_value, "%s%ld \xC2\xB0" "C",
                              negative ? "-" : "", (long)whole);
    } else {
        lv_label_set_text_fmt(s_about_temperature_value, "%s%ld.%02ld \xC2\xB0" "C",
                              negative ? "-" : "", (long)whole,
                              (long)hundredths);
    }
}

static void temperature_read_task(void *arg)
{
    (void)arg;
    uint16_t device_status = 0;
    int16_t quarter_degrees = 0;
    s_temperature_read_result =
        chameleon_get_temperature(&quarter_degrees, &device_status);
    s_temperature_read_quarters = quarter_degrees;
    if (s_temperature_read_result != ESP_OK) {
        ESP_LOGW(TAG, "Failed to read nRF52840 temperature: %s, status=0x%04x",
                 esp_err_to_name(s_temperature_read_result), device_status);
    }
    atomic_store_explicit(&s_temperature_read_done, true, memory_order_release);
    atomic_store_explicit(&s_temperature_task_running, false, memory_order_release);
    vTaskDelete(NULL);
}

static void temperature_timer_cb(lv_timer_t *timer)
{
    if (!atomic_load_explicit(&s_temperature_read_done, memory_order_acquire)) {
        return;
    }

    if (s_temperature_read_result == ESP_OK) {
        s_temperature_cached_quarters = s_temperature_read_quarters;
        s_temperature_cache_valid = true;
        set_about_temperature_text(s_temperature_cached_quarters);
    } else {
        s_temperature_cache_valid = false;
        if (s_about_temperature_value && lv_obj_is_valid(s_about_temperature_value)) {
            lv_label_set_text(s_about_temperature_value, "-");
        }
    }

    atomic_store_explicit(&s_temperature_read_done, false, memory_order_relaxed);
    s_temperature_timer = NULL;
    lv_timer_delete(timer);
}

static void start_temperature_read(void)
{
    if (s_temperature_timer ||
        atomic_load_explicit(&s_temperature_task_running, memory_order_acquire)) {
        return;
    }

    int64_t now = esp_timer_get_time();
    if (s_temperature_last_request_us != 0 &&
        now - s_temperature_last_request_us < TEMPERATURE_MIN_INTERVAL_US) {
        if (s_temperature_cache_valid) {
            set_about_temperature_text(s_temperature_cached_quarters);
        }
        return;
    }

    bool expected = false;
    if (!atomic_compare_exchange_strong_explicit(&s_temperature_task_running,
                                                  &expected, true,
                                                  memory_order_acq_rel,
                                                  memory_order_acquire)) {
        return;
    }

    s_temperature_last_request_us = now;
    atomic_store_explicit(&s_temperature_read_done, false, memory_order_relaxed);
    s_temperature_timer = lv_timer_create(temperature_timer_cb, 50, NULL);
    if (!s_temperature_timer) {
        atomic_store_explicit(&s_temperature_task_running, false, memory_order_release);
        ESP_LOGE(TAG, "Failed to create temperature result timer");
        return;
    }

    BaseType_t created = xTaskCreate(temperature_read_task,
                                     "nrf_temperature",
                                     TEMPERATURE_TASK_STACK,
                                     NULL,
                                     TEMPERATURE_TASK_PRIORITY,
                                     NULL);
    if (created != pdPASS) {
        lv_timer_delete(s_temperature_timer);
        s_temperature_timer = NULL;
        atomic_store_explicit(&s_temperature_task_running, false, memory_order_release);
        ESP_LOGE(TAG, "Failed to start temperature read task");
    }
}

static void open_source_licenses_show(void)
{
    lv_obj_t *screen = lv_screen_active();
    lv_obj_clean(screen);
    ui_nav_enable_right_swipe(screen);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x11141A), 0);
    lv_obj_set_style_pad_all(screen, 0, 0);

    ui_status_bar_add(screen, SETUP_UI_WIDTH);

    lv_obj_t *title = lv_label_create(screen);
    lv_label_set_text(title, ui_i18n_get(UI_STR_OPEN_SOURCE_LICENSES));
    lv_obj_set_width(title, SETUP_UI_WIDTH - 50);
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_set_style_text_font(title, ui_i18n_font_28(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 25, 72);

    lv_obj_t *panel = lv_obj_create(screen);
    lv_obj_remove_style_all(panel);
    lv_obj_set_size(panel, SETUP_UI_WIDTH - 50, 490);
    lv_obj_align(panel, LV_ALIGN_TOP_MID, 0, 130);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x292A32), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(panel, 4, 0);
    lv_obj_set_style_pad_all(panel, 20, 0);
    lv_obj_add_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(panel, LV_DIR_VER);

    lv_obj_t *licenses = lv_label_create(panel);
    lv_label_set_text(licenses,
                      "MiixKEY firmware\n\n"
                      "LVGL - MIT License\n\n"
                      "ESP-IDF - Apache License 2.0\n\n"
                      "TinyUSB - MIT License\n\n"
                      "CanoKey - Apache License 2.0\n\n"
                      "Full license texts are included in the source tree.");
    lv_obj_set_width(licenses, SETUP_UI_WIDTH - 90);
    lv_label_set_long_mode(licenses, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(licenses, lv_color_hex(0xE0E2E7), 0);
    lv_obj_set_style_text_font(licenses, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_line_space(licenses, 8, 0);
}

static void open_source_licenses_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_CLICKED) {
        ui_nav_push(setup_ui_show, open_source_licenses_show);
    }
}

static void about_firmware_upgrade_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_CLICKED) {
        ui_nav_push(setup_ui_show, firmware_update_page_show);
    }
}

static void add_about_group(lv_obj_t *parent)
{
    lv_obj_t *group = lv_obj_create(parent);
    lv_obj_remove_style_all(group);
    lv_obj_set_size(group, SETUP_UI_WIDTH - 50, 696);
    lv_obj_set_style_pad_all(group, 0, 0);
    lv_obj_set_style_pad_row(group, 0, 0);
    lv_obj_set_flex_flow(group, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(group, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *brand = create_connect_row(group, 88);
    lv_obj_t *brand_label = lv_label_create(brand);
    lv_label_set_text(brand_label, "MiixKEY");
    lv_obj_set_style_text_color(brand_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(brand_label, &lv_font_montserrat_28, 0);
    lv_obj_center(brand_label);

    const esp_app_desc_t *app = esp_app_get_description();
    add_about_info_row(group, ui_i18n_get(UI_STR_SOFTWARE),
                       app && app->version[0] ? app->version : "-");
    add_about_info_row(group, ui_i18n_get(UI_STR_HARDWARE), "1.01");
    add_about_info_row(group, ui_i18n_get(UI_STR_RESCUE), "-");
    s_about_temperature_value =
        add_about_info_row(group, ui_i18n_get(UI_STR_TEMPERATURE), "-");
    char ip_address[USB_NCM_IP_ADDRESS_LEN];
    usb_ncm_get_ip_address(ip_address);
    add_about_info_row(group, ui_i18n_get(UI_STR_IP_ADDRESS), ip_address);
    lv_obj_t *website_value =
        add_about_info_row(group, ui_i18n_get(UI_STR_WEBSITE), "www.miixpro.com");
    lv_obj_set_width(website_value, 260);
    lv_obj_set_style_text_align(website_value, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_align(website_value, LV_ALIGN_LEFT_MID, 150, 0);
    add_connect_action(group, ui_i18n_get(UI_STR_OPEN_SOURCE_LICENSES),
                       ui_i18n_get(UI_STR_READ), open_source_licenses_event_cb);
    add_connect_action(group, ui_i18n_get(UI_STR_FIRMWARE_UPGRADE),
                       ui_i18n_get(UI_STR_UPGRADE), about_firmware_upgrade_event_cb);
}

static void usb_ncm_switch_task(void *arg)
{
    bool enabled = (bool)(uintptr_t)arg;
    s_usb_ncm_switch_result = enabled ? usb_hid_enable_ncm() : usb_hid_disable_ncm();
    atomic_store_explicit(&s_usb_ncm_switch_done, true, memory_order_release);
    vTaskDelete(NULL);
}

static void usb_ncm_switch_timer_cb(lv_timer_t *timer)
{
    if (!atomic_load_explicit(&s_usb_ncm_switch_done, memory_order_acquire)) {
        return;
    }

    if (s_usb_ncm_switch && lv_obj_is_valid(s_usb_ncm_switch)) {
        if (usb_hid_ncm_is_enabled()) {
            lv_obj_add_state(s_usb_ncm_switch, LV_STATE_CHECKED);
        } else {
            lv_obj_clear_state(s_usb_ncm_switch, LV_STATE_CHECKED);
        }
        lv_obj_clear_state(s_usb_ncm_switch, LV_STATE_DISABLED);
    }
    if (s_usb_ncm_switch_result != ESP_OK) {
        ESP_LOGE(TAG, "Failed to switch USB NCM: %s",
                 esp_err_to_name(s_usb_ncm_switch_result));
    }

    atomic_store_explicit(&s_usb_ncm_switch_done, false, memory_order_relaxed);
    s_usb_ncm_switch_timer = NULL;
    lv_timer_delete(timer);
}

static void usb_ncm_switch_delete_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_DELETE &&
        lv_event_get_target(event) == s_usb_ncm_switch) {
        s_usb_ncm_switch = NULL;
    }
}

static void usb_ncm_switch_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_VALUE_CHANGED || s_usb_ncm_switch_timer) {
        return;
    }

    lv_obj_t *sw = lv_event_get_target(event);
    s_usb_ncm_switch_target = lv_obj_has_state(sw, LV_STATE_CHECKED);
    lv_obj_add_state(sw, LV_STATE_DISABLED);
    atomic_store_explicit(&s_usb_ncm_switch_done, false, memory_order_relaxed);

    s_usb_ncm_switch_timer = lv_timer_create(usb_ncm_switch_timer_cb, 50, NULL);
    if (!s_usb_ncm_switch_timer) {
        lv_obj_clear_state(sw, LV_STATE_DISABLED);
        ESP_LOGE(TAG, "Failed to create USB NCM switch timer");
        return;
    }

    BaseType_t created = xTaskCreate(usb_ncm_switch_task,
                                     "usb_ncm_switch",
                                     USB_NCM_SWITCH_TASK_STACK,
                                     (void *)(uintptr_t)s_usb_ncm_switch_target,
                                     USB_NCM_SWITCH_TASK_PRIORITY,
                                     NULL);
    if (created != pdPASS) {
        lv_timer_delete(s_usb_ncm_switch_timer);
        s_usb_ncm_switch_timer = NULL;
        lv_obj_clear_state(sw, LV_STATE_DISABLED);
        if (usb_hid_ncm_is_enabled()) {
            lv_obj_add_state(sw, LV_STATE_CHECKED);
        } else {
            lv_obj_clear_state(sw, LV_STATE_CHECKED);
        }
        ESP_LOGE(TAG, "Failed to start USB NCM switch task");
    }
}

static void add_usb_ncm_switch(lv_obj_t *parent)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, SETUP_UI_WIDTH - 50, 88);
    lv_obj_set_style_bg_color(row, lv_color_hex(0x292A32), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(row, 4, 0);
    lv_obj_set_style_pad_all(row, 0, 0);

    lv_obj_t *label = lv_label_create(row);
    lv_label_set_text(label, ui_i18n_get(UI_STR_USB_NCM));
    lv_obj_set_width(label, SETUP_UI_WIDTH - 190);
    lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_set_style_text_font(label, ui_i18n_font_28(), 0);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, 20, 0);

    lv_obj_t *sw = lv_switch_create(row);
    s_usb_ncm_switch = sw;
    lv_obj_set_size(sw, 82, 44);
    lv_obj_align(sw, LV_ALIGN_RIGHT_MID, -18, 0);
    lv_obj_set_style_bg_color(sw, lv_color_hex(0x3A3C46), LV_PART_MAIN);
    lv_obj_set_style_bg_color(sw, lv_color_hex(0xA2ED8B), LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(sw, lv_color_white(), LV_PART_KNOB);
    if (usb_hid_ncm_is_enabled()) {
        lv_obj_add_state(sw, LV_STATE_CHECKED);
    } else if (s_usb_ncm_switch_timer && s_usb_ncm_switch_target) {
        lv_obj_add_state(sw, LV_STATE_CHECKED);
    }
    if (s_usb_ncm_switch_timer) {
        lv_obj_add_state(sw, LV_STATE_DISABLED);
    }
    lv_obj_add_event_cb(sw, usb_ncm_switch_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(sw, usb_ncm_switch_delete_cb, LV_EVENT_DELETE, NULL);
}

static void nrf_mode_switch_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_VALUE_CHANGED) {
        return;
    }

    lv_obj_t *sw = lv_event_get_target(event);
    bool enabled = lv_obj_has_state(sw, LV_STATE_CHECKED);
    esp_err_t ret = usb_nrf_switch_set(enabled);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set NRF mode GPIO: %s", esp_err_to_name(ret));
        if (usb_nrf_switch_is_enabled()) {
            lv_obj_add_state(sw, LV_STATE_CHECKED);
        } else {
            lv_obj_clear_state(sw, LV_STATE_CHECKED);
        }
    }
}

static void add_usb_to_nrf_switch(lv_obj_t *parent)
{
    lv_obj_t *row = create_connect_row(parent, 88);
    add_connect_row_label(row, ui_i18n_get(UI_STR_SWITCH_USB_TO_NRF));

    lv_obj_t *sw = lv_switch_create(row);
    lv_obj_set_size(sw, 82, 44);
    lv_obj_align(sw, LV_ALIGN_RIGHT_MID, -18, 0);
    lv_obj_set_style_bg_color(sw, lv_color_hex(0x3A3C46), LV_PART_MAIN);
    lv_obj_set_style_bg_color(sw, lv_color_hex(0xA2ED8B), LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(sw, lv_color_white(), LV_PART_KNOB);
    esp_err_t ret = usb_nrf_switch_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize NRF mode GPIO: %s", esp_err_to_name(ret));
        lv_obj_add_state(sw, LV_STATE_DISABLED);
    } else if (usb_nrf_switch_is_enabled()) {
        lv_obj_add_state(sw, LV_STATE_CHECKED);
    }
    lv_obj_add_event_cb(sw, nrf_mode_switch_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
}

static void nrf_dropdown_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_VALUE_CHANGED) {
        return;
    }

    const char *key = lv_event_get_user_data(event);
    uint8_t selected = (uint8_t)lv_dropdown_get_selected(lv_event_get_target(event));
    esp_err_t ret = write_nrf_setting_u8(key, selected);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to save NRF option %s: %s", key, esp_err_to_name(ret));
        return;
    }
    if (strcmp(key, NRF_MODE_NVS_KEY) == 0) {
        canokey_nfc_set_enabled(selected == SETUP_NRF_MODE_NFC_BT);
        bool hid_enabled = selected == SETUP_NRF_MODE_NFC_BT ||
                           selected == SETUP_NRF_MODE_BT_PAIRING;
        bool pairing_allowed = selected == SETUP_NRF_MODE_BT_PAIRING;
        chameleon_ble_hid_set_mode(hid_enabled, pairing_allowed);
        ESP_LOGI(TAG, "NRF mode=%u, NFC bridge=%u BLE HID=%u pairing=%u",
                 selected, selected == SETUP_NRF_MODE_NFC_BT ? 1 : 0,
                 hid_enabled ? 1 : 0, pairing_allowed ? 1 : 0);
        if (pairing_allowed) {
            char pairing_key[CHAMELEON_BLE_PAIRING_KEY_LENGTH + 1] = {0};
            char message[64];
            uint16_t device_status = 0;
            esp_err_t key_ret = chameleon_get_ble_pairing_key(
                pairing_key, &device_status);
            lv_obj_t *dialog = lv_msgbox_create(NULL);
            lv_msgbox_add_title(dialog, ui_i18n_get(UI_STR_BLE_PAIRING));
            if (key_ret == ESP_OK) {
                snprintf(message, sizeof(message),
                         ui_i18n_get(UI_STR_BLE_PAIRING_KEY), pairing_key);
                lv_msgbox_add_text(dialog, message);
            } else {
                ESP_LOGW(TAG,
                         "Failed to read BLE pairing key: %s, status=0x%04x",
                         esp_err_to_name(key_ret), device_status);
                lv_msgbox_add_text(
                    dialog, ui_i18n_get(UI_STR_BLE_PAIRING_KEY_UNAVAILABLE));
            }
            lv_msgbox_add_close_button(dialog);
            lv_obj_set_width(dialog, SETUP_UI_WIDTH - 48);
            lv_obj_set_style_text_font(dialog, ui_i18n_font_28(), 0);
            lv_obj_center(dialog);
        }
    }
}

static void add_nrf_dropdown(lv_obj_t *parent,
                             const char *text,
                             const char *options,
                             const char *key,
                             uint8_t default_index)
{
    lv_obj_t *row = create_connect_row(parent, 88);
    add_connect_row_label(row, text);

    lv_obj_t *dropdown = lv_dropdown_create(row);
    lv_dropdown_set_options(dropdown, options);
    uint8_t selected = read_nrf_setting_u8(key, default_index);
    if (selected >= dropdown_option_count(options)) {
        selected = default_index;
    }
    lv_dropdown_set_selected(dropdown, selected);
    lv_obj_set_size(dropdown, 180, 58);
    lv_obj_align(dropdown, LV_ALIGN_RIGHT_MID, -14, 0);
    lv_obj_set_style_bg_color(dropdown, lv_color_hex(0x292A32), 0);
    lv_obj_set_style_border_width(dropdown, 0, 0);
    lv_obj_set_style_radius(dropdown, 0, 0);
    lv_obj_set_style_text_color(dropdown, lv_color_hex(0xD5D8DE), 0);
    lv_obj_set_style_text_font(dropdown, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_font(lv_dropdown_get_list(dropdown), &lv_font_montserrat_24, 0);
    lv_obj_add_event_cb(dropdown, nrf_dropdown_event_cb,
                        LV_EVENT_VALUE_CHANGED, (void *)key);
}

static void nrf_setting_switch_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_VALUE_CHANGED) {
        return;
    }

    const char *key = lv_event_get_user_data(event);
    lv_obj_t *sw = lv_event_get_target(event);
    uint8_t enabled = lv_obj_has_state(sw, LV_STATE_CHECKED) ? 1 : 0;
    esp_err_t ret = write_nrf_setting_u8(key, enabled);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to save NRF switch %s: %s", key, esp_err_to_name(ret));
    }
}

static void nrf_chameleon_emu_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_VALUE_CHANGED) {
        return;
    }

    lv_obj_t *sw = lv_event_get_target(event);
    bool enabled = lv_obj_has_state(sw, LV_STATE_CHECKED);
    uint16_t device_status = 0;
    esp_err_t ret = chameleon_change_mode(enabled ? CHAMELEON_MODE_TAG
                                                   : CHAMELEON_MODE_READER,
                                           &device_status);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to switch Chameleon emulation: %s, status=0x%04x",
                 esp_err_to_name(ret), device_status);
        if (enabled) {
            lv_obj_clear_state(sw, LV_STATE_CHECKED);
        } else {
            lv_obj_add_state(sw, LV_STATE_CHECKED);
        }
        return;
    }

    ret = write_nrf_setting_u8(NRF_CHAMELEON_EMU_NVS_KEY, enabled ? 1 : 0);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to save Chameleon emulation state: %s",
                 esp_err_to_name(ret));
    }
}

static lv_obj_t *add_nrf_switch(lv_obj_t *parent,
                                const char *text,
                                const char *key,
                                bool default_enabled,
                                lv_event_cb_t event_cb)
{
    lv_obj_t *row = create_connect_row(parent, 88);
    add_connect_row_label(row, text);

    lv_obj_t *sw = lv_switch_create(row);
    lv_obj_set_size(sw, 82, 44);
    lv_obj_align(sw, LV_ALIGN_RIGHT_MID, -18, 0);
    lv_obj_set_style_bg_color(sw, lv_color_hex(0x3A3C46), LV_PART_MAIN);
    lv_obj_set_style_bg_color(sw, lv_color_hex(0x2997E8),
                              LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(sw, lv_color_white(), LV_PART_KNOB);
    if (read_nrf_setting_u8(key, default_enabled ? 1 : 0) != 0) {
        lv_obj_add_state(sw, LV_STATE_CHECKED);
    }
    lv_obj_add_event_cb(sw, event_cb, LV_EVENT_VALUE_CHANGED, (void *)key);
    return sw;
}

static void add_nrf_settings_group(lv_obj_t *parent)
{
    lv_obj_t *group = lv_obj_create(parent);
    lv_obj_remove_style_all(group);
    lv_obj_set_size(group, SETUP_UI_WIDTH - 50, 88 * 5);
    lv_obj_set_style_pad_all(group, 0, 0);
    lv_obj_set_style_pad_row(group, 0, 0);
    lv_obj_set_flex_flow(group, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(group, LV_OBJ_FLAG_SCROLLABLE);

    add_nrf_dropdown(group, ui_i18n_get(UI_STR_NRF_MODE),
                     "off\nNFC+BT\nChameleon\nBT-Pairing\nUpgrade",
                     NRF_MODE_NVS_KEY, 0);
    add_usb_to_nrf_switch(group);
    add_nrf_switch(group, ui_i18n_get(UI_STR_CHAMELEON_EMULATION),
                   NRF_CHAMELEON_EMU_NVS_KEY, false,
                   nrf_chameleon_emu_event_cb);
    add_nrf_switch(group, ui_i18n_get(UI_STR_START_WHEN_LOCKED),
                   NRF_CHAMELEON_LOCK_NVS_KEY, false,
                   nrf_setting_switch_event_cb);
    add_nrf_dropdown(group, ui_i18n_get(UI_STR_LOCK_POLLING),
                     "5s\n10s\n20s\n30s", NRF_LOCK_POLL_NVS_KEY, 0);
}

static void password_field_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }

    s_password_active_field = (uint8_t)(uintptr_t)lv_event_get_user_data(event);
    for (uint8_t i = 0; i < 3; i++) {
        if (s_password_fields[i]) {
            if (i == s_password_active_field) {
                lv_obj_add_state(s_password_fields[i], LV_STATE_FOCUSED);
            } else {
                lv_obj_clear_state(s_password_fields[i], LV_STATE_FOCUSED);
            }
        }
    }
}

static void update_password_field_focus(void)
{
    for (uint8_t i = 0; i < 3; i++) {
        if (s_password_fields[i]) {
            if (i == s_password_active_field) {
                lv_obj_add_state(s_password_fields[i], LV_STATE_FOCUSED);
            } else {
                lv_obj_clear_state(s_password_fields[i], LV_STATE_FOCUSED);
            }
        }
    }
}

static void set_password_status(const char *text, lv_color_t color)
{
    if (s_password_status_label) {
        lv_label_set_text(s_password_status_label, text);
        lv_obj_set_style_text_color(s_password_status_label, color, 0);
    }
}

static void save_password_from_fields(void)
{
    const char *current = lv_textarea_get_text(s_password_fields[0]);
    const char *new_pin = lv_textarea_get_text(s_password_fields[1]);
    const char *confirm = lv_textarea_get_text(s_password_fields[2]);

    if (!setup_ui_device_password_matches(current)) {
        set_password_status("Current password wrong", lv_color_hex(0xF27A7A));
        lv_textarea_set_text(s_password_fields[0], "");
        s_password_active_field = 0;
        lv_obj_add_state(s_password_fields[0], LV_STATE_FOCUSED);
        return;
    }

    if (!new_pin || new_pin[0] == '\0') {
        set_password_status("New password is empty", lv_color_hex(0xF27A7A));
        return;
    }

    if (strcmp(new_pin, confirm) != 0) {
        set_password_status("Confirm password mismatch", lv_color_hex(0xF27A7A));
        lv_textarea_set_text(s_password_fields[2], "");
        s_password_active_field = 2;
        lv_obj_add_state(s_password_fields[2], LV_STATE_FOCUSED);
        return;
    }

    esp_err_t ret = write_device_password(new_pin);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to save device password: %s", esp_err_to_name(ret));
        set_password_status("Save failed", lv_color_hex(0xF27A7A));
        return;
    }

    for (uint8_t i = 0; i < 3; i++) {
        lv_textarea_set_text(s_password_fields[i], "");
    }
    s_password_active_field = 0;
    lv_obj_add_state(s_password_fields[0], LV_STATE_FOCUSED);
    set_password_status("Password updated", lv_color_hex(0xA2ED8B));
}

static void password_keypad_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_VALUE_CHANGED) {
        return;
    }

    lv_obj_t *button_matrix = lv_event_get_target_obj(event);
    uint32_t button_id = lv_buttonmatrix_get_selected_button(button_matrix);
    const char *key = lv_buttonmatrix_get_button_text(button_matrix, button_id);
    lv_obj_t *field = s_password_fields[s_password_active_field];
    if (!key || !field) {
        return;
    }

    if (strcmp(key, LV_SYMBOL_BACKSPACE) == 0) {
        lv_textarea_delete_char(field);
    } else if (strcmp(key, LV_SYMBOL_OK) == 0) {
        save_password_from_fields();
    } else if (key[0] >= '0' && key[0] <= '9') {
        lv_textarea_add_char(field, key[0]);
        if (strlen(lv_textarea_get_text(field)) >= 4 && s_password_active_field < 2) {
            s_password_active_field++;
            update_password_field_focus();
        }
    }
}

static lv_obj_t *add_password_textarea(lv_obj_t *screen, const char *placeholder, uint8_t index, int32_t y)
{
    lv_obj_t *textarea = lv_textarea_create(screen);
    lv_obj_set_size(textarea, SETUP_UI_WIDTH - 50, 58);
    lv_obj_align(textarea, LV_ALIGN_TOP_MID, 0, y);
    lv_textarea_set_one_line(textarea, true);
    lv_textarea_set_password_mode(textarea, true);
    lv_textarea_set_max_length(textarea, DEVICE_PASSWORD_MAX);
    lv_textarea_set_placeholder_text(textarea, placeholder);
    lv_obj_add_event_cb(textarea, password_field_event_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)index);
    lv_obj_set_style_bg_color(textarea, lv_color_hex(0x2B2C34), 0);
    lv_obj_set_style_bg_color(textarea, lv_color_hex(0x343744), LV_STATE_FOCUSED);
    lv_obj_set_style_border_color(textarea, lv_color_hex(0x31333C), 0);
    lv_obj_set_style_border_color(textarea, lv_color_hex(0x6DD6F2), LV_STATE_FOCUSED);
    lv_obj_set_style_border_width(textarea, 1, 0);
    lv_obj_set_style_radius(textarea, 5, 0);
    lv_obj_set_style_text_color(textarea, lv_color_white(), 0);
    lv_obj_set_style_text_color(textarea, lv_color_hex(0x8D9099), LV_PART_TEXTAREA_PLACEHOLDER);
    lv_obj_set_style_text_font(textarea, &lv_font_montserrat_24, 0);
    lv_obj_set_style_pad_left(textarea, 16, 0);
    lv_obj_set_style_pad_right(textarea, 16, 0);
    return textarea;
}

static void setup_password_page_show(void)
{
    lv_obj_t *screen = lv_screen_active();
    lv_obj_clean(screen);
    ui_nav_enable_right_swipe(screen);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x11141A), 0);
    lv_obj_set_style_pad_all(screen, 0, 0);

    lv_obj_t *title = lv_label_create(screen);
    lv_label_set_text(title, ui_i18n_get(UI_STR_DEVICE_PASSWORD));
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_set_style_text_font(title, ui_i18n_font_28(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 25, 30);

    s_password_status_label = lv_label_create(screen);
    lv_label_set_text(s_password_status_label, "Enter current and new PIN");
    lv_obj_set_width(s_password_status_label, SETUP_UI_WIDTH - 50);
    lv_label_set_long_mode(s_password_status_label, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_color(s_password_status_label, lv_color_hex(0x9EA3AD), 0);
    lv_obj_set_style_text_font(s_password_status_label, &lv_font_montserrat_16, 0);
    lv_obj_align(s_password_status_label, LV_ALIGN_TOP_LEFT, 25, 64);

    s_password_fields[0] = add_password_textarea(screen, "Current password", 0, 96);
    s_password_fields[1] = add_password_textarea(screen, "New password", 1, 166);
    s_password_fields[2] = add_password_textarea(screen, "Confirm password", 2, 236);
    s_password_active_field = 0;
    lv_obj_add_state(s_password_fields[0], LV_STATE_FOCUSED);

    static const char *pin_map[] = {
        "1", "2", "3", "\n",
        "4", "5", "6", "\n",
        "7", "8", "9", "\n",
        LV_SYMBOL_BACKSPACE, "0", LV_SYMBOL_OK, ""
    };

    lv_obj_t *keypad = lv_buttonmatrix_create(screen);
    lv_buttonmatrix_set_map(keypad, pin_map);
    lv_obj_set_size(keypad, SETUP_UI_WIDTH - 12, 300);
    lv_obj_align(keypad, LV_ALIGN_BOTTOM_MID, 0, -4);
    lv_obj_set_style_bg_opa(keypad, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(keypad, 0, 0);
    lv_obj_set_style_pad_all(keypad, 0, 0);
    lv_obj_set_style_pad_row(keypad, 4, 0);
    lv_obj_set_style_pad_column(keypad, 4, 0);
    lv_obj_set_style_radius(keypad, 3, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(keypad, lv_color_hex(0x292A32), LV_PART_ITEMS);
    lv_obj_set_style_bg_color(keypad, lv_color_hex(0x3A3C46), LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_border_color(keypad, lv_color_hex(0x1A1B22), LV_PART_ITEMS);
    lv_obj_set_style_border_width(keypad, 1, LV_PART_ITEMS);
    lv_obj_set_style_text_color(keypad, lv_color_white(), LV_PART_ITEMS);
    lv_obj_set_style_text_font(keypad, &lv_font_montserrat_28, LV_PART_ITEMS);
    lv_obj_add_event_cb(keypad, password_keypad_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
}

static void password_menu_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_CLICKED) {
        ui_nav_push(setup_ui_show, setup_password_page_show);
    }
}

static lv_obj_t *add_security_row_label(lv_obj_t *row, const char *text)
{
    lv_obj_t *label = lv_label_create(row);
    lv_label_set_text(label, text);
    lv_obj_set_width(label, 210);
    lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_set_style_text_font(label, ui_i18n_font_28(), 0);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, 20, 0);
    return label;
}

static void security_dropdown_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_VALUE_CHANGED) {
        return;
    }

    const char *key = lv_event_get_user_data(event);
    uint8_t selected = (uint8_t)lv_dropdown_get_selected(lv_event_get_target(event));
    esp_err_t ret = write_security_setting_u8(key, selected);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to save security option %s: %s",
                 key, esp_err_to_name(ret));
    }
}

static lv_obj_t *add_security_dropdown_control(lv_obj_t *row,
                                                const char *options,
                                                const char *key,
                                                uint8_t default_index)
{
    lv_obj_t *dropdown = lv_dropdown_create(row);
    lv_dropdown_set_options(dropdown, options);
    uint8_t selected = read_security_setting_u8(key, default_index);
    if (selected >= dropdown_option_count(options)) {
        selected = default_index;
    }
    lv_dropdown_set_selected(dropdown, selected);
    lv_obj_set_size(dropdown, 180, 58);
    lv_obj_align(dropdown, LV_ALIGN_RIGHT_MID, -14, 0);
    lv_obj_set_style_bg_color(dropdown, lv_color_hex(0x292A32), 0);
    lv_obj_set_style_border_width(dropdown, 0, 0);
    lv_obj_set_style_radius(dropdown, 0, 0);
    lv_obj_set_style_text_color(dropdown, lv_color_hex(0xD5D8DE), 0);
    lv_obj_set_style_text_font(dropdown, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_font(lv_dropdown_get_list(dropdown), &lv_font_montserrat_24, 0);
    lv_obj_add_event_cb(dropdown, security_dropdown_event_cb,
                        LV_EVENT_VALUE_CHANGED, (void *)key);
    return dropdown;
}

static void add_security_dropdown(lv_obj_t *parent,
                                  const char *text,
                                  const char *options,
                                  const char *key,
                                  uint8_t default_index)
{
    lv_obj_t *row = create_connect_row(parent, 112);
    lv_obj_t *label = add_security_row_label(row, text);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_height(label, LV_SIZE_CONTENT);
    add_security_dropdown_control(row, options, key, default_index);
}

static void security_switch_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_VALUE_CHANGED) {
        return;
    }

    const char *key = lv_event_get_user_data(event);
    lv_obj_t *sw = lv_event_get_target(event);
    uint8_t enabled = lv_obj_has_state(sw, LV_STATE_CHECKED) ? 1 : 0;
    esp_err_t ret = write_security_setting_u8(key, enabled);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to save security switch %s: %s",
                 key, esp_err_to_name(ret));
    }
}

static void add_security_switch(lv_obj_t *parent,
                                const char *text,
                                const char *key,
                                bool default_enabled)
{
    lv_obj_t *row = create_connect_row(parent, 88);
    add_security_row_label(row, text);

    lv_obj_t *sw = lv_switch_create(row);
    lv_obj_set_size(sw, 82, 44);
    lv_obj_align(sw, LV_ALIGN_RIGHT_MID, -18, 0);
    lv_obj_set_style_bg_color(sw, lv_color_hex(0x3A3C46), LV_PART_MAIN);
    lv_obj_set_style_bg_color(sw, lv_color_hex(0x2997E8),
                              LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(sw, lv_color_white(), LV_PART_KNOB);
    if (read_security_setting_u8(key, default_enabled ? 1 : 0) != 0) {
        lv_obj_add_state(sw, LV_STATE_CHECKED);
    }
    lv_obj_add_event_cb(sw, security_switch_event_cb,
                        LV_EVENT_VALUE_CHANGED, (void *)key);
}

static void add_security_mode(lv_obj_t *parent)
{
    lv_obj_t *row = create_connect_row(parent, 88);
    add_security_row_label(row, ui_i18n_get(UI_STR_MIIXKEY_MODE));
    add_security_dropdown_control(row, "Normal\nEmergency\nDuress",
                                  SECURITY_MODE_NVS_KEY, 0);
}

static void add_pin_error_limit(lv_obj_t *parent)
{
    lv_obj_t *row = create_connect_row(parent, 88);
    add_security_row_label(row, ui_i18n_get(UI_STR_PIN_ERROR_LIMIT));
    add_security_dropdown_control(row, "5\n10\n20",
                                  SECURITY_PIN_LIMIT_NVS_KEY, 0);
}

static void add_security_settings_group(lv_obj_t *parent)
{
    lv_obj_t *group = lv_obj_create(parent);
    lv_obj_remove_style_all(group);
    lv_obj_set_size(group, SETUP_UI_WIDTH - 50, 640);
    lv_obj_set_style_pad_all(group, 0, 0);
    lv_obj_set_style_pad_row(group, 0, 0);
    lv_obj_set_flex_flow(group, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(group, LV_OBJ_FLAG_SCROLLABLE);

    add_security_mode(group);
    add_security_dropdown(group, ui_i18n_get(UI_STR_EMERGENCY_MODE_WAIT),
                          "12h\n24h\n48h", SECURITY_EMERGENCY_WAIT_NVS_KEY, 0);
    add_connect_action(group, ui_i18n_get(UI_STR_CHANGE_PIN),
                       ui_i18n_get(UI_STR_MODIFY), password_menu_event_cb);
    add_pin_error_limit(group);
    add_security_switch(group, ui_i18n_get(UI_STR_PIN_MASKING),
                        SECURITY_PIN_MASKING_NVS_KEY, true);
    add_security_switch(group, ui_i18n_get(UI_STR_PIN_SCRAMBLE),
                        SECURITY_PIN_SCRAMBLE_NVS_KEY, false);
    add_security_switch(group, ui_i18n_get(UI_STR_HIDE_PASSWORD),
                        SECURITY_HIDE_PASSWORD_NVS_KEY, false);
}

static bool has_case_insensitive_suffix(const char *text, const char *suffix)
{
    size_t text_len = strlen(text);
    size_t suffix_len = strlen(suffix);
    return text_len >= suffix_len &&
           strcasecmp(text + text_len - suffix_len, suffix) == 0;
}

static bool is_wallet_file(const char *name)
{
    return has_case_insensitive_suffix(name, ".kdbx") ||
           has_case_insensitive_suffix(name, ".kdb") ||
           strcasecmp(name, "config.txt") == 0 ||
           strcasecmp(name, "temp.csv") == 0;
}

static void clear_wallet_files(const char *directory, uint8_t depth)
{
    if (depth > 8) {
        return;
    }

    DIR *dir = opendir(directory);
    if (!dir) {
        ESP_LOGW(TAG, "Unable to open wallet directory %s", directory);
        return;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        char path[320];
        int written = snprintf(path, sizeof(path), "%s/%s", directory, entry->d_name);
        if (written < 0 || (size_t)written >= sizeof(path)) {
            continue;
        }

        struct stat info;
        if (stat(path, &info) != 0) {
            continue;
        }
        if (S_ISDIR(info.st_mode)) {
            clear_wallet_files(path, depth + 1);
        } else if (is_wallet_file(entry->d_name)) {
            if (remove(path) != 0) {
                ESP_LOGW(TAG, "Failed to remove wallet file %s", path);
            }
        }
    }
    closedir(dir);
}

static void factory_reset_task(void *arg)
{
    uint32_t options = (uint32_t)(uintptr_t)arg;
    bool usb_format_succeeded = false;

    if ((options & (FACTORY_RESET_FORMAT_USB | FACTORY_RESET_CLEAR_WALLET)) != 0) {
        esp_err_t ret = usb_hid_disable_usbstore();
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to switch USB storage to APP before reset: %s",
                     esp_err_to_name(ret));
        }
    }

    if ((options & FACTORY_RESET_FORMAT_USB) != 0) {
        esp_err_t ret = usb_msc_factory_format();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Factory USB format failed: %s", esp_err_to_name(ret));
        } else {
            usb_format_succeeded = true;
        }
    } else if ((options & FACTORY_RESET_CLEAR_WALLET) != 0) {
        clear_wallet_files("/data", 0);
    }

    if ((options & FACTORY_RESET_CLEAR_CARDS) != 0) {
        const esp_partition_t *partition =
            esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                     ESP_PARTITION_SUBTYPE_ANY,
                                     "canokey_lfs");
        if (!partition) {
            ESP_LOGE(TAG, "CanoKey data partition not found during factory reset");
        } else {
            esp_err_t ret = esp_partition_erase_range(partition, 0, partition->size);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "Failed to clear card data: %s", esp_err_to_name(ret));
            }
        }
    }

    esp_err_t ret = nvs_flash_deinit();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to deinitialize NVS before factory reset: %s",
                 esp_err_to_name(ret));
    }
    ret = nvs_flash_erase();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to erase NVS during factory reset: %s",
                 esp_err_to_name(ret));
    }

    if (ret == ESP_OK &&
        (((options & FACTORY_RESET_FORMAT_USB) == 0) || usb_format_succeeded)) {
        ret = nvs_flash_init();
        if (ret == ESP_OK) {
            ret = usb_msc_mark_storage_initialized();
        }
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to preserve USB initialization state: %s",
                     esp_err_to_name(ret));
        }
    }

    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

static void restore_dialog_deleted_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_DELETE) {
        return;
    }
    s_restore_dialog = NULL;
    s_restore_pin_textarea = NULL;
    s_restore_status_label = NULL;
}

static void close_restore_dialog(void)
{
    if (s_restore_dialog && lv_obj_is_valid(s_restore_dialog)) {
        lv_obj_delete(s_restore_dialog);
    }
    if (s_factory_reset_switch && lv_obj_is_valid(s_factory_reset_switch)) {
        lv_obj_clear_state(s_factory_reset_switch, LV_STATE_CHECKED);
    }
}

static void restore_cancel_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_CLICKED &&
        !atomic_load_explicit(&s_factory_reset_running, memory_order_acquire)) {
        close_restore_dialog();
    }
}

static void begin_factory_reset(void)
{
    if (!s_restore_pin_textarea ||
        !setup_ui_device_password_matches(lv_textarea_get_text(s_restore_pin_textarea))) {
        if (s_restore_status_label) {
            lv_label_set_text(s_restore_status_label, ui_i18n_get(UI_STR_PIN_INCORRECT));
            lv_obj_set_style_text_color(s_restore_status_label, lv_color_hex(0xF27A7A), 0);
        }
        if (s_restore_pin_textarea) {
            lv_textarea_set_text(s_restore_pin_textarea, "");
        }
        return;
    }

    bool expected = false;
    if (!atomic_compare_exchange_strong_explicit(&s_factory_reset_running,
                                                  &expected, true,
                                                  memory_order_acq_rel,
                                                  memory_order_acquire)) {
        return;
    }

    uint32_t options = 0;
    if (s_restore_clear_cards_switch &&
        lv_obj_has_state(s_restore_clear_cards_switch, LV_STATE_CHECKED)) {
        options |= FACTORY_RESET_CLEAR_CARDS;
    }
    if (s_restore_format_usb_switch &&
        lv_obj_has_state(s_restore_format_usb_switch, LV_STATE_CHECKED)) {
        options |= FACTORY_RESET_FORMAT_USB;
    }
    if (s_restore_clear_wallet_switch &&
        lv_obj_has_state(s_restore_clear_wallet_switch, LV_STATE_CHECKED)) {
        options |= FACTORY_RESET_CLEAR_WALLET;
    }

    lv_label_set_text(s_restore_status_label, ui_i18n_get(UI_STR_RESTORING));
    lv_obj_set_style_text_color(s_restore_status_label, lv_color_hex(0xFFC958), 0);
    lv_obj_add_state(s_restore_pin_textarea, LV_STATE_DISABLED);

    BaseType_t created = xTaskCreate(factory_reset_task,
                                     "factory_reset",
                                     FACTORY_RESET_TASK_STACK,
                                     (void *)(uintptr_t)options,
                                     FACTORY_RESET_TASK_PRIORITY,
                                     NULL);
    if (created != pdPASS) {
        atomic_store_explicit(&s_factory_reset_running, false, memory_order_release);
        lv_label_set_text(s_restore_status_label, ui_i18n_get(UI_STR_FAILED));
        lv_obj_set_style_text_color(s_restore_status_label, lv_color_hex(0xF27A7A), 0);
        lv_obj_clear_state(s_restore_pin_textarea, LV_STATE_DISABLED);
    }
}

static void restore_keypad_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_VALUE_CHANGED ||
        !s_restore_pin_textarea ||
        atomic_load_explicit(&s_factory_reset_running, memory_order_acquire)) {
        return;
    }

    lv_obj_t *keypad = lv_event_get_target(event);
    uint32_t button_id = lv_buttonmatrix_get_selected_button(keypad);
    const char *key = lv_buttonmatrix_get_button_text(keypad, button_id);
    if (!key) {
        return;
    }
    if (strcmp(key, LV_SYMBOL_BACKSPACE) == 0) {
        lv_textarea_delete_char(s_restore_pin_textarea);
    } else if (strcmp(key, LV_SYMBOL_OK) == 0) {
        begin_factory_reset();
    } else if (key[0] >= '0' && key[0] <= '9') {
        lv_textarea_add_char(s_restore_pin_textarea, key[0]);
    }
}

static void show_restore_dialog(void)
{
    if (s_restore_dialog ||
        atomic_load_explicit(&s_factory_reset_running, memory_order_acquire)) {
        return;
    }

    lv_obj_t *screen = lv_screen_active();
    s_restore_dialog = lv_obj_create(screen);
    lv_obj_remove_style_all(s_restore_dialog);
    lv_obj_set_size(s_restore_dialog, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_restore_dialog, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_restore_dialog, LV_OPA_70, 0);
    lv_obj_add_flag(s_restore_dialog, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_restore_dialog, LV_OBJ_FLAG_GESTURE_BUBBLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_restore_dialog, restore_dialog_deleted_cb, LV_EVENT_DELETE, NULL);

    lv_obj_t *panel = lv_obj_create(s_restore_dialog);
    lv_obj_set_size(panel, 450, 594);
    lv_obj_center(panel);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x20232B), 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(0x3A3C46), 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_radius(panel, 6, 0);
    lv_obj_set_style_pad_all(panel, 0, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_GESTURE_BUBBLE);

    lv_obj_t *title = lv_label_create(panel);
    lv_label_set_text(title, ui_i18n_get(UI_STR_FACTORY_RESET));
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_set_style_text_font(title, ui_i18n_font_28(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 20, 16);

    lv_obj_t *prompt = lv_label_create(panel);
    lv_label_set_text(prompt, ui_i18n_get(UI_STR_FACTORY_RESET_PROMPT));
    lv_obj_set_width(prompt, 410);
    lv_label_set_long_mode(prompt, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(prompt, lv_color_hex(0xB7BBC5), 0);
    lv_obj_set_style_text_font(prompt, ui_i18n_font_16(), 0);
    lv_obj_align(prompt, LV_ALIGN_TOP_LEFT, 20, 60);

    s_restore_pin_textarea = lv_textarea_create(panel);
    lv_obj_set_size(s_restore_pin_textarea, 410, 68);
    lv_obj_align(s_restore_pin_textarea, LV_ALIGN_TOP_MID, 0, 154);
    lv_textarea_set_one_line(s_restore_pin_textarea, true);
    lv_textarea_set_password_mode(s_restore_pin_textarea, true);
    lv_textarea_set_max_length(s_restore_pin_textarea, DEVICE_PASSWORD_MAX);
    lv_obj_set_style_bg_color(s_restore_pin_textarea, lv_color_hex(0x292A32), 0);
    lv_obj_set_style_border_color(s_restore_pin_textarea, lv_color_hex(0x4A4D59), 0);
    lv_obj_set_style_text_color(s_restore_pin_textarea, lv_color_white(), 0);
    lv_obj_set_style_text_font(s_restore_pin_textarea, &lv_font_montserrat_28, 0);

    s_restore_status_label = lv_label_create(panel);
    lv_label_set_text(s_restore_status_label, "");
    lv_obj_set_width(s_restore_status_label, 410);
    lv_obj_set_style_text_align(s_restore_status_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(s_restore_status_label, ui_i18n_font_16(), 0);
    lv_obj_align(s_restore_status_label, LV_ALIGN_TOP_MID, 0, 230);

    static const char *const pin_map[] = {
        "1", "2", "3", "\n",
        "4", "5", "6", "\n",
        "7", "8", "9", "\n",
        LV_SYMBOL_BACKSPACE, "0", LV_SYMBOL_OK, ""
    };
    lv_obj_t *keypad = lv_buttonmatrix_create(panel);
    lv_buttonmatrix_set_map(keypad, pin_map);
    lv_obj_set_size(keypad, 410, 260);
    lv_obj_align(keypad, LV_ALIGN_TOP_MID, 0, 260);
    lv_obj_set_style_bg_opa(keypad, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(keypad, 0, 0);
    lv_obj_set_style_pad_all(keypad, 0, 0);
    lv_obj_set_style_pad_row(keypad, 4, 0);
    lv_obj_set_style_pad_column(keypad, 4, 0);
    lv_obj_set_style_bg_color(keypad, lv_color_hex(0x292A32), LV_PART_ITEMS);
    lv_obj_set_style_bg_color(keypad, lv_color_hex(0x3A3C46),
                              LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_text_color(keypad, lv_color_white(), LV_PART_ITEMS);
    lv_obj_set_style_text_font(keypad, &lv_font_montserrat_28, LV_PART_ITEMS);
    lv_obj_add_event_cb(keypad, restore_keypad_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *cancel = lv_button_create(panel);
    lv_obj_set_size(cancel, 180, 52);
    lv_obj_align(cancel, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_set_style_bg_color(cancel, lv_color_hex(0x3A3C46), 0);
    lv_obj_set_style_radius(cancel, 5, 0);
    lv_obj_add_event_cb(cancel, restore_cancel_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cancel_label = lv_label_create(cancel);
    lv_label_set_text(cancel_label, ui_i18n_get(UI_STR_CANCEL));
    lv_obj_set_style_text_font(cancel_label, ui_i18n_font_28(), 0);
    lv_obj_center(cancel_label);
}

static void factory_reset_switch_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_VALUE_CHANGED) {
        return;
    }
    lv_obj_t *sw = lv_event_get_target(event);
    if (lv_obj_has_state(sw, LV_STATE_CHECKED)) {
        show_restore_dialog();
    }
}

static lv_obj_t *add_restore_switch_control(lv_obj_t *row,
                                             const char *key,
                                             bool factory_action)
{
    lv_obj_t *sw = lv_switch_create(row);
    lv_obj_set_size(sw, 82, 44);
    lv_obj_align(sw, LV_ALIGN_TOP_RIGHT, -18, 14);
    lv_obj_set_style_bg_color(sw, lv_color_hex(0x3A3C46), LV_PART_MAIN);
    lv_obj_set_style_bg_color(sw, lv_color_hex(0x2997E8),
                              LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(sw, lv_color_white(), LV_PART_KNOB);
    if (!factory_action && read_security_setting_u8(key, 0) != 0) {
        lv_obj_add_state(sw, LV_STATE_CHECKED);
    }
    lv_obj_add_event_cb(sw,
                        factory_action ? factory_reset_switch_event_cb
                                       : security_switch_event_cb,
                        LV_EVENT_VALUE_CHANGED,
                        factory_action ? NULL : (void *)key);
    return sw;
}

static lv_obj_t *add_restore_option(lv_obj_t *parent,
                                    const char *text,
                                    const char *key)
{
    lv_obj_t *row = create_connect_row(parent, 88);
    add_security_row_label(row, text);
    return add_restore_switch_control(row, key, false);
}

static void add_restore_settings_group(lv_obj_t *parent)
{
    lv_obj_t *group = lv_obj_create(parent);
    lv_obj_remove_style_all(group);
    lv_obj_set_size(group, SETUP_UI_WIDTH - 50, 352);
    lv_obj_set_style_pad_all(group, 0, 0);
    lv_obj_set_style_pad_row(group, 0, 0);
    lv_obj_set_flex_flow(group, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(group, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *factory_row = create_connect_row(group, 88);
    add_security_row_label(factory_row, ui_i18n_get(UI_STR_FACTORY_RESET));
    s_factory_reset_switch = add_restore_switch_control(factory_row, NULL, true);

    s_restore_clear_cards_switch =
        add_restore_option(group, ui_i18n_get(UI_STR_CLEAR_CARDS),
                           RESTORE_CLEAR_CARDS_NVS_KEY);
    s_restore_format_usb_switch =
        add_restore_option(group, ui_i18n_get(UI_STR_FORMAT_USB_RESTORE),
                           RESTORE_FORMAT_USB_NVS_KEY);
    s_restore_clear_wallet_switch =
        add_restore_option(group, ui_i18n_get(UI_STR_CLEAR_WALLET),
                           RESTORE_CLEAR_WALLET_NVS_KEY);
}

void setup_ui_show(void)
{
    esp_err_t i18n_ret = ui_i18n_init();
    if (i18n_ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to initialize UI language: %s", esp_err_to_name(i18n_ret));
    }
    if (canokey_esp32p4_is_ready() && !sync_sc_ccid_settings()) {
        ESP_LOGW(TAG, "Failed to synchronize CanoKey CCID feature configuration");
    }

    s_trust_browser_switch = NULL;
    s_about_temperature_value = NULL;
    s_factory_reset_switch = NULL;
    s_restore_clear_cards_switch = NULL;
    s_restore_format_usb_switch = NULL;
    s_restore_clear_wallet_switch = NULL;
    s_restore_dialog = NULL;
    s_restore_pin_textarea = NULL;
    s_restore_status_label = NULL;

    lv_obj_t *screen = lv_screen_active();
    lv_obj_clean(screen);
    ui_nav_enable_right_swipe(screen);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x11141A), 0);
    lv_obj_set_style_pad_all(screen, 0, 0);

    ui_status_bar_add(screen, SETUP_UI_WIDTH);

    lv_obj_t *title = lv_label_create(screen);
    lv_label_set_text(title, ui_i18n_get(UI_STR_SETUP));
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_set_style_text_font(title, ui_i18n_font_32(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 25, 82);

    lv_obj_t *list = lv_obj_create(screen);
    lv_obj_remove_style_all(list);
    lv_obj_set_size(list, SETUP_UI_WIDTH, SETUP_UI_HEIGHT - 140);
    lv_obj_align(list, LV_ALIGN_TOP_MID, 0, 140);
    lv_obj_set_style_pad_left(list, 25, 0);
    lv_obj_set_style_pad_right(list, 25, 0);
    lv_obj_set_style_pad_top(list, 0, 0);
    lv_obj_set_style_pad_bottom(list, 0, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list, 10, 0);
    lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, LV_PART_SCROLLBAR);

    add_setup_section_label(list, ui_i18n_get(UI_STR_DISPLAY));

    lv_obj_t *display_group = lv_obj_create(list);
    lv_obj_remove_style_all(display_group);
    lv_obj_set_size(display_group, SETUP_UI_WIDTH - 50, 96 * 3);
    lv_obj_set_style_pad_all(display_group, 0, 0);
    lv_obj_set_style_pad_row(display_group, 0, 0);
    lv_obj_set_flex_flow(display_group, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(display_group, LV_OBJ_FLAG_SCROLLABLE);

    add_backlight_slider(display_group);
    add_timeout_dropdown(display_group);
    add_custom_background_switch(display_group);
    add_setup_section_label(list, ui_i18n_get(UI_STR_CONNECT));

    lv_obj_t *connect_group = lv_obj_create(list);
    lv_obj_remove_style_all(connect_group);
    lv_obj_set_size(connect_group, SETUP_UI_WIDTH - 50, 1172);
    lv_obj_set_style_pad_all(connect_group, 0, 0);
    lv_obj_set_style_pad_row(connect_group, 0, 0);
    lv_obj_set_flex_flow(connect_group, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(connect_group, LV_OBJ_FLAG_SCROLLABLE);

    add_usb_disk_switch(connect_group);
    add_trust_browser_switch(connect_group);
    add_connect_action(connect_group, ui_i18n_get(UI_STR_NIC_IP),
                       ui_i18n_get(UI_STR_SET), nic_ip_set_event_cb);
    add_connect_action(connect_group, ui_i18n_get(UI_STR_ADDON_PAIR),
                       ui_i18n_get(UI_STR_RESET), addon_pair_reset_event_cb);
    add_connect_dropdown(connect_group, ui_i18n_get(UI_STR_KBD_MODE),
                         "US\nUK\nGerman DE\nFrench FR\nSpanish ES\nItalian IT\nPortuguese PT-PT",
                         CONNECT_HID_LAYOUT_NVS_KEY, HID_LAYOUT_US);
    add_connect_dropdown(connect_group, ui_i18n_get(UI_STR_HID_OUTPUT),
                         "USB\nBluetooth\nAuto", CONNECT_HID_OUTPUT_NVS_KEY,
                         HID_KEYBOARD_TRANSPORT_USB);
    add_connect_dropdown(connect_group, ui_i18n_get(UI_STR_NEWLINE),
                         "Off\nOn", CONNECT_NEWLINE_NVS_KEY, 0);
    add_connect_switch(connect_group, ui_i18n_get(UI_STR_PW_ENTER),
                       CONNECT_PW_ENTER_NVS_KEY, false);
    add_connect_dropdown(connect_group, ui_i18n_get(UI_STR_HID_SPEED),
                         "Normal\nFast\nSlow", CONNECT_HID_SPEED_NVS_KEY, 0);
    add_connect_switch(connect_group, ui_i18n_get(UI_STR_QR_XFER),
                       CONNECT_QR_XFER_NVS_KEY, false);
    add_connect_switch(connect_group, ui_i18n_get(UI_STR_NFC_XFER),
                       CONNECT_NFC_XFER_NVS_KEY, false);
    add_connect_switch(connect_group, ui_i18n_get(UI_STR_XFER_ALL_INFO),
                       CONNECT_XFER_ALL_NVS_KEY, false);
    add_connect_action(connect_group, ui_i18n_get(UI_STR_USB_VID),
                       ui_i18n_get(UI_STR_SET), usb_vid_setup_event_cb);

    add_usb_ncm_switch(list);

    add_setup_section_label(list, ui_i18n_get(UI_STR_SYSTEM));

    lv_obj_t *system_group = lv_obj_create(list);
    lv_obj_remove_style_all(system_group);
    lv_obj_set_size(system_group, SETUP_UI_WIDTH - 50, 88 * 8);
    lv_obj_set_style_pad_all(system_group, 0, 0);
    lv_obj_set_style_pad_row(system_group, 0, 0);
    lv_obj_set_flex_flow(system_group, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(system_group, LV_OBJ_FLAG_SCROLLABLE);

    add_connect_action(system_group, ui_i18n_get(UI_STR_RELOAD_DB),
                       ui_i18n_get(UI_STR_LOAD), reload_database_event_cb);
    add_system_switch(system_group, ui_i18n_get(UI_STR_BATTERY_SAVE),
                      SYSTEM_BATTERY_SAVE_NVS_KEY, true);
    add_connect_action(system_group, ui_i18n_get(UI_STR_TIMEZONE),
                       ui_i18n_get(UI_STR_SET), timezone_setting_event_cb);
    add_connect_action(system_group, ui_i18n_get(UI_STR_TIME),
                       ui_i18n_get(UI_STR_SET), date_time_setting_event_cb);
    add_system_switch(system_group, ui_i18n_get(UI_STR_AUTO_ADJUST),
                      SYSTEM_AUTO_ADJUST_NVS_KEY, false);
    add_system_dropdown(system_group, ui_i18n_get(UI_STR_LOCK_CLOCK),
                        "HM YMD\nHM MD\nHM\noff",
                        SYSTEM_LOCK_CLOCK_NVS_KEY, 0);
    add_system_language_dropdown(system_group);
    add_connect_action(system_group, ui_i18n_get(UI_STR_HIDE_MENU_ITEM),
                       ui_i18n_get(UI_STR_SET), hide_menu_item_event_cb);

    add_setup_section_label(list, ui_i18n_get(UI_STR_SC_SETTING));

    lv_obj_t *sc_group = lv_obj_create(list);
    lv_obj_remove_style_all(sc_group);
    lv_obj_set_size(sc_group, SETUP_UI_WIDTH - 50, 88 * 5);
    lv_obj_set_style_pad_all(sc_group, 0, 0);
    lv_obj_set_style_pad_row(sc_group, 0, 0);
    lv_obj_set_flex_flow(sc_group, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(sc_group, LV_OBJ_FLAG_SCROLLABLE);

    add_sc_switch(sc_group, ui_i18n_get(UI_STR_OPENPGP_ON),
                  SC_OPENPGP_ON_NVS_KEY, true);
    add_sc_switch(sc_group, ui_i18n_get(UI_STR_PIV_ON),
                  SC_PIV_ON_NVS_KEY, true);
    add_sc_switch(sc_group, ui_i18n_get(UI_STR_NDEF_WP),
                  SC_NDEF_WP_NVS_KEY, false);
    add_connect_action(sc_group, ui_i18n_get(UI_STR_PGP_WIPE),
                       ui_i18n_get(UI_STR_RESET), sc_reset_event_cb);
    add_connect_action(sc_group, ui_i18n_get(UI_STR_PIV_WIPE),
                       ui_i18n_get(UI_STR_RESET), sc_reset_event_cb);
    add_setup_section_label(list, ui_i18n_get(UI_STR_NRF_SETTINGS));
    add_nrf_settings_group(list);

    add_setup_section_label(list, ui_i18n_get(UI_STR_SECURITY));
    add_security_settings_group(list);

    add_setup_section_label(list, ui_i18n_get(UI_STR_RESTORE));
    add_restore_settings_group(list);

    add_setup_section_label(list, ui_i18n_get(UI_STR_ABOUT));
    add_about_group(list);
    start_temperature_read();
}

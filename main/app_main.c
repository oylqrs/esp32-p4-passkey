/*
 * SPDX-FileCopyrightText: 2010-2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include <stdint.h>
#include <stddef.h>

#include "credential_test.h"
#include "ADC/battery_monitor.h"
#include "FT6336.h"
#include "app_fido.h"
#include "PCF85063/pcf85063.h"
#include "canokey_esp32p4.h"
#include "canokey_nfc.h"
#include "chameleon_uart/chameleon_uart.h"
#include "chameleon_uart/chameleon_ble_hid.h"
#include "debug_monitor_uart.h"
#include "browser_autofill/browser_confirm.h"
#include "display_idle.h"
#include "lcd/lcd_driver.h"
#include "powerdown_state.h"
#include "runtime_monitor.h"
#include "test_ctap_makecredential.h"
#include "test_fido_storage_loop.h"
#include "test_aes.h"
#include "test_ecdsa_sign.h"
#include "test_ecdsa_verify.h"
#include "test_ecc.h"
#include "test_hmac_sha256.h"
#include "test_public_key_verify.h"
#include "test_random.h"
#include "test_sha256.h"
#include "ui/unlock_pin_ui.h"
#include "ui/setup_ui.h"
#include "ui/ui_clock_desktop.h"
#include "ui/ui_fido_confirm.h"
#include "ui/i18n/ui_i18n.h"
#include "ui/wallpaper.h"
#include "wallet/wallet_service.h"
#include "usb_hid_keyboard.h"
#include "hid_keyboard.h"
#include "firmware_update.h"
#include "usb_nrf/USB_NRF_SWITCH.h"
#include "usb_msc.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

static const char *TAG = "app";

#define LVGL_TICK_PERIOD_MS             2
#define LVGL_DRAW_BUF_LINES             80
#define LCD_BACKLIGHT_GPIO              GPIO_NUM_19
#define BOARD_INIT_HIGH_GPIO             GPIO_NUM_35

typedef struct {
    esp_lcd_panel_handle_t panel;
    lv_display_t *display;
} lvgl_port_display_t;

static const char *reset_reason_name(esp_reset_reason_t reason)
{
    switch (reason) {
    case ESP_RST_POWERON: return "power-on";
    case ESP_RST_EXT: return "external pin";
    case ESP_RST_SW: return "software restart";
    case ESP_RST_PANIC: return "panic";
    case ESP_RST_INT_WDT: return "interrupt watchdog";
    case ESP_RST_TASK_WDT: return "task watchdog";
    case ESP_RST_WDT: return "hardware watchdog";
    case ESP_RST_DEEPSLEEP: return "deep sleep wake";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_SDIO: return "SDIO";
    case ESP_RST_USB: return "USB";
    case ESP_RST_JTAG: return "JTAG";
    case ESP_RST_EFUSE: return "eFuse error";
    case ESP_RST_PWR_GLITCH: return "power glitch";
    case ESP_RST_CPU_LOCKUP: return "CPU lockup";
    case ESP_RST_UNKNOWN:
    default: return "unknown";
    }
}



static void lvgl_tick_cb(void *arg)
{
    (void)arg;
    lv_tick_inc(LVGL_TICK_PERIOD_MS);
}

static void lvgl_flush_cb(lv_display_t *display, const lv_area_t *area, uint8_t *px_map)
{
    lvgl_port_display_t *port_display = lv_display_get_user_data(display);
    if (!port_display || !port_display->panel) {
        lv_display_flush_ready(display);
        return;
    }

    esp_err_t ret = esp_lcd_panel_draw_bitmap(port_display->panel,
                                              area->x1,
                                              area->y1,
                                              area->x2 + 1,
                                              area->y2 + 1,
                                              px_map);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "LVGL flush failed: %s", esp_err_to_name(ret));
    }

    lv_display_flush_ready(display);
}

static void lvgl_touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;

    static uint16_t last_x;
    static uint16_t last_y;

    ft6336_point_t point = {0};
    uint8_t point_count = 0;
    esp_err_t ret = ft6336_read_point(&point, &point_count);
    bool pressed = ret == ESP_OK && point_count > 0;
    if (display_idle_should_suppress_touch(pressed)) {
        data->state = LV_INDEV_STATE_RELEASED;
        data->point.x = last_x;
        data->point.y = last_y;
        return;
    }

    if (pressed) {
        if (point.x >= LCD_H_RES) {
            point.x = LCD_H_RES - 1;
        }
        if (point.y >= LCD_V_RES) {
            point.y = LCD_V_RES - 1;
        }

        data->state = LV_INDEV_STATE_PRESSED;
        data->point.x = point.x;
        data->point.y = point.y;

        // ESP_LOGI("touch", "touch count=%u id=%u event=%u x=%u y=%u",
        //          point_count, point.id, point.event, point.x, point.y);

        last_x = point.x;
        last_y = point.y;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
        data->point.x = last_x;
        data->point.y = last_y;
        // ESP_LOGI("touch", "release");
    }
}

static void lvgl_task(void *arg)
{
    (void)arg;
    while (true) {
        uint32_t delay_ms = lv_timer_handler();
        if (delay_ms < 5) {
            delay_ms = 5;
        } else if (delay_ms > 20) {
            delay_ms = 20;
        }
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
}

static void lvgl_port_start(esp_lcd_panel_handle_t panel, bool touch_enabled)
{
    lv_init();

    static lvgl_port_display_t port_display = {0};
    port_display.panel = panel;

    lv_display_t *display = lv_display_create(LCD_H_RES, LCD_V_RES);
    port_display.display = display;
    lv_display_set_user_data(display, &port_display);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(display, lvgl_flush_cb);

    const size_t draw_buf_size = LCD_H_RES * LVGL_DRAW_BUF_LINES * LCD_BYTES_PER_PIXEL;
    void *draw_buf_1 = heap_caps_malloc(draw_buf_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT | MALLOC_CAP_DMA);
    void *draw_buf_2 = heap_caps_malloc(draw_buf_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT | MALLOC_CAP_DMA);
    ESP_ERROR_CHECK((draw_buf_1 && draw_buf_2) ? ESP_OK : ESP_ERR_NO_MEM);
    lv_display_set_buffers(display, draw_buf_1, draw_buf_2, draw_buf_size, LV_DISPLAY_RENDER_MODE_PARTIAL);

    if (touch_enabled) {
        lv_indev_t *indev = lv_indev_create();
        lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
        lv_indev_set_read_cb(indev, lvgl_touch_read_cb);
        lv_indev_set_display(indev, display);
    }

    const esp_timer_create_args_t tick_timer_args = {
        .callback = lvgl_tick_cb,
        .name = "lvgl_tick",
    };
    esp_timer_handle_t tick_timer = NULL;
    ESP_ERROR_CHECK(esp_timer_create(&tick_timer_args, &tick_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(tick_timer, LVGL_TICK_PERIOD_MS * 1000));

    browser_confirm_init();
    if (touch_enabled) {
        esp_err_t idle_ret = display_idle_init(display);
        if (idle_ret != ESP_OK) {
            ESP_LOGW(TAG, "Display idle manager init failed: %s",
                     esp_err_to_name(idle_ret));
        }
    }

    if (setup_ui_get_custom_background()) {
        wallpaper_show();
    } else {
        ui_clock_desktop_show();
    }

    esp_err_t monitor_ui_ret = runtime_monitor_ui_init();
    if (monitor_ui_ret != ESP_OK) {
        ESP_LOGW(TAG, "Runtime monitor overlay init failed: %s",
                 esp_err_to_name(monitor_ui_ret));
    }

    BaseType_t task_created = xTaskCreate(lvgl_task, "lvgl", 8192, NULL, 4, NULL);
    if (task_created != pdPASS) {
        size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        size_t largest_internal = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        ESP_LOGE(TAG, "Failed to create LVGL task: free_internal=%u largest_block=%u",
                 (unsigned int)free_internal,
                 (unsigned int)largest_internal);
        ESP_ERROR_CHECK(ESP_ERR_NO_MEM);
    }
}

esp_err_t app_fido_start(void)
{
    if (canokey_esp32p4_is_running()) {
        return ESP_OK;
    }

    ui_fido_confirm_init();
    return canokey_esp32p4_start();
}

static void gpio1_init_high(void)
{
    const gpio_config_t gpio1_config = {
        .pin_bit_mask = 1ULL << GPIO_NUM_37,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    ESP_ERROR_CHECK(gpio_config(&gpio1_config));
    ESP_ERROR_CHECK(gpio_set_level(GPIO_NUM_37, 1));

 
    vTaskDelay(pdMS_TO_TICKS(100));

    gpio_set_level(GPIO_NUM_37, 0);
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(GPIO_NUM_37, 1);
    vTaskDelay(pdMS_TO_TICKS(100));
}

static esp_err_t init_nvs_storage(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    return ret;
}

static bool should_run_normal_boot(void)
{
    esp_err_t ret = init_nvs_storage();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "NVS init failed, falling back to normal boot: %s", esp_err_to_name(ret));
        return true;
    }

    nvs_handle_t nvs;
    ret = nvs_open(POWERDOWN_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "NVS open failed, falling back to normal boot: %s", esp_err_to_name(ret));
        return true;
    }

    uint8_t powerdown_requested = 0;
    ret = nvs_get_u8(nvs, POWERDOWN_NVS_KEY, &powerdown_requested);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        powerdown_requested = 0;
        ret = ESP_OK;
    } else if (ret != ESP_OK) {
        ESP_LOGW(TAG, "NVS read powerdown request failed, falling back to normal boot: %s",
                 esp_err_to_name(ret));
        nvs_close(nvs);
        return true;
    }

    uint32_t boot_count = 0;
    ret = nvs_get_u32(nvs, POWERDOWN_BOOT_COUNT_NVS_KEY, &boot_count);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        boot_count = 0;
        ret = ESP_OK;
    } else if (ret != ESP_OK) {
        ESP_LOGW(TAG, "NVS read boot count failed, falling back to normal boot: %s",
                 esp_err_to_name(ret));
        nvs_close(nvs);
        return true;
    }

    boot_count++;
    ret = nvs_set_u32(nvs, POWERDOWN_BOOT_COUNT_NVS_KEY, boot_count);
    if (powerdown_requested != 0) {
        if (ret == ESP_OK) {
            ret = nvs_set_u8(nvs, POWERDOWN_NVS_KEY, 0);
        }
    }
    if (ret == ESP_OK) {
        ret = nvs_commit(nvs);
    }
    nvs_close(nvs);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to save boot state, continuing normal boot: %s",
                 esp_err_to_name(ret));
        return true;
    }

    bool normal_boot = (boot_count & 1U) == 0 && powerdown_requested == 0;
    ESP_LOGI(TAG, "Boot count=%lu mode=%s%s",
             (unsigned long)boot_count,
             normal_boot ? "normal" : "deep-sleep",
             powerdown_requested ? " (powerdown requested)" : "");
    return normal_boot;
}

static void board_init_high_gpio(void)
{
    const gpio_config_t config = {
        .pin_bit_mask = 1ULL << BOARD_INIT_HIGH_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    ESP_ERROR_CHECK(gpio_config(&config));
    ESP_ERROR_CHECK(gpio_set_level(BOARD_INIT_HIGH_GPIO, 1));
}

static void force_black_screen(void)
{
    const gpio_config_t backlight_config = {
        .pin_bit_mask = 1ULL << LCD_BACKLIGHT_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    ESP_ERROR_CHECK(gpio_config(&backlight_config));
    ESP_ERROR_CHECK(gpio_set_level(LCD_BACKLIGHT_GPIO, 0));
    ESP_ERROR_CHECK(gpio_hold_en(LCD_BACKLIGHT_GPIO));
    ESP_LOGW(TAG, "Sleep boot: LCD backlight forced off");
}

void app_main(void)
{ 
    esp_reset_reason_t reset_reason = esp_reset_reason();
    ESP_LOGI(TAG, "Previous reset reason: %s (%d)",
             reset_reason_name(reset_reason), (int)reset_reason);

    esp_err_t update_ret = firmware_update_init();
    if (update_ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to confirm running OTA image: %s", esp_err_to_name(update_ret));
    }

    board_init_high_gpio();

    ESP_ERROR_CHECK(gpio_set_direction(LCD_BACKLIGHT_GPIO, GPIO_MODE_OUTPUT));
    ESP_ERROR_CHECK(gpio_set_level(LCD_BACKLIGHT_GPIO, 0));
    ESP_ERROR_CHECK(gpio_hold_dis(LCD_BACKLIGHT_GPIO));

    esp_log_level_set("tusb_desc", ESP_LOG_WARN);
    esp_log_level_set("kdbx", ESP_LOG_INFO);
#if DEBUG_MONITOR_UART_ENABLED
    esp_err_t monitor_ret = debug_monitor_uart_init();
    if (monitor_ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize debug UART: %s", esp_err_to_name(monitor_ret));
    }
#endif


   // test_sha256();
    //test_hmac_sha256();
   // test_aes();
    //test_random();
    //test_ecc();
   // test_public_key_verify();
   // test_ecdsa_sign();
   // test_ecdsa_verify();
   // test_ctap_makecredential();
    //credential_test();
   // test_fido_storage_loop();
  //  canokey_esp32p4_crypto_selftest();

    if (!should_run_normal_boot()) {
        force_black_screen();
        ESP_LOGW(TAG, "Entering deep sleep; reset or power cycle to start normally");
        esp_deep_sleep_start();
    }

    // Format the USB storage before starting UI and periodic monitor tasks.
    // This keeps first-boot flash activity isolated from LVGL/PSRAM access.
    esp_err_t msc_ret = usb_msc_storage_init();
    if (msc_ret != ESP_OK) {
        ESP_LOGW(TAG, "USB MSC storage init failed: %s", esp_err_to_name(msc_ret));
    }

    esp_lcd_panel_handle_t dpi_panel = lcd_driver_init();

    esp_err_t touch_ret = ft6336_init();
    bool touch_enabled = touch_ret == ESP_OK;
    if (!touch_enabled) {
        ESP_LOGW(TAG, "Touch init failed: %s", esp_err_to_name(touch_ret));
    }

    ESP_LOGI(TAG, "Start LVGL");
    lvgl_port_start(dpi_panel, touch_enabled);

    esp_err_t runtime_monitor_ret = runtime_monitor_start();
    if (runtime_monitor_ret != ESP_OK) {
        ESP_LOGE(TAG, "Runtime monitor start failed: %s",
                 esp_err_to_name(runtime_monitor_ret));
    }


    
    esp_err_t wallet_ret = wallet_service_init();
    if (wallet_ret != ESP_OK) {
        ESP_LOGE(TAG, "Wallet crypto init failed: %s", esp_err_to_name(wallet_ret));
    }

    esp_err_t nrf_ret = usb_nrf_switch_init();
    if (nrf_ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize NRF switch GPIO: %s", esp_err_to_name(nrf_ret));
    }

#if !DEBUG_MONITOR_UART_ENABLED
    esp_err_t uart_ret = chameleon_uart_init();
    if (uart_ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize Chameleon UART: %s", esp_err_to_name(uart_ret));
    } else {
        esp_err_t ble_hid_ret = chameleon_ble_hid_init();
        if (ble_hid_ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to initialize Chameleon BLE HID: %s",
                     esp_err_to_name(ble_hid_ret));
        }
        esp_err_t nfc_ret = canokey_nfc_bind_transport(chameleon_uart_nfc_transceive);
        if (nfc_ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to bind NFC FIDO UART: %s", esp_err_to_name(nfc_ret));
        } else {
            setup_nrf_mode_t nrf_mode = setup_ui_get_nrf_mode();
            canokey_nfc_set_enabled(nrf_mode == SETUP_NRF_MODE_NFC_BT);
        }
        setup_nrf_mode_t nrf_mode = setup_ui_get_nrf_mode();
        bool ble_hid_enabled = nrf_mode == SETUP_NRF_MODE_NFC_BT ||
                               nrf_mode == SETUP_NRF_MODE_BT_PAIRING;
        chameleon_ble_hid_set_mode(
            ble_hid_enabled, nrf_mode == SETUP_NRF_MODE_BT_PAIRING);
        ESP_LOGI(TAG, "NRF mode=%u, NFC bridge=%u BLE HID=%u pairing=%u",
                 (unsigned)nrf_mode,
                 nrf_mode == SETUP_NRF_MODE_NFC_BT ? 1 : 0,
                 ble_hid_enabled ? 1 : 0,
                 nrf_mode == SETUP_NRF_MODE_BT_PAIRING ? 1 : 0);
    }
#endif

    esp_err_t rtc_ret = pcf85063_init();
    if (rtc_ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to initialize RTC: %s", esp_err_to_name(rtc_ret));
    }

    //gpio1_init_high();

    esp_err_t i18n_ret = ui_i18n_init();
    if (i18n_ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to restore UI language: %s", esp_err_to_name(i18n_ret));
    }

    esp_err_t hid_ret = ESP_FAIL;
    (void)hid_keyboard_set_transport(setup_ui_get_hid_transport());
    for (int attempt = 1; attempt <= 3; attempt++) {
        hid_ret = usb_hid_keyboard_init();
        if (hid_ret == ESP_OK) {
            break;
        }
        ESP_LOGW(TAG, "USB HID init attempt %d failed: %s",
                 attempt, esp_err_to_name(hid_ret));
        vTaskDelay(pdMS_TO_TICKS(250));
    }

    esp_err_t battery_ret = battery_monitor_start();
    if (battery_ret != ESP_OK) {
        ESP_LOGW(TAG, "Battery monitor start failed: %s", esp_err_to_name(battery_ret));
    }
    
    if (msc_ret == ESP_OK) {
        esp_err_t usbstore_ret = usb_hid_disable_usbstore();
        if (usbstore_ret != ESP_OK) {
            ESP_LOGW(TAG, "USB store default-off setup failed: %s", esp_err_to_name(usbstore_ret));
        }
    }


    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

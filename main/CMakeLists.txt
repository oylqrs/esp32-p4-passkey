idf_component_register(SRCS "usb/usb_hid_keyboard.c" "usb/usb_msc.c" "usb/usb_ccid.c" "FT6336.c" "hello_world_main.c" "lcd/lcd_driver.c" "ADC/battery_monitor.c" "ui/ui_clock_desktop.c" "ui/UI_SCREEN1.c" "ui/UI_SCREEN2.c" "ui/ui_new_entry.c" "ui/ui_script_detail.c" "ui/setup_ui.c" "ui/smartcard_ui.c" "ui/openpgp_ui.c" "ui/ui_piv_detail.c" "ui/Serach_UI.c" "ui/FIDO_Management.c" "ui/ui_fido_confirm.c" "ui/ui_password_manager.c" "ui/ui_password_detail.c" "ui/ui_nav.c" "kdbx/kdbx_loader.c" "kdbx/kdbx_header.c" "kdbx/kdbx_kdf.c" "kdbx/kdbx_payload.c" "kdbx/kdbx_variant.c"
                    PRIV_REQUIRES canokey-port crypto_test fido_test esp_adc esp_lcd esp_driver_gpio esp_driver_i2c esp_driver_ledc esp_hw_support esp_mm esp_rom esp_timer esp_partition esp_tinyusb fatfs lvgl mbedtls nvs_flash wear_levelling usb_hid_callbacks
                    PRIV_INCLUDE_DIRS "${IDF_PATH}/components/esp_lcd/dsi"
                    INCLUDE_DIRS "." "usb")

target_compile_definitions(${COMPONENT_LIB} PRIVATE USB_CCID_ENABLED=1)

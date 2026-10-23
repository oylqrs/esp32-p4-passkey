idf_component_register(SRCS "usb/usb_hid_keyboard.c" "usb/usb_msc.c" "usb/usb_ccid.c" "FT6336.c" "app_main.c" "lcd/lcd_driver.c" "ADC/battery_monitor.c" "ui/ui_clock_desktop.c" "ui/wallpaper.c" "ui/ttf/font_clock_96.c" "ui/ttf/font_cn_16.c" "ui/ttf/font_cn_24.c" "ui/ttf/font_cn_28.c" "ui/ttf/font_cn_32.c" "ui/ttf/font_cn_64.c" "ui/i18n/ui_i18n.c" "ui/i18n/strings_en.c" "ui/i18n/strings_zh.c" "ui/unlock_pin_ui.c" "ui/main_item_ui.c" "ui/ui_new_entry.c" "ui/ui_script_detail.c" "ui/setup_ui.c" "ui/smartcard_ui.c" "ui/chameleon_manager_ui.c" "ui/openpgp_ui.c" "ui/ui_piv_detail.c" "ui/Serach_UI.c" "ui/FIDO_Management.c" "ui/ui_fido_confirm.c" "ui/ui_password_manager.c" "ui/ui_password_detail.c" "ui/ui_nav.c" "ui/ui_status_bar.c" "ui/icon/menu_icons_opaque.c" "ui/kdbx_icon/C00_Password.c" "ui/kdbx_icon/C01_Package_Network.c" "ui/kdbx_icon/C03_Server.c" "ui/kdbx_icon/C19_Mail_Generic.c" "ui/kdbx_icon/C37_KPercentage.c" "ui/kdbx_icon/C38_Samba_Unmount.c" "ui/kdbx_icon/C48_Folder.c" "kdbx/kdbx_loader.c" "kdbx/kdbx_header.c" "kdbx/kdbx_kdf.c" "kdbx/kdbx_payload.c" "kdbx/kdbx_variant.c" "kdbx/kdbx_totp.c"
                    PRIV_REQUIRES canokey-port crypto_test fido_test libwally-core app_update esp_adc esp_lcd esp_driver_gpio esp_driver_i2c esp_driver_ledc esp_driver_uart esp_event esp_hw_support esp_mm esp_rom esp_timer esp_partition esp_tinyusb esp_netif esp_http_server fatfs hal json lwip lvgl mbedtls nvs_flash wear_levelling usb_hid_callbacks
                    PRIV_INCLUDE_DIRS "${IDF_PATH}/components/esp_lcd/dsi"
                    INCLUDE_DIRS "." "usb"
                    EMBED_FILES "ui/wallpaper.bin")

target_compile_definitions(${COMPONENT_LIB} PRIVATE USB_CCID_ENABLED=1)
target_sources(${COMPONENT_LIB} PRIVATE "usb_script/usb_script.c")
target_sources(${COMPONENT_LIB} PRIVATE "usb_script/hid_keyboard.c")
target_sources(${COMPONENT_LIB} PRIVATE "usb_script/ducky_parser.c" "usb_script/ducky_executor.c" "usb_script/ducky_command.c" "usb_script/keyboard_layout.c")
target_include_directories(${COMPONENT_LIB} PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/usb_script")
target_sources(${COMPONENT_LIB} PRIVATE "firmware_update/firmware_update.c" "firmware_update/firmware_verify.c")
target_include_directories(${COMPONENT_LIB} PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/firmware_update")
target_sources(${COMPONENT_LIB} PRIVATE "usb_nrf/USB_NRF_SWITCH.c")
target_sources(${COMPONENT_LIB} PRIVATE "chameleon_uart/chameleon_uart.c")
target_sources(${COMPONENT_LIB} PRIVATE "chameleon_uart/chameleon_commands.c")
target_sources(${COMPONENT_LIB} PRIVATE "chameleon_uart/chameleon_ble_hid.c")
target_sources(${COMPONENT_LIB} PRIVATE "PCF85063/pcf85063.c")
target_sources(${COMPONENT_LIB} PRIVATE "usb_ncm/usb_ncm.c")
target_sources(${COMPONENT_LIB} PRIVATE "ui/ttf/font_cn_35.c")
target_sources(${COMPONENT_LIB} PRIVATE "ui/ttf/font_cn_42.c")
target_sources(${COMPONENT_LIB} PRIVATE "ui/ttf/font_unlock_title_32.c")
target_sources(${COMPONENT_LIB} PRIVATE "ui/icon/ascii_icon.c")
target_sources(${COMPONENT_LIB} PRIVATE
    "wallet/jade_ui_model.c"
    "wallet/jade_wallet_ui.c"
    "wallet/jade_usb_protocol.c"
    "wallet/wallet_crypto.c"
    "wallet/wallet_service.c" "wallet/wallet_storage.c" "wallet/wallet_flow_ui.c")
target_sources(${COMPONENT_LIB} PRIVATE "Qrcode/qrcode_text.c")
target_sources(${COMPONENT_LIB} PRIVATE "ui/UI_SmartCard.c")
target_sources(${COMPONENT_LIB} PRIVATE "display_idle.c")
target_sources(${COMPONENT_LIB} PRIVATE "storage_operation_guard.c")
target_sources(${COMPONENT_LIB} PRIVATE "log_storage.c")
target_sources(${COMPONENT_LIB} PRIVATE "debug_monitor_uart.c")
target_sources(${COMPONENT_LIB} PRIVATE "runtime_monitor.c")
target_sources(${COMPONENT_LIB} PRIVATE "ui/hide_menu_item_ui.c")
target_sources(${COMPONENT_LIB} PRIVATE "ui/ip_setting_ui.c")
target_sources(${COMPONENT_LIB} PRIVATE "ui/ui_usb_vid_setup.c")
target_sources(${COMPONENT_LIB} PRIVATE "ui/date_time_setting_ui.c")
target_sources(${COMPONENT_LIB} PRIVATE "ui/timezone_setup_ui.c" "ui/ttf/font_timezone_24.c")
target_sources(${COMPONENT_LIB} PRIVATE
    "browser_autofill/keepass_http_server.c"
    "browser_autofill/keepass_http_protocol.c"
    "browser_autofill/keepass_http_crypto.c"
    "browser_autofill/browser_pairing.c"
    "browser_autofill/credential_matcher.c"
    "browser_autofill/browser_confirm.c")

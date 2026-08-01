idf_component_register(
    SRCS "usb_hid_callbacks.c"
    REQUIRES esp_tinyusb canokey-port
)

target_link_options(${COMPONENT_LIB} INTERFACE
    "-Wl,-u,tud_hid_descriptor_report_cb"
    "-Wl,-u,tud_hid_get_report_cb"
    "-Wl,-u,tud_hid_set_report_cb"
    "-Wl,-u,tud_hid_report_complete_cb"
)

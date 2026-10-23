# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/canokey-port"
    "${CMAKE_CURRENT_LIST_DIR}/main/wallet/libwally-core")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
set(PROJECT_VER "1.0.14")
project(hello_world)

# TinyUSB calls these callbacks from its static archive. Force the callback
# object out of the application component archive during the final link.
idf_build_set_property(LINK_OPTIONS APPEND
    "-Wl,-u,tud_hid_descriptor_report_cb"
    "-Wl,-u,tud_hid_get_report_cb"
    "-Wl,-u,tud_hid_set_report_cb"
    "-Wl,-u,tud_hid_report_complete_cb"
)

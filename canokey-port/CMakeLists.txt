set(CANOKEY_DIR "${CMAKE_CURRENT_LIST_DIR}/../canokey-core")
set(CANOKEY_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(CANOKEY_TINYCBOR_GENERATED_DIR "${CANOKEY_GENERATED_DIR}/tinycbor")
set(CANOKEY_MBEDTLS_PRIVATE_DIR "${CANOKEY_GENERATED_DIR}/mbedtls/private")

set(CANOKEY_VENDOR_DEPS_READY TRUE)
if(NOT EXISTS "${CANOKEY_DIR}/littlefs/lfs.c")
    set(CANOKEY_VENDOR_DEPS_READY FALSE)
endif()
if(NOT EXISTS "${CANOKEY_DIR}/littlefs/lfs_util.c")
    set(CANOKEY_VENDOR_DEPS_READY FALSE)
endif()
if(NOT EXISTS "${CANOKEY_DIR}/tinycbor/src/cborencoder.c")
    set(CANOKEY_VENDOR_DEPS_READY FALSE)
endif()
if(NOT EXISTS "${CANOKEY_DIR}/tinycbor/src/cborparser.c")
    set(CANOKEY_VENDOR_DEPS_READY FALSE)
endif()

file(MAKE_DIRECTORY "${CANOKEY_GENERATED_DIR}")
file(MAKE_DIRECTORY "${CANOKEY_TINYCBOR_GENERATED_DIR}")
file(MAKE_DIRECTORY "${CANOKEY_MBEDTLS_PRIVATE_DIR}")
file(WRITE "${CANOKEY_GENERATED_DIR}/canokey-core-git-rev.h"
    "#pragma once\n"
    "#define CANOKEY_CORE_GIT_REV \"esp32-p4-port\"\n"
)
file(WRITE "${CANOKEY_TINYCBOR_GENERATED_DIR}/tinycbor-export.h"
    "#ifndef CBOR_API\n"
    "#define CBOR_API\n"
    "#endif\n"
)
file(WRITE "${CANOKEY_TINYCBOR_GENERATED_DIR}/tinycbor-version.h"
    "#define TINYCBOR_VERSION_MAJOR 7\n"
    "#define TINYCBOR_VERSION_MINOR 0\n"
    "#define TINYCBOR_VERSION_PATCH 0\n"
)
file(WRITE "${CANOKEY_MBEDTLS_PRIVATE_DIR}/bignum.h" "#include <mbedtls/bignum.h>\n")
file(WRITE "${CANOKEY_MBEDTLS_PRIVATE_DIR}/ecdsa.h" "#include <mbedtls/ecdsa.h>\n")
file(WRITE "${CANOKEY_MBEDTLS_PRIVATE_DIR}/ecp.h" "#include <mbedtls/ecp.h>\n")

if(CANOKEY_VENDOR_DEPS_READY)
    find_package(Python3 QUIET COMPONENTS Interpreter)
    if(Python3_EXECUTABLE)
        execute_process(
            COMMAND "${Python3_EXECUTABLE}"
                "${CANOKEY_DIR}/scripts/gen_ctap_get_info.py"
                --headers
                    "${CANOKEY_DIR}/applets/ctap/ctap-internal.h"
                    "${CANOKEY_DIR}/applets/ctap/cose-key.h"
                --credential-id-size 70
                --attestation-format none
                --output "${CANOKEY_GENERATED_DIR}/ctap_get_info_cbor.inc"
            RESULT_VARIABLE CANOKEY_GEN_CTAP_RESULT
        )
    endif()

    if(NOT EXISTS "${CANOKEY_GENERATED_DIR}/ctap_get_info_cbor.inc")
        message(WARNING "Failed to generate ctap_get_info_cbor.inc; writing an empty placeholder")
        file(WRITE "${CANOKEY_GENERATED_DIR}/ctap_get_info_cbor.inc" "")
    endif()

    file(GLOB CANOKEY_CORE_SRCS
        "${CANOKEY_DIR}/src/*.c"
        "${CANOKEY_DIR}/applets/admin/*.c"
        "${CANOKEY_DIR}/applets/ctap/*.c"
        "${CANOKEY_DIR}/applets/ndef/*.c"
        "${CANOKEY_DIR}/applets/oath/*.c"
        "${CANOKEY_DIR}/applets/openpgp/*.c"
        "${CANOKEY_DIR}/applets/pass/*.c"
        "${CANOKEY_DIR}/applets/piv/*.c"
        "${CANOKEY_DIR}/interfaces/USB/class/ctaphid/ctaphid.c"
        "${CANOKEY_DIR}/interfaces/NFC/nfc.c"
        "${CANOKEY_DIR}/littlefs/lfs.c"
        "${CANOKEY_DIR}/littlefs/lfs_util.c"
        "${CANOKEY_DIR}/tinycbor/src/cborencoder.c"
        "${CANOKEY_DIR}/tinycbor/src/cborparser.c"
        "${CANOKEY_DIR}/canokey-crypto/src/*.c"
    )
else()
    message(WARNING "CanoKey vendored submodules are missing; building canokey_core placeholder only")
    set(CANOKEY_CORE_SRCS "port_nfc.c" "canokey_esp32p4_port.c" "port_usb.c" "port_flash.c" "port_crypto.c" "${CMAKE_CURRENT_LIST_DIR}/port_ui.c" "${CMAKE_CURRENT_LIST_DIR}/canokey_core_placeholder.c")
endif()

list(APPEND CANOKEY_CORE_SRCS "${CMAKE_CURRENT_LIST_DIR}/canokey_esp32p4_port.c")
list(APPEND CANOKEY_CORE_SRCS "${CMAKE_CURRENT_LIST_DIR}/port_crypto.c")
list(APPEND CANOKEY_CORE_SRCS "${CMAKE_CURRENT_LIST_DIR}/port_ecc_debug.c")
list(APPEND CANOKEY_CORE_SRCS "${CMAKE_CURRENT_LIST_DIR}/port_flash.c")
list(APPEND CANOKEY_CORE_SRCS "${CMAKE_CURRENT_LIST_DIR}/port_fs_debug.c")
list(APPEND CANOKEY_CORE_SRCS "${CMAKE_CURRENT_LIST_DIR}/port_gpg_flash.c")
list(APPEND CANOKEY_CORE_SRCS "${CMAKE_CURRENT_LIST_DIR}/port_random.c")
list(APPEND CANOKEY_CORE_SRCS "${CMAKE_CURRENT_LIST_DIR}/port_time.c")
list(APPEND CANOKEY_CORE_SRCS "${CMAKE_CURRENT_LIST_DIR}/port_ui.c")
list(APPEND CANOKEY_CORE_SRCS "${CMAKE_CURRENT_LIST_DIR}/port_usb.c")
list(APPEND CANOKEY_CORE_SRCS "${CMAKE_CURRENT_LIST_DIR}/port_nfc.c")
list(APPEND CANOKEY_CORE_SRCS "${CMAKE_CURRENT_LIST_DIR}/../piv/piv_crypto.c")
list(APPEND CANOKEY_CORE_SRCS "${CMAKE_CURRENT_LIST_DIR}/../piv/piv_dispatch.c")
list(APPEND CANOKEY_CORE_SRCS "${CMAKE_CURRENT_LIST_DIR}/../piv/piv_pin.c")
list(APPEND CANOKEY_CORE_SRCS "${CMAKE_CURRENT_LIST_DIR}/../piv/piv_port.c")
list(APPEND CANOKEY_CORE_SRCS "${CMAKE_CURRENT_LIST_DIR}/../piv/piv_storage.c")

set(CANOKEY_INCLUDE_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/include"
    "${CANOKEY_DIR}/include"
    "${CANOKEY_DIR}/applets/ctap"
    "${CANOKEY_DIR}/interfaces/USB/class/ctaphid"
    "${CANOKEY_DIR}/interfaces/USB/class/kbdhid"
    "${CANOKEY_DIR}/interfaces/USB/device"
    "${CANOKEY_DIR}/interfaces/USB/core/inc"
    "${CANOKEY_GENERATED_DIR}"
    "${CANOKEY_TINYCBOR_GENERATED_DIR}"
    "${CANOKEY_DIR}/canokey-crypto/include"
)

if(EXISTS "${CANOKEY_DIR}/littlefs")
    list(APPEND CANOKEY_INCLUDE_DIRS "${CANOKEY_DIR}/littlefs")
endif()
if(EXISTS "${CANOKEY_DIR}/tinycbor/src")
    list(APPEND CANOKEY_INCLUDE_DIRS "${CANOKEY_DIR}/tinycbor/src")
endif()

idf_component_register(
    SRCS ${CANOKEY_CORE_SRCS}
    INCLUDE_DIRS ${CANOKEY_INCLUDE_DIRS}
    PRIV_REQUIRES
        esp_timer
        esp_tinyusb
        esp_partition
        freertos
        log
        mbedtls
)

target_compile_definitions(${COMPONENT_LIB}
    PUBLIC
        ENABLE_NFC=1
        NFC_CHIP=-1
        ENABLE_APPLET_NDEF=1
        NDEF_DEFAULT_EMPTY=0
        ENABLE_PASS=0
        ENABLE_IFACE_CTAPHID=1
        ENABLE_IFACE_WEBUSB=0
        ENABLE_IFACE_CCID=0
        ENABLE_IFACE_KBDHID=0
        CTAP_NONE_ATTESTATION=1
         
    PRIVATE
        CBOR_NO_FLOATING_POINT
        CBOR_STATIC_DEFINE
         
)

target_compile_options(${COMPONENT_LIB}
    PRIVATE
        -Wno-error=format
        -Wno-format
        -Wno-error
)

target_link_options(${COMPONENT_LIB}
    INTERFACE
        "-Wl,--wrap=CTAPHID_Loop"
        "-Wl,--wrap=get_touch_result"
        "-Wl,--wrap=write_file"
        "-Wl,--wrap=read_file"
        "-Wl,--wrap=write_attr"
        "-Wl,--wrap=read_attr"
        "-Wl,--wrap=remove_attr"
        "-Wl,--wrap=get_file_size"
        "-Wl,--wrap=remove_file"
        "-Wl,--wrap=admin_platform_serial_read"
        "-Wl,--wrap=pin_create"
        "-Wl,--wrap=pin_verify"
        "-Wl,--wrap=pin_update"
        "-Wl,--wrap=pin_get_size"
        "-Wl,--wrap=pin_get_retries"
        "-Wl,--wrap=pin_get_default_retries"
        "-Wl,--wrap=pin_set_retries"
        "-Wl,--wrap=pin_clear"
        "-Wl,--wrap=piv_process_apdu"
        "-Wl,--wrap=piv_process_apdu_message"
        "-Wl,--wrap=rsa_generate_key"
        "-Wl,--wrap=rsa_get_public_key"
        "-Wl,--wrap=rsa_private"
        "-Wl,--wrap=rsa_sign_pkcs_v15"
        "-Wl,--wrap=tdes_enc"
        "-Wl,--wrap=tdes_dec"
        "-Wl,--wrap=ecdh"
        "-Wl,--wrap=ck_generate_key"
        "-Wl,--wrap=ck_sign"
        "-Wl,--wrap=piv_install"
        "-Wl,--wrap=piv_poweroff"
        "-Wl,--wrap=piv_platform_algorithm_extension_config_read"
        "-Wl,--wrap=piv_platform_algorithm_extension_config_write"
)

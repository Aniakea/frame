idf_component_register(
    SRCS
        "abi_smoke.cc"
        "poc_a_main.cc"
        "poca_console.cc"
        "poca_plugin.cc"
    INCLUDE_DIRS "."
    PRIV_REQUIRES
        console
        elf_loader
        esp_hw_support
        frame_poc_mpb
        frame_poc_runtime
        mbedtls
)

include("${CMAKE_CURRENT_LIST_DIR}/../plugins/plugins.cmake")

poca_pubkey_header(
    "${CMAKE_CURRENT_LIST_DIR}/../../../../tools/frame_tools/testdata/keys/poc_a_test_signing_key.pub.pem"
    "${CMAKE_BINARY_DIR}/plugins/generated/poca_pubkey.h"
    "0xe5677a1c")

poca_plugin(baseline
    SOURCE "${CMAKE_CURRENT_LIST_DIR}/../plugins/baseline/baseline.c"
    VERSION "1.0.0"
    ENTRY "frame_plugin_entry"
    MANIFEST "${CMAKE_CURRENT_LIST_DIR}/../plugins/baseline/manifest.json")

target_include_directories(${COMPONENT_LIB} PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../plugins"
    "${CMAKE_BINARY_DIR}/plugins/generated")

add_dependencies(${COMPONENT_LIB} poca_plugin_baseline)
target_add_binary_data(${COMPONENT_LIB} "${CMAKE_BINARY_DIR}/plugins/baseline.mpb" BINARY)

target_compile_options(${COMPONENT_LIB} PRIVATE
    -std=c++26
    -Wall
    -Wextra
    -Wconversion
    -Werror=return-type
    -fno-exceptions
    -fno-rtti
    -fno-unwind-tables
)

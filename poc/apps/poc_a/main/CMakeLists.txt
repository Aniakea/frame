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

poca_plugin(globdat
    SOURCE "${CMAKE_CURRENT_LIST_DIR}/../plugins/globdat/globdat.c"
    VERSION "1.0.0"
    ENTRY "frame_plugin_entry"
    MANIFEST "${CMAKE_CURRENT_LIST_DIR}/../plugins/globdat/manifest.json"
    IMPORTS "poca_host_sentinel")

poca_plugin(plt
    SOURCE "${CMAKE_CURRENT_LIST_DIR}/../plugins/plt/plt.c"
    VERSION "1.0.0"
    ENTRY "frame_plugin_entry"
    MANIFEST "${CMAKE_CURRENT_LIST_DIR}/../plugins/plt/manifest.json"
    IMPORTS "poca_host_add")

poca_plugin(import_neg
    SOURCE "${CMAKE_CURRENT_LIST_DIR}/../plugins/import_neg/import_neg.c"
    VERSION "1.0.0"
    ENTRY "frame_plugin_entry"
    MANIFEST "${CMAKE_CURRENT_LIST_DIR}/../plugins/import_neg/manifest.json"
    IMPORTS "frame_unknown_symbol")

poca_negative_corpus(
    BASELINE_ELF "${CMAKE_BINARY_DIR}/plugins/baseline/baseline.elf")
# negatives derive from the gated baseline ELF: gate first, then fabricate
add_dependencies(poca_negative_corpus poca_plugin_baseline)

target_include_directories(${COMPONENT_LIB} PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../plugins"
    "${CMAKE_BINARY_DIR}/plugins/generated")

add_dependencies(${COMPONENT_LIB}
    poca_plugin_baseline
    poca_plugin_globdat
    poca_plugin_plt
    poca_plugin_import_neg
    poca_negative_corpus)
target_add_binary_data(${COMPONENT_LIB} "${CMAKE_BINARY_DIR}/plugins/baseline.mpb" BINARY)
target_add_binary_data(${COMPONENT_LIB} "${CMAKE_BINARY_DIR}/plugins/globdat.mpb" BINARY)
target_add_binary_data(${COMPONENT_LIB} "${CMAKE_BINARY_DIR}/plugins/plt.mpb" BINARY)
target_add_binary_data(${COMPONENT_LIB} "${CMAKE_BINARY_DIR}/plugins/import_neg.mpb" BINARY)
target_add_binary_data(${COMPONENT_LIB} "${CMAKE_BINARY_DIR}/plugins/neg_r32.mpb" BINARY)
target_add_binary_data(${COMPONENT_LIB} "${CMAKE_BINARY_DIR}/plugins/neg_s0op.mpb" BINARY)
target_add_binary_data(${COMPONENT_LIB} "${CMAKE_BINARY_DIR}/plugins/neg_phspan.mpb" BINARY)
target_add_binary_data(${COMPONENT_LIB} "${CMAKE_BINARY_DIR}/plugins/neg_phbe.mpb" BINARY)
target_add_binary_data(${COMPONENT_LIB} "${CMAKE_BINARY_DIR}/plugins/neg_ph64.mpb" BINARY)
target_add_binary_data(${COMPONENT_LIB} "${CMAKE_BINARY_DIR}/plugins/neg_phmach.mpb" BINARY)

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

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

# T7 IRAM probe: hot functions in .plugin_iram (gate 5 + objdump l32r check
# in the corpus step below).
string(REPLACE "-gcc" "-objdump" plugin_objdump "${CMAKE_C_COMPILER}")
poca_plugin(iram_probe
    SOURCE "${CMAKE_CURRENT_LIST_DIR}/../plugins/iram_probe/iram_probe.c"
    VERSION "1.0.0"
    ENTRY "frame_plugin_entry"
    MANIFEST "${CMAKE_CURRENT_LIST_DIR}/../plugins/iram_probe/manifest.json"
    IRAM)
poca_iram_corpus(
    PROBE_ELF "${CMAKE_BINARY_DIR}/plugins/iram_probe/iram_probe.elf"
    OBJDUMP "${plugin_objdump}")
add_dependencies(poca_iram_corpus poca_plugin_iram_probe)

poca_negative_corpus(
    BASELINE_ELF "${CMAKE_BINARY_DIR}/plugins/baseline/baseline.elf")
# negatives derive from the gated baseline ELF: gate first, then fabricate
add_dependencies(poca_negative_corpus poca_plugin_baseline)

# T8 generation coexistence: baseline_v2 is the SAME source packaged as the
# same manifest name "baseline" with version 2.0.0 and a distinct activate()
# magic; baseline_300k proves a >256KiB default-arena declaration (300KiB)
# is admission-requestable under the 512KiB hard max (MEM-003); neg_maxmem
# hostile-declares 600KiB and must be rejected on-board before esp_elf_init.
poca_plugin(baseline_v2
    SOURCE "${CMAKE_CURRENT_LIST_DIR}/../plugins/baseline/baseline.c"
    VERSION "2.0.0"
    ENTRY "frame_plugin_entry"
    MANIFEST "${CMAKE_CURRENT_LIST_DIR}/../plugins/baseline_v2/manifest.json"
    MPB_NAME "baseline"
    MAGIC "0x32504F43u")
poca_plugin(baseline_300k
    SOURCE "${CMAKE_CURRENT_LIST_DIR}/../plugins/baseline/baseline.c"
    VERSION "1.0.1"
    ENTRY "frame_plugin_entry"
    MANIFEST "${CMAKE_CURRENT_LIST_DIR}/../plugins/baseline_300k/manifest.json"
    MPB_NAME "baseline")
poca_maxmem_corpus(
    BASELINE_ELF "${CMAKE_BINARY_DIR}/plugins/baseline/baseline.elf")
add_dependencies(poca_maxmem_corpus poca_plugin_baseline)

# T6 C++ feature-rejection probes: each variant must be REJECTED by the
# same gates that protect the positive builds (expected-failure wrapper
# echoes the verbatim gate error). NEEDLES name the required rejection
# reason (plain substrings); gate order is reloc-allowlist -> C++-sections
# -> imports -> shape.
poca_cxx_probe(cxx_ctor
    SOURCE "${CMAKE_CURRENT_LIST_DIR}/../plugins/cxx_probes/cxx_static_ctor.cpp"
    ENTRY "frame_plugin_entry"
    NEEDLES "forbidden C++ feature section" ".ctors")
poca_cxx_probe(cxx_dtor
    SOURCE "${CMAKE_CURRENT_LIST_DIR}/../plugins/cxx_probes/cxx_static_dtor.cpp"
    ENTRY "frame_plugin_entry"
    NEEDLES "forbidden C++ feature section" ".dtors")
poca_cxx_probe(cxx_tls
    SOURCE "${CMAKE_CURRENT_LIST_DIR}/../plugins/cxx_probes/cxx_tls.cpp"
    ENTRY "frame_plugin_entry"
    NEEDLES "relocation allowlist violated" "R_XTENSA_TLS")
poca_cxx_probe(cxx_exceptions
    SOURCE "${CMAKE_CURRENT_LIST_DIR}/../plugins/cxx_probes/cxx_exceptions.cpp"
    ENTRY "frame_plugin_entry"
    EXTRA_FLAGS -fexceptions -funwind-tables
    NEEDLES "forbidden C++ feature section" ".eh_frame")
poca_cxx_probe(cxx_rtti
    SOURCE "${CMAKE_CURRENT_LIST_DIR}/../plugins/cxx_probes/cxx_rtti.cpp"
    ENTRY "frame_plugin_entry"
    EXTRA_FLAGS -frtti
    NEEDLES "import gate failed" "__dynamic_cast")
poca_cxx_probe(cxx_unwind
    SOURCE "${CMAKE_CURRENT_LIST_DIR}/../plugins/cxx_probes/cxx_unwind.cpp"
    ENTRY "frame_plugin_entry"
    EXTRA_FLAGS -funwind-tables -fasynchronous-unwind-tables
    NEEDLES "forbidden C++ feature section" ".eh_frame")

poca_cxx_probe_corpus(
    CTOR_ELF "${CMAKE_BINARY_DIR}/plugins/cxx_ctor/cxx_ctor.elf"
    TLS_ELF "${CMAKE_BINARY_DIR}/plugins/cxx_tls/cxx_tls.elf")
add_dependencies(poca_cxx_probe_corpus poca_cxx_probe_cxx_ctor poca_cxx_probe_cxx_tls)

target_include_directories(${COMPONENT_LIB} PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../plugins"
    "${CMAKE_BINARY_DIR}/plugins/generated")

add_dependencies(${COMPONENT_LIB}
    poca_plugin_baseline
    poca_plugin_baseline_v2
    poca_plugin_baseline_300k
    poca_plugin_globdat
    poca_plugin_plt
    poca_plugin_import_neg
    poca_plugin_iram_probe
    poca_negative_corpus
    poca_maxmem_corpus
    poca_cxx_probe_cxx_ctor
    poca_cxx_probe_cxx_dtor
    poca_cxx_probe_cxx_tls
    poca_cxx_probe_cxx_exceptions
    poca_cxx_probe_cxx_rtti
    poca_cxx_probe_cxx_unwind
    poca_cxx_probe_corpus
    poca_iram_corpus)
target_add_binary_data(${COMPONENT_LIB} "${CMAKE_BINARY_DIR}/plugins/baseline.mpb" BINARY)
target_add_binary_data(${COMPONENT_LIB} "${CMAKE_BINARY_DIR}/plugins/baseline_v2.mpb" BINARY)
target_add_binary_data(${COMPONENT_LIB} "${CMAKE_BINARY_DIR}/plugins/baseline_300k.mpb" BINARY)
target_add_binary_data(${COMPONENT_LIB} "${CMAKE_BINARY_DIR}/plugins/globdat.mpb" BINARY)
target_add_binary_data(${COMPONENT_LIB} "${CMAKE_BINARY_DIR}/plugins/plt.mpb" BINARY)
target_add_binary_data(${COMPONENT_LIB} "${CMAKE_BINARY_DIR}/plugins/import_neg.mpb" BINARY)
target_add_binary_data(${COMPONENT_LIB} "${CMAKE_BINARY_DIR}/plugins/iram_probe.mpb" BINARY)
target_add_binary_data(${COMPONENT_LIB} "${CMAKE_BINARY_DIR}/plugins/iram_mismatch.mpb" BINARY)
target_add_binary_data(${COMPONENT_LIB} "${CMAKE_BINARY_DIR}/plugins/neg_r32.mpb" BINARY)
target_add_binary_data(${COMPONENT_LIB} "${CMAKE_BINARY_DIR}/plugins/neg_s0op.mpb" BINARY)
target_add_binary_data(${COMPONENT_LIB} "${CMAKE_BINARY_DIR}/plugins/neg_phspan.mpb" BINARY)
target_add_binary_data(${COMPONENT_LIB} "${CMAKE_BINARY_DIR}/plugins/neg_phbe.mpb" BINARY)
target_add_binary_data(${COMPONENT_LIB} "${CMAKE_BINARY_DIR}/plugins/neg_ph64.mpb" BINARY)
target_add_binary_data(${COMPONENT_LIB} "${CMAKE_BINARY_DIR}/plugins/neg_phmach.mpb" BINARY)
target_add_binary_data(${COMPONENT_LIB} "${CMAKE_BINARY_DIR}/plugins/neg_maxmem.mpb" BINARY)
target_add_binary_data(${COMPONENT_LIB} "${CMAKE_BINARY_DIR}/plugins/cxx_ctor.mpb" BINARY)
target_add_binary_data(${COMPONENT_LIB} "${CMAKE_BINARY_DIR}/plugins/cxx_tls.mpb" BINARY)
target_add_binary_data(${COMPONENT_LIB} "${CMAKE_BINARY_DIR}/plugins/neg_tls_nosect.mpb" BINARY)

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

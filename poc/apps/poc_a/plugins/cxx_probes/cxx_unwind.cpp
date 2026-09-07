/*
 * C++ feature-rejection probe v6: unwind tables (task T6 of poc-a-dynamic-elf,
 * appendix C 11.2/11.3).
 *
 * Compiled with -funwind-tables -fasynchronous-unwind-tables (and the C++
 * EH flags deliberately OFF): every function - including the noinline
 * marker below - gets an .eh_frame FDE. No exception code, no EH runtime
 * imports: the purest emitter of the forbidden unwind-metadata section,
 * which gate 4 rejects at build time and loader patch p4 rejects on
 * target before any plugin byte executes.
 */

#include "plugin_abi.h"

#define CXX_PROBE_ERR_ABI_MISMATCH (-7)

__attribute__((noinline)) static uint32_t unwind_marker(uint32_t value) {
    return value ^ 0x0BADCAFEu;
}

static int32_t unwind_probe_prepare(void) { return 0; }

static int32_t unwind_probe_activate(void) {
    const uint32_t crc = poca_crc32(POCA_BASELINE_PATTERN, POCA_BASELINE_PATTERN_SIZE);
    const uint32_t marked = unwind_marker(crc);
    if (marked != (crc ^ 0x0BADCAFEu)) {
        return CXX_PROBE_ERR_ABI_MISMATCH;
    }
    return static_cast<int32_t>(POCA_PLUGIN_MAGIC ^ marked);
}

static int32_t unwind_probe_unload(void) { return 0; }

static const poca_plugin_table_t k_unwind_table = {
    .struct_size = (uint32_t)sizeof(poca_plugin_table_t),
    .abi_major = POCA_PLUGIN_ABI_MAJOR,
    .abi_minor = POCA_PLUGIN_ABI_MINOR,
    .prepare = &unwind_probe_prepare,
    .activate = &unwind_probe_activate,
    .unload = &unwind_probe_unload,
    .state_schema_id = 0u,
    .state_schema_version = 0u,
    .host_sentinel = nullptr,
    .host_add = nullptr,
    .iram_check = nullptr,
    .iram_write = nullptr,
    .data_word_ptr = nullptr,
    .data_word_read = nullptr,
};

extern "C" __attribute__((visibility("default"))) const poca_plugin_table_t*
frame_plugin_entry(void) {
    return &k_unwind_table;
}

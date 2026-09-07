/*
 * C++ feature-rejection probe v4: exceptions (task T6 of poc-a-dynamic-elf,
 * appendix C 11.2/11.3).
 *
 * Compiled with -fexceptions (plus -funwind-tables, which this toolchain
 * requires to emit the landing-pad tables at -Os): a throw/catch pair in
 * activate() emits .gcc_except_table and .eh_frame unwind metadata and
 * leaves the C++ EH runtime imports (__cxa_*, _Unwind_Resume,
 * __gxx_personality_v0) undefined. The PoC-A loader has no unwinder; the
 * sections are the forbidden artifact (build gate 4), the imports would
 * additionally fail the closed-world import gate (gate 2) with -ENOSYS on
 * target.
 */

#include "plugin_abi.h"

#define CXX_PROBE_ERR_ABI_MISMATCH (-7)
#define CXX_PROBE_ERR_PACKAGE_INVALID (-18)

static int32_t exc_probe_prepare(void) { return 0; }

static int32_t exc_probe_activate(void) {
    const uint32_t crc = poca_crc32(POCA_BASELINE_PATTERN, POCA_BASELINE_PATTERN_SIZE);
    uint32_t code = 0u;
    try {
        throw 0xE77C37;
    } catch (int caught) {
        code = static_cast<uint32_t>(caught) & 0x00FFFFFFu;
    }
    if (code != 0x00E77C37u) {
        return CXX_PROBE_ERR_PACKAGE_INVALID;
    }
    return static_cast<int32_t>(POCA_PLUGIN_MAGIC ^ crc ^ code);
}

static int32_t exc_probe_unload(void) { return 0; }

static const poca_plugin_table_t k_exc_table = {
    .struct_size = (uint32_t)sizeof(poca_plugin_table_t),
    .abi_major = POCA_PLUGIN_ABI_MAJOR,
    .abi_minor = POCA_PLUGIN_ABI_MINOR,
    .prepare = &exc_probe_prepare,
    .activate = &exc_probe_activate,
    .unload = &exc_probe_unload,
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
    return &k_exc_table;
}

/*
 * C++ feature-rejection probe v3: dynamic TLS (task T6 of poc-a-dynamic-elf,
 * appendix C 11.2/11.3).
 *
 * Two __thread variables, written and read at runtime (a never-written
 * thread-local is constant-folded away by gcc and emits nothing, so the
 * runtime store is load-bearing for the probe). Under the pipeline link
 * flags this emits .tdata/.tbss sections plus R_XTENSA_TLSDESC_FN /
 * R_XTENSA_TLSDESC_ARG dynamic relocations in .rela.dyn:
 *  - build gate 1 rejects the relocation types (out of the loader's
 *    xtensa allowlist),
 *  - build gate 4 rejects the TLS sections,
 *  - on target, loader patch p4 rejects the sections pre-execute and
 *    (with the sections renamed away, the neg_tls_nosect corpus row) patch
 *    p1 rejects the relocation types with -EINVAL.
 */

#include "plugin_abi.h"

#define CXX_PROBE_ERR_ABI_MISMATCH (-7)

static __thread uint32_t g_tls_slot = 0x71557003u;
static __thread uint32_t g_tls_zero = 0u;

__attribute__((noinline)) static uint32_t tls_stage(uint32_t seed) {
    g_tls_slot = seed ^ 0x71557003u;
    g_tls_zero = seed + 1u;
    return 0u;
}

static int32_t tls_probe_prepare(void) { return 0; }

static int32_t tls_probe_activate(void) {
    (void)tls_stage(0xC7557001u);
    const uint32_t crc = poca_crc32(POCA_BASELINE_PATTERN, POCA_BASELINE_PATTERN_SIZE);
    return static_cast<int32_t>(POCA_PLUGIN_MAGIC ^ crc ^ g_tls_slot ^ g_tls_zero);
}

static int32_t tls_probe_unload(void) { return 0; }

static const poca_plugin_table_t k_tls_table = {
    .struct_size = (uint32_t)sizeof(poca_plugin_table_t),
    .abi_major = POCA_PLUGIN_ABI_MAJOR,
    .abi_minor = POCA_PLUGIN_ABI_MINOR,
    .prepare = &tls_probe_prepare,
    .activate = &tls_probe_activate,
    .unload = &tls_probe_unload,
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
    return &k_tls_table;
}

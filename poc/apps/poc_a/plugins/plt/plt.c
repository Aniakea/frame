/*
 * R_XTENSA_JMP_SLOT positive probe (task T5 of poc-a-dynamic-elf).
 *
 * Calls the firmware-registered host function poca_host_add directly from
 * plugin code; with the pipeline's -fPIC -shared flags the xtensa backend
 * routes every direct extern call through the PLT, emitting
 * R_XTENSA_JMP_SLOT relocations that patch call literal words inside
 * .text (the "GOT slot" of the xtensa PLT lives in the text literal pool,
 * so the loader's .text window covers it).
 */

#include "plugin_abi.h"

extern uint32_t poca_host_add(uint32_t a, uint32_t b);

static int32_t plt_prepare(void);
static int32_t plt_activate(void);
static int32_t plt_unload(void);

static const poca_plugin_table_t k_plt_table = {
    .struct_size = (uint32_t)sizeof(poca_plugin_table_t),
    .abi_major = POCA_PLUGIN_ABI_MAJOR,
    .abi_minor = POCA_PLUGIN_ABI_MINOR,
    .prepare = &plt_prepare,
    .activate = &plt_activate,
    .unload = &plt_unload,
    .state_schema_id = 0u,
    .state_schema_version = 0u,
    .host_sentinel = 0,
    .host_add = &poca_host_add, /* GLOB_DAT value-probe slot for the same import */
};

static int32_t plt_prepare(void) {
    if (k_plt_table.host_add == 0) {
        return POCA_ERR_PACKAGE_INVALID;
    }
    uint32_t roundtrip = poca_host_add(1u, 2u);
    if (roundtrip != poca_host_add_value(1u, 2u)) {
        return POCA_ERR_HASH_MISMATCH;
    }
    return 0;
}

static int32_t plt_activate(void) {
    uint32_t crc = poca_crc32(POCA_BASELINE_PATTERN, POCA_BASELINE_PATTERN_SIZE);
    /* two distinct direct call sites so the PLT path is exercised twice
     * with different arguments; both go through JMP_SLOT-patched literals */
    uint32_t left = poca_host_add(crc, 0u);
    uint32_t right = poca_host_add(POCA_HOST_ADD_DELTA, crc);
    return (int32_t)(POCA_PLUGIN_MAGIC ^ left ^ right);
}

static int32_t plt_unload(void) { return 0; }

__attribute__((visibility("default"))) const poca_plugin_table_t* frame_plugin_entry(void) {
    return &k_plt_table;
}

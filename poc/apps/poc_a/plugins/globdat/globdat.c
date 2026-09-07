/*
 * R_XTENSA_GLOB_DAT positive probe (task T5 of poc-a-dynamic-elf).
 *
 * Imports the firmware-registered host function poca_host_sentinel and
 * stores its address in the (const) entry table, which lands in
 * .data.rel.ro: the linker emits an R_XTENSA_GLOB_DAT dynamic relocation
 * for the word and no PLT is involved. The harness asserts the relocated
 * word equals the firmware's own &poca_host_sentinel (exact relocation
 * VALUE check) and activate() proves the plugin can call through it.
 */

#include "plugin_abi.h"

extern uint32_t poca_host_sentinel(void);

static int32_t globdat_prepare(void);
static int32_t globdat_activate(void);
static int32_t globdat_unload(void);

/* volatile: keeps the call through the relocated table word instead of
 * letting the compiler devirtualize it into a direct PLT call */
static uint32_t (*volatile globdat_fn)(void);

static const poca_plugin_table_t k_globdat_table = {
    .struct_size = (uint32_t)sizeof(poca_plugin_table_t),
    .abi_major = POCA_PLUGIN_ABI_MAJOR,
    .abi_minor = POCA_PLUGIN_ABI_MINOR,
    .prepare = &globdat_prepare,
    .activate = &globdat_activate,
    .unload = &globdat_unload,
    .state_schema_id = 0u,
    .state_schema_version = 0u,
    .host_sentinel = &poca_host_sentinel, /* the R_XTENSA_GLOB_DAT slot */
    .host_add = 0,
};

static int32_t globdat_prepare(void) {
    if (k_globdat_table.host_sentinel == 0) {
        return POCA_ERR_PACKAGE_INVALID;
    }
    globdat_fn = k_globdat_table.host_sentinel;
    if (globdat_fn() != poca_host_sentinel_value()) {
        return POCA_ERR_HASH_MISMATCH;
    }
    return 0;
}

static int32_t globdat_activate(void) {
    return (int32_t)(POCA_PLUGIN_MAGIC ^
                     poca_crc32(POCA_BASELINE_PATTERN, POCA_BASELINE_PATTERN_SIZE) ^ globdat_fn());
}

static int32_t globdat_unload(void) { return 0; }

__attribute__((visibility("default"))) const poca_plugin_table_t* frame_plugin_entry(void) {
    return &k_globdat_table;
}

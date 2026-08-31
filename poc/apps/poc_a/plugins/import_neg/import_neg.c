/*
 * Import-allowlist negative probe (task T5 of poc-a-dynamic-elf).
 *
 * Imports frame_unknown_symbol, a name that is deliberately NOT registered
 * with the loader's symbol tables. The pipeline's per-plugin IMPORTS
 * declaration lets the build finish (build-side intent declaration); the
 * device-side resolver must refuse the load with -ENOSYS at relocation
 * time, before a single plugin byte executes.
 */

#include "plugin_abi.h"

extern uint32_t frame_unknown_symbol(uint32_t seed);

static int32_t import_neg_prepare(void);
static int32_t import_neg_activate(void);
static int32_t import_neg_unload(void);

static const poca_plugin_table_t k_import_neg_table = {
    .struct_size = (uint32_t)sizeof(poca_plugin_table_t),
    .abi_major = POCA_PLUGIN_ABI_MAJOR,
    .abi_minor = POCA_PLUGIN_ABI_MINOR,
    .prepare = &import_neg_prepare,
    .activate = &import_neg_activate,
    .unload = &import_neg_unload,
    .state_schema_id = 0u,
    .state_schema_version = 0u,
    .host_sentinel = 0,
    .host_add = 0,
};

static int32_t import_neg_prepare(void) { return frame_unknown_symbol(1u); }

static int32_t import_neg_activate(void) { return (int32_t)frame_unknown_symbol(2u); }

static int32_t import_neg_unload(void) { return 0; }

__attribute__((visibility("default"))) const poca_plugin_table_t* frame_plugin_entry(void) {
    return &k_import_neg_table;
}

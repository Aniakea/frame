/*
 * Baseline PoC-A plugin (task T4 of poc-a-dynamic-elf).
 *
 * Plain C, no libc/libgcc references, single exported query entry
 * frame_plugin_entry; everything else is hidden (-fvisibility=hidden) and
 * internal. The plugin is compiled and linked by plugins/plugins.cmake with
 * the flag set mirrored from the vendored elf_loader component's
 * project_elf()/project_so() machinery so that the emitted dynamic
 * relocations stay inside the loader's narrow allowlist
 * (R_XTENSA_RELATIVE / RTLD / GLOB_DAT / JMP_SLOT).
 */

#include "plugin_abi.h"

/* frame_err_t-style result codes used by the self-checks (poc/common/
 * frame_poc_runtime frame_abi.h values; kept literal to keep the plugin
 * decoupled from firmware headers). */
#define BASELINE_ERR_ABI_MISMATCH (-7)
#define BASELINE_ERR_PACKAGE_INVALID (-18)
#define BASELINE_ERR_HASH_MISMATCH (-19)

/* Generation-variant magic (task T8 coexistence): the same source is packaged
 * as baseline v1 (default magic) and baseline_v2 v2.0.0 with a different
 * activate() magic constant, so concurrent calls into two resident
 * generations are distinguishable by value, not only by address. */
#ifndef POCA_BASELINE_ACTIVATE_MAGIC
#define POCA_BASELINE_ACTIVATE_MAGIC POCA_PLUGIN_MAGIC
#endif

static int32_t baseline_prepare(void);
static int32_t baseline_activate(void);
static int32_t baseline_unload(void);

static const poca_plugin_table_t k_baseline_table = {
    .struct_size = (uint32_t)sizeof(poca_plugin_table_t),
    .abi_major = POCA_PLUGIN_ABI_MAJOR,
    .abi_minor = POCA_PLUGIN_ABI_MINOR,
    .prepare = &baseline_prepare,
    .activate = &baseline_activate,
    .unload = &baseline_unload,
    .state_schema_id = 0u,
    .state_schema_version = 0u,
};

static int32_t baseline_prepare(void) {
    /* Runs at the relocated image: validates the entry table it was reached
     * through and the pattern bytes in its own .rodata window. */
    if (k_baseline_table.struct_size != (uint32_t)sizeof(poca_plugin_table_t)) {
        return BASELINE_ERR_PACKAGE_INVALID;
    }
    if (k_baseline_table.abi_major != POCA_PLUGIN_ABI_MAJOR ||
        k_baseline_table.abi_minor != POCA_PLUGIN_ABI_MINOR) {
        return BASELINE_ERR_ABI_MISMATCH;
    }
    if (k_baseline_table.prepare == 0 || k_baseline_table.activate == 0 ||
        k_baseline_table.unload == 0) {
        return BASELINE_ERR_PACKAGE_INVALID;
    }
    if (POCA_BASELINE_PATTERN[0] != 0xA5u || POCA_BASELINE_PATTERN[63] != 0x5Au) {
        return BASELINE_ERR_PACKAGE_INVALID;
    }
    /* First call builds the CRC table in .bss, second call proves it is
     * stable; both read the pattern from the relocated .rodata window. */
    uint32_t first = poca_crc32(POCA_BASELINE_PATTERN, POCA_BASELINE_PATTERN_SIZE);
    uint32_t second = poca_crc32(POCA_BASELINE_PATTERN, POCA_BASELINE_PATTERN_SIZE);
    if (first == 0u || first != second) {
        return BASELINE_ERR_HASH_MISMATCH;
    }
    return 0;
}

static int32_t baseline_activate(void) {
    /* The proof value: computed by THIS code, at THIS loaded address, over
     * THIS image's pattern bytes. The firmware recomputes it over its own
     * flash copy of the same constants and must observe the same value. */
    return (int32_t)(POCA_BASELINE_ACTIVATE_MAGIC ^
                     poca_crc32(POCA_BASELINE_PATTERN, POCA_BASELINE_PATTERN_SIZE));
}

static int32_t baseline_unload(void) {
    /* Stateless plugin: nothing to release. */
    return 0;
}

__attribute__((visibility("default"))) const poca_plugin_table_t* frame_plugin_entry(void) {
    return &k_baseline_table;
}

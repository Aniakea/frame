/*
 * IRAM probe plugin (task T7 of poc-a-dynamic-elf).
 *
 * Same freestanding C contract as the baseline plugin (plugins.cmake
 * pipeline, narrow relocation allowlist), plus a .plugin_iram section: the
 * two iram_* functions below are placed into internal executable RAM by
 * the patched loader and the entry table hands their native 0x40.. window
 * addresses back to the firmware.
 *
 * IRAM SECTION CONTRACT (violation = runtime-only fault, asserted at build
 * time by build_iram_corpus.py): the iram_* functions must contain no l32r
 * literal and no static reference. Every large constant arrives as a
 * parameter; an l32r pool for .plugin_iram code would be placed in the
 * PSRAM text window, beyond the 256 KB backward-only l32r reach from the
 * IRAM window.
 */

#include "plugin_abi.h"

#define IRAM_PROBE_ERR_ABI_MISMATCH (-7)
#define IRAM_PROBE_ERR_PACKAGE_INVALID (-18)
#define IRAM_PROBE_ERR_HASH_MISMATCH (-19)

static int32_t iram_probe_prepare(void);
static int32_t iram_probe_activate(void);
static int32_t iram_probe_unload(void);
static uint32_t iram_check_impl(uint32_t magic, uint32_t mult, uint32_t seed, uint32_t poly,
                                uint32_t words);
static int32_t iram_write_impl(uint32_t* dst, uint32_t value);
static uint32_t* iram_data_word_ptr(void);
static uint32_t iram_data_word_read(void);

/* The write-visibility probe target: a plain .data word, so the loader
 * places it in the PSRAM data window and IRAM-resident stores to it must
 * be observable through every other window. */
static uint32_t g_probe_word = 0x13572468u;

__attribute__((section(".plugin_iram"), noinline, used)) static uint32_t
iram_check_impl(uint32_t magic, uint32_t mult, uint32_t seed, uint32_t poly, uint32_t words) {
    /* Check value: magic ^ fold over a small table built on the stack from
     * the caller-chosen parameters (poca_iram_fold in plugin_abi.h defines
     * the algorithm; the firmware recomputes the expected value with its
     * own flash-resident copy of the same code). */
    uint32_t tbl[16];
    uint32_t acc = seed;
    for (uint32_t i = 0; i < words && i < 16u; ++i) {
        acc = acc * mult + i + 1u;
        tbl[i] = acc ^ (poly >> (i & 7u));
    }
    uint32_t v = magic;
    for (uint32_t i = 0; i < words && i < 16u; ++i) {
        v = (v << 3) | (v >> 29);
        v ^= tbl[i] + magic;
    }
    return v ^ seed;
}

__attribute__((section(".plugin_iram"), noinline, used)) static int32_t
iram_write_impl(uint32_t* dst, uint32_t value) {
    *dst = value;
    return 0;
}

static uint32_t* iram_data_word_ptr(void) { return &g_probe_word; }

static uint32_t iram_data_word_read(void) { return g_probe_word; }

static int32_t iram_probe_prepare(void) {
    if (POCA_BASELINE_PATTERN[0] != 0xA5u || POCA_BASELINE_PATTERN[63] != 0x5Au) {
        return IRAM_PROBE_ERR_PACKAGE_INVALID;
    }
    uint32_t first = poca_crc32(POCA_BASELINE_PATTERN, POCA_BASELINE_PATTERN_SIZE);
    if (first == 0u) {
        return IRAM_PROBE_ERR_HASH_MISMATCH;
    }
    return 0;
}

static int32_t iram_probe_activate(void) {
    return (int32_t)(POCA_PLUGIN_MAGIC ^
                     poca_crc32(POCA_BASELINE_PATTERN, POCA_BASELINE_PATTERN_SIZE));
}

static int32_t iram_probe_unload(void) { return 0; }

static const poca_plugin_table_t k_iram_table = {
    .struct_size = (uint32_t)sizeof(poca_plugin_table_t),
    .abi_major = POCA_PLUGIN_ABI_MAJOR,
    .abi_minor = POCA_PLUGIN_ABI_MINOR,
    .prepare = &iram_probe_prepare,
    .activate = &iram_probe_activate,
    .unload = &iram_probe_unload,
    .state_schema_id = 0u,
    .state_schema_version = 0u,
    .iram_check = &iram_check_impl,
    .iram_write = &iram_write_impl,
    .data_word_ptr = &iram_data_word_ptr,
    .data_word_read = &iram_data_word_read,
};

__attribute__((visibility("default"))) const poca_plugin_table_t* frame_plugin_entry(void) {
    return &k_iram_table;
}

/*
 * Shared contract between the poc_a plugin build pipeline and the poc_a
 * firmware harness (task T4 of poc-a-dynamic-elf).
 *
 * This header is compiled into BOTH sides from the same source tree:
 *   - plugins/baseline/baseline.c builds it into the plugin ELF that is
 *     packaged, signed and executed from PSRAM;
 *   - main/poca_plugin.cc builds it into the firmware to compute the
 *     expected self-check value over an identical copy of the pattern bytes.
 *
 * Keep this header plain C (no C++ runtime features, appendix C 11.2) and
 * free of any library call: the plugin is linked -nostdlib and every symbol
 * it references must be defined inside the plugin itself.
 */

#ifndef POC_A_PLUGINS_PLUGIN_ABI_H
#define POC_A_PLUGINS_PLUGIN_ABI_H

#include <stddef.h>
#include <stdint.h>

/* Entry-table ABI of the PoC-A plugin contract (plan section 4.14.2 minimal
 * subset: prepare/activate/unload, state_schema all zero). Minor 2 adds the
 * IRAM probe slots at the end of the table (task T7); designated
 * initializers leave them null for plugins without a .plugin_iram section. */
#define POCA_PLUGIN_ABI_MAJOR 1
#define POCA_PLUGIN_ABI_MINOR 2

/* The fixed name of the single exported query entry. The pipeline links the
 * plugin with -e frame_plugin_entry and every other symbol hidden. */
#define POCA_PLUGIN_ENTRY_NAME "frame_plugin_entry"

/* 32-bit magic stamped into every activate() self-check value. */
#define POCA_PLUGIN_MAGIC 0x504F4341u /* "POCA" */

/* Host-side import contract (task T5 relocation/import allowlist matrix).
 * The firmware registers these under their exact C names through
 * esp_elf_register_symbol(); plugins that import them get GLOB_DAT
 * (address taken into the entry table) and JMP_SLOT (direct call) dynamic
 * relocations, both inside the loader allowlist. The pure integer
 * implementations live here so the firmware can compute the expected
 * activate() value without calling plugin code. */
#define POCA_HOST_SENTINEL_VALUE 0x5EEDC0DEu
#define POCA_HOST_ADD_MUL 7u
#define POCA_HOST_ADD_DELTA 1234u

static inline uint32_t poca_host_sentinel_value(void) { return POCA_HOST_SENTINEL_VALUE; }

static inline uint32_t poca_host_add_value(uint32_t a, uint32_t b) {
    return a * POCA_HOST_ADD_MUL + b;
}

/* frame_err_t-style result codes for plugin self-checks (poc/common/
 * frame_poc_runtime frame_abi.h values; kept literal to keep the plugin
 * decoupled from firmware headers). */
#define POCA_ERR_ABI_MISMATCH (-7)
#define POCA_ERR_PACKAGE_INVALID (-18)
#define POCA_ERR_HASH_MISMATCH (-19)

/* 64 fixed pattern bytes over which the plugin computes its CRC32 at
 * activation time. The firmware computes the CRC over its own identical copy
 * (compiled from this header) and asserts MAGIC ^ crc equality; the value
 * returned by the plugin therefore proves that plugin code and plugin data
 * were relocated to and executed from the dynamically loaded image. */
#define POCA_BASELINE_PATTERN_SIZE 64u

static const uint8_t POCA_BASELINE_PATTERN[POCA_BASELINE_PATTERN_SIZE] = {
    0xA5, 0x3C, 0x77, 0x01, 0xFE, 0x12, 0x9B, 0x44, /* 0-7   */
    0x61, 0xE0, 0x2D, 0xB8, 0x0F, 0xC3, 0x56, 0x9A, /* 8-15  */
    0x3E, 0x71, 0xAD, 0x04, 0xD2, 0x88, 0x1B, 0xF7, /* 16-23 */
    0x40, 0x95, 0x63, 0x2A, 0xEE, 0x09, 0xB1, 0x7C, /* 24-31 */
    0x0A, 0xD5, 0x36, 0x89, 0x5F, 0xE2, 0x1C, 0xA8, /* 32-39 */
    0x73, 0x06, 0xCB, 0x94, 0x21, 0x5D, 0xFA, 0x38, /* 40-47 */
    0xC6, 0x81, 0x17, 0x4E, 0xA0, 0x33, 0x6B, 0xDE, /* 48-55 */
    0x52, 0xF9, 0x24, 0x97, 0x45, 0xBA, 0x08, 0x5A, /* 56-63 */
};

/*
 * Reflected CRC-32 (poly 0xEDB88320, init/xor 0xFFFFFFFF). Implemented with a
 * lazily built table so the plugin exercises its .bss window without pulling
 * a large const table into .rodata. Pure integer code, no library calls; the
 * identical function runs on both sides and must produce identical values.
 */
static inline uint32_t poca_crc32(const uint8_t* data, size_t length) {
    static uint32_t table[256];
    static int table_ready = 0;
    if (table_ready == 0) {
        for (uint32_t i = 0; i < 256u; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) {
                c = ((c & 1u) != 0u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            }
            table[i] = c;
        }
        table_ready = 1;
    }
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < length; ++i) {
        crc = table[(crc ^ (uint32_t)data[i]) & 0xFFu] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

/*
 * Minimal entry table returned by the exported query entry. The function
 * pointers are relocated by the ELF loader (R_XTENSA_RELATIVE words in
 * .data.rel.ro) and therefore point at the loaded image after relocation.
 */
typedef struct poca_plugin_table {
    uint32_t struct_size; /* sizeof(poca_plugin_table_t) */
    uint16_t abi_major;
    uint16_t abi_minor;
    /* Self-check: validates the relocated image from inside the plugin.
     * Returns 0 on success or a negative frame_err_t-style code. */
    int32_t (*prepare)(void);
    /* Returns POCA_PLUGIN_MAGIC ^ crc32(POCA_BASELINE_PATTERN). */
    int32_t (*activate)(void);
    /* Releases plugin-side resources. Returns 0 on success. */
    int32_t (*unload)(void);
    uint32_t state_schema_id;      /* 0: stateless baseline plugin */
    uint32_t state_schema_version; /* 0: stateless baseline plugin */
    /* Host-import probe slots (T5 matrix). Null for plugins that import
     * nothing (baseline). For importing plugins these words are the
     * R_XTENSA_GLOB_DAT relocation slots: the loader writes the firmware
     * address of the registered host function into them, so the harness
     * can assert the exact relocation value by pointer comparison. */
    uint32_t (*host_sentinel)(void);
    uint32_t (*host_add)(uint32_t a, uint32_t b);
    /* IRAM probe slots (task T7). iram_check/iram_write are compiled into
     * the plugin's .plugin_iram section: after relocation these pointers
     * carry native IRAM window addresses (0x4037.. on ESP32-S3), which is
     * itself the address-window proof. data_word_ptr/data_word_read are
     * ordinary .text exports over the plugin's .data word used for the
     * write-visibility probe. Null for plugins without .plugin_iram. */
    uint32_t (*iram_check)(uint32_t magic, uint32_t mult, uint32_t seed, uint32_t poly,
                           uint32_t words);
    int32_t (*iram_write)(uint32_t* dst, uint32_t value);
    uint32_t* (*data_word_ptr)(void);
    uint32_t (*data_word_read)(void);
} poca_plugin_table_t;

typedef const poca_plugin_table_t* (*poca_plugin_query_fn)(void);

/*
 * IRAM probe value contract (task T7): the check value computed by the
 * IRAM-resident function. The implementation must stay LITERAL-FREE and
 * free of static references - every large constant arrives as a parameter,
 * because an xtensa l32r literal pool for .plugin_iram code would sit in a
 * different load window than the code and l32r reach (256 KB, backward
 * only) cannot span the IRAM/PSRAM split. The firmware computes the
 * expected value with this same inline over flash-resident code; equality
 * proves the IRAM-resident copy executed.
 */
#define POCA_IRAM_PROBE_MULT 1664525u
#define POCA_IRAM_PROBE_SEED 0xC0FFEE57u
#define POCA_IRAM_PROBE_POLY 0xEDB88320u
#define POCA_IRAM_PROBE_WORDS 16u
#define POCA_IRAM_WRITE_PATTERN 0x5CA1AB1Eu

static inline uint32_t poca_iram_fold(uint32_t magic, uint32_t mult, uint32_t seed, uint32_t poly,
                                      uint32_t words) {
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

static inline uint32_t poca_iram_probe_value(uint32_t magic) {
    return poca_iram_fold(magic, POCA_IRAM_PROBE_MULT, POCA_IRAM_PROBE_SEED, POCA_IRAM_PROBE_POLY,
                          POCA_IRAM_PROBE_WORDS);
}

#endif /* POC_A_PLUGINS_PLUGIN_ABI_H */

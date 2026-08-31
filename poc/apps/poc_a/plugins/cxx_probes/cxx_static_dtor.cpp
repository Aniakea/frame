/*
 * C++ feature-rejection probe v2: static destructor (task T6 of
 * poc-a-dynamic-elf, appendix C 11.2/11.3).
 *
 * A file-scope object with a non-trivial destructor plus an explicit
 * __attribute__((destructor)) hook force the toolchain to emit a static
 * finalization section (".dtors" on this xtensa gcc, i.e. ".fini_array").
 * The PoC-A unload path is the plugin's own unload() callback; a section
 * driven teardown register has no runner in the entry-point model, so the
 * artifact is forbidden.
 */

#include "plugin_abi.h"

#define CXX_PROBE_ERR_ABI_MISMATCH (-7)

static uint32_t g_dtor_mark = 0xA11CE11Du;

class DtorProbe {
  public:
    ~DtorProbe() { g_dtor_mark = 0xDEAD0002u; }
};

static DtorProbe g_dtor_probe;

__attribute__((destructor)) static void dtor_hook(void) { g_dtor_mark ^= 0x00000001u; }

static int32_t dtor_probe_prepare(void) { return 0; }

static int32_t dtor_probe_activate(void) {
    const uint32_t crc = poca_crc32(POCA_BASELINE_PATTERN, POCA_BASELINE_PATTERN_SIZE);
    return static_cast<int32_t>(POCA_PLUGIN_MAGIC ^ crc ^ g_dtor_mark);
}

static int32_t dtor_probe_unload(void) { return 0; }

static const poca_plugin_table_t k_dtor_table = {
    .struct_size = (uint32_t)sizeof(poca_plugin_table_t),
    .abi_major = POCA_PLUGIN_ABI_MAJOR,
    .abi_minor = POCA_PLUGIN_ABI_MINOR,
    .prepare = &dtor_probe_prepare,
    .activate = &dtor_probe_activate,
    .unload = &dtor_probe_unload,
    .state_schema_id = 0u,
    .state_schema_version = 0u,
    .host_sentinel = nullptr,
    .host_add = nullptr,
};

extern "C" __attribute__((visibility("default"))) const poca_plugin_table_t*
frame_plugin_entry(void) {
    return &k_dtor_table;
}

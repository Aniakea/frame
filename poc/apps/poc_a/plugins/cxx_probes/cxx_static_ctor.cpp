/*
 * C++ feature-rejection probe v1: static constructor (task T6 of
 * poc-a-dynamic-elf, appendix C 11.2/11.3).
 *
 * A file-scope object with a non-trivial constructor forces the toolchain to
 * emit a static-initialization section (this xtensa gcc emits ".ctors", the
 * pre-standard spelling of ".init_array"). The section is on the C++ feature
 * forbidden list: the PoC-A loader has no entry-point-model replacement for
 * running constructors, so admitting the image would silently skip them.
 *
 * This source is built by poca_cxx_probe() ONLY to prove that the pipeline
 * gates reject it; it is never whitelisted and (for the on-board row) only
 * its packaged ELF is offered to the loader, which must refuse it before any
 * plugin byte executes.
 */

#include "plugin_abi.h"

#define CXX_PROBE_ERR_ABI_MISMATCH (-7)
#define CXX_PROBE_ERR_PACKAGE_INVALID (-18)

static uint32_t g_ctor_sentinel = 0u;

class CtorProbe {
  public:
    CtorProbe() { g_ctor_sentinel = 0xC0DEC7C0u; }
};

static CtorProbe g_ctor_probe;

static int32_t ctor_probe_prepare(void) {
    if (g_ctor_sentinel != 0xC0DEC7C0u) {
        /* Constructor never ran: exactly the silent misbehavior the feature
         * ban protects against. */
        return CXX_PROBE_ERR_PACKAGE_INVALID;
    }
    return 0;
}

static int32_t ctor_probe_activate(void) {
    const uint32_t crc = poca_crc32(POCA_BASELINE_PATTERN, POCA_BASELINE_PATTERN_SIZE);
    return static_cast<int32_t>(POCA_PLUGIN_MAGIC ^ crc ^ g_ctor_sentinel);
}

static int32_t ctor_probe_unload(void) { return 0; }

static const poca_plugin_table_t k_ctor_table = {
    .struct_size = (uint32_t)sizeof(poca_plugin_table_t),
    .abi_major = POCA_PLUGIN_ABI_MAJOR,
    .abi_minor = POCA_PLUGIN_ABI_MINOR,
    .prepare = &ctor_probe_prepare,
    .activate = &ctor_probe_activate,
    .unload = &ctor_probe_unload,
    .state_schema_id = 0u,
    .state_schema_version = 0u,
    .host_sentinel = nullptr,
    .host_add = nullptr,
};

extern "C" __attribute__((visibility("default"))) const poca_plugin_table_t*
frame_plugin_entry(void) {
    return &k_ctor_table;
}

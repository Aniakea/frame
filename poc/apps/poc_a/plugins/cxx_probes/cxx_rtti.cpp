/*
 * C++ feature-rejection probe v5: RTTI (task T6 of poc-a-dynamic-elf,
 * appendix C 11.2/11.3).
 *
 * Compiled with -frtti on top of the probe baseline (-fno-rtti): a
 * polymorphic typeid plus a downcast dynamic_cast emit local typeinfo
 * objects (relocated with allowlisted R_XTENSA_RELATIVE words) but leave
 * the C++ ABI runtime imports undefined: __dynamic_cast (libstdc++) and
 * the __cxxabiv1 class-type-info vtables. This xtensa toolchain emits no
 * dedicated RTTI section, so the closed-world import gate (gate 2) is the
 * build-side rejection layer for this variant; on target the imports would
 * fail with -ENOSYS before any plugin byte executes (stock loader
 * behavior, proven for import_neg in T5).
 */

#include "plugin_abi.h"

#include <typeinfo>

#define CXX_PROBE_ERR_ABI_MISMATCH (-7)

struct RttiBase {
    virtual ~RttiBase() {}
};

struct RttiDerived : RttiBase {};

static RttiDerived g_rtti_obj;

static int32_t rtti_probe_prepare(void) { return 0; }

static int32_t rtti_probe_activate(void) {
    const uint32_t crc = poca_crc32(POCA_BASELINE_PATTERN, POCA_BASELINE_PATTERN_SIZE);
    const RttiBase* obj = &g_rtti_obj;
    const RttiDerived* down = dynamic_cast<const RttiDerived*>(obj);
    const uintptr_t info_addr = reinterpret_cast<uintptr_t>(&typeid(*obj));
    const uint32_t down_bit = (down != nullptr && info_addr != 0u) ? 0x37713900u : 0u;
    return static_cast<int32_t>(POCA_PLUGIN_MAGIC ^ crc ^ down_bit);
}

static int32_t rtti_probe_unload(void) { return 0; }

static const poca_plugin_table_t k_rtti_table = {
    .struct_size = (uint32_t)sizeof(poca_plugin_table_t),
    .abi_major = POCA_PLUGIN_ABI_MAJOR,
    .abi_minor = POCA_PLUGIN_ABI_MINOR,
    .prepare = &rtti_probe_prepare,
    .activate = &rtti_probe_activate,
    .unload = &rtti_probe_unload,
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
    return &k_rtti_table;
}

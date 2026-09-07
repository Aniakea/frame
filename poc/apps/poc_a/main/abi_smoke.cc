#include "abi_smoke.hh"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <type_traits>

#include "frame_poc/frame_abi.h"

namespace frame::poca {
namespace {

int check(bool passed, const char* label) {
    std::printf("%s %s\n", passed ? "PASS:" : "FAIL:", label);
    return passed ? 0 : 1;
}

int layout_checks() {
    int failures = 0;
    failures += check(offsetof(frame_abi_header_t, struct_size) == 0,
                      "frame_abi_header_t.struct_size offset is 0");
    failures += check(sizeof(frame_abi_header_t) == 16, "frame_abi_header_t size is 16");
    failures += check(sizeof(plugin_instance_handle_t) == 8, "plugin_instance_handle_t size is 8");
    return failures;
}

int compatibility_checks() {
    constexpr uint64_t required_feature = UINT64_C(1) << 12;
    const frame_abi_header_t offered{
        .struct_size = sizeof(frame_abi_header_t),
        .abi_major = FRAME_ABI_MAJOR,
        .abi_minor = FRAME_ABI_MINOR,
        .feature_bits = required_feature,
    };

    int failures = 0;
    failures += check(frame_abi_supports(&offered, FRAME_ABI_MAJOR, FRAME_ABI_MINOR,
                                         sizeof(frame_abi_header_t), required_feature),
                      "frame_abi_supports accepts matching major/minor/size/features");
    failures += check(!frame_abi_supports(&offered, FRAME_ABI_MAJOR + 1, FRAME_ABI_MINOR,
                                          sizeof(frame_abi_header_t), required_feature),
                      "frame_abi_supports rejects newer abi_major");
    failures += check(!frame_abi_supports(&offered, FRAME_ABI_MAJOR, FRAME_ABI_MINOR,
                                          sizeof(frame_abi_header_t) + 1, required_feature),
                      "frame_abi_supports rejects larger struct_size");
    return failures;
}

static_assert(__cplusplus >= 202302L);
static_assert(std::is_standard_layout_v<frame_abi_header_t>);

} // namespace

int run_abi_smoke() {
    int failures = layout_checks();
    failures += compatibility_checks();
    std::printf("poca abi: %s (%d failure%s)\n", failures == 0 ? "PASS" : "FAIL", failures,
                failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}

} // namespace frame::poca

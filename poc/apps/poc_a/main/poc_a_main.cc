#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "frame_poc/frame_abi.h"
#include "unity.h"

TEST_CASE("C ABI header layout is stable", "[poc-a][abi]")
{
    TEST_ASSERT_EQUAL_UINT32(0, offsetof(frame_abi_header_t, struct_size));
    TEST_ASSERT_EQUAL_UINT32(16, sizeof(frame_abi_header_t));
    TEST_ASSERT_EQUAL_UINT32(8, sizeof(plugin_instance_handle_t));
}

TEST_CASE("C ABI compatibility checks size version and features", "[poc-a][abi]")
{
    constexpr uint64_t required_feature = UINT64_C(1) << 12;
    const frame_abi_header_t offered{
        .struct_size = sizeof(frame_abi_header_t),
        .abi_major = FRAME_ABI_MAJOR,
        .abi_minor = FRAME_ABI_MINOR,
        .feature_bits = required_feature,
    };

    TEST_ASSERT_TRUE(frame_abi_supports(
        &offered, FRAME_ABI_MAJOR, FRAME_ABI_MINOR,
        sizeof(frame_abi_header_t), required_feature));
    TEST_ASSERT_FALSE(frame_abi_supports(
        &offered, FRAME_ABI_MAJOR + 1, FRAME_ABI_MINOR,
        sizeof(frame_abi_header_t), required_feature));
    TEST_ASSERT_FALSE(frame_abi_supports(
        &offered, FRAME_ABI_MAJOR, FRAME_ABI_MINOR,
        sizeof(frame_abi_header_t) + 1, required_feature));
}

static_assert(__cplusplus >= 202302L);
static_assert(std::is_standard_layout_v<frame_abi_header_t>);

extern "C" void app_main(void)
{
    unity_run_menu();
}
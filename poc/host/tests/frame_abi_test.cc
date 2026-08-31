#include <cstddef>
#include <cstdint>
#include <type_traits>

#include <gtest/gtest.h>

#include "frame_poc/frame_abi.h"

namespace {

constexpr uint64_t kRequiredFeature = UINT64_C(1) << 12;

struct extended_service {
    frame_abi_header_t abi;
    void (*operation)(void);
};

static_assert(__cplusplus >= 202302L);
static_assert(std::is_standard_layout_v<frame_abi_header_t>);
static_assert(std::is_standard_layout_v<frame_post_operation_t>);
static_assert(std::is_standard_layout_v<frame_completion_operation_t>);
static_assert(offsetof(frame_abi_header_t, struct_size) == 0);
static_assert(offsetof(extended_service, abi) == 0);
static_assert(sizeof(plugin_instance_handle_t) == sizeof(uint64_t));

TEST(FrameAbi, AcceptsCompatibleExtension) {
    const extended_service service{
        .abi =
            {
                .struct_size = sizeof(extended_service),
                .abi_major = FRAME_ABI_MAJOR,
                .abi_minor = FRAME_ABI_MINOR + 1,
                .feature_bits = kRequiredFeature,
            },
        .operation = nullptr,
    };

    EXPECT_TRUE(frame_abi_supports(&service.abi, FRAME_ABI_MAJOR, FRAME_ABI_MINOR,
                                   sizeof(extended_service), kRequiredFeature));
}

TEST(FrameAbi, RejectsIncompatibleOffer) {
    const extended_service service{
        .abi =
            {
                .struct_size = sizeof(extended_service),
                .abi_major = FRAME_ABI_MAJOR,
                .abi_minor = FRAME_ABI_MINOR + 1,
                .feature_bits = kRequiredFeature,
            },
        .operation = nullptr,
    };

    EXPECT_FALSE(frame_abi_supports(nullptr, FRAME_ABI_MAJOR, FRAME_ABI_MINOR,
                                    sizeof(frame_abi_header_t), 0));
    EXPECT_FALSE(frame_abi_supports(&service.abi, FRAME_ABI_MAJOR + 1, FRAME_ABI_MINOR,
                                    sizeof(frame_abi_header_t), 0));
    EXPECT_FALSE(frame_abi_supports(&service.abi, FRAME_ABI_MAJOR, FRAME_ABI_MINOR + 2,
                                    sizeof(frame_abi_header_t), 0));
    EXPECT_FALSE(frame_abi_supports(&service.abi, FRAME_ABI_MAJOR, FRAME_ABI_MINOR,
                                    sizeof(extended_service) + 1, 0));
    EXPECT_FALSE(frame_abi_supports(&service.abi, FRAME_ABI_MAJOR, FRAME_ABI_MINOR,
                                    sizeof(frame_abi_header_t), kRequiredFeature << 1));
}

TEST(FrameAbi, PinsStableErrorValues) {
    EXPECT_EQ(FRAME_OK, 0);
    EXPECT_EQ(FRAME_ERR_INVALID_PTR, -1);
    EXPECT_EQ(FRAME_ERR_INVALID_ARGUMENT, -28);
    EXPECT_EQ(FRAME_ERR_RATE_LIMITED, -29);
    EXPECT_EQ(FRAME_ERR_TARGET_MISMATCH, -35);
}

} // namespace

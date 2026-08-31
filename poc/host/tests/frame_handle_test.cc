#include <cstdint>

#include <gtest/gtest.h>

#include "frame_poc/frame_handle.hh"

using frame::poc::handle_owner;
using frame::poc::handle_table;
using frame::poc::handle_type;

TEST(FrameHandle, EnforcesCapacityOwnershipGenerationAndType) {
    handle_table<uint32_t, 1> table;
    const handle_owner first_owner{.instance = 7, .generation = 11};
    const handle_owner next_generation{.instance = 7, .generation = 12};

    uint64_t first_handle = 0;
    ASSERT_EQ(table.acquire(first_owner, handle_type::timer, 41, &first_handle), FRAME_OK);
    EXPECT_NE(first_handle, 0U);

    uint32_t* value = nullptr;
    ASSERT_EQ(table.get(first_handle, first_owner, handle_type::timer, &value), FRAME_OK);
    ASSERT_NE(value, nullptr);
    EXPECT_EQ(*value, 41U);
    EXPECT_EQ(table.get(first_handle, next_generation, handle_type::timer, &value),
              FRAME_ERR_INVALID_PTR);
    EXPECT_EQ(table.get(first_handle, first_owner, handle_type::buffer, &value),
              FRAME_ERR_INVALID_PTR);

    uint64_t overflow_handle = 0;
    EXPECT_EQ(table.acquire(first_owner, handle_type::buffer, 99, &overflow_handle),
              FRAME_ERR_CAPACITY);
    EXPECT_EQ(table.release(first_handle, next_generation, handle_type::timer),
              FRAME_ERR_INVALID_PTR);
    EXPECT_EQ(table.release(first_handle, first_owner, handle_type::timer), FRAME_OK);
    EXPECT_EQ(table.release(first_handle, first_owner, handle_type::timer), FRAME_ERR_INVALID_PTR);
}

TEST(FrameHandle, ReuseInvalidatesStaleHandle) {
    handle_table<uint32_t, 1> table;
    const handle_owner first_owner{.instance = 7, .generation = 11};
    const handle_owner next_owner{.instance = 7, .generation = UINT64_C(1) << 40};
    uint64_t first_handle = 0;
    uint64_t replacement_handle = 0;

    ASSERT_EQ(table.acquire(first_owner, handle_type::timer, 41, &first_handle), FRAME_OK);
    ASSERT_EQ(table.release(first_handle, first_owner, handle_type::timer), FRAME_OK);
    ASSERT_EQ(table.acquire(next_owner, handle_type::timer, 42, &replacement_handle), FRAME_OK);
    EXPECT_NE(replacement_handle, first_handle);

    uint32_t* value = nullptr;
    EXPECT_EQ(table.get(first_handle, first_owner, handle_type::timer, &value),
              FRAME_ERR_INVALID_PTR);
    ASSERT_EQ(table.get(replacement_handle, next_owner, handle_type::timer, &value), FRAME_OK);
    ASSERT_NE(value, nullptr);
    EXPECT_EQ(*value, 42U);
}

TEST(FrameHandle, RejectsInvalidArguments) {
    handle_table<uint32_t, 1> table;
    const handle_owner valid{.instance = 1, .generation = 1};
    uint64_t handle = 0;

    EXPECT_EQ(table.acquire(valid, handle_type::timer, 1, nullptr), FRAME_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(table.acquire({.instance = 0, .generation = 1}, handle_type::timer, 1, &handle),
              FRAME_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(table.acquire({.instance = 1, .generation = 0}, handle_type::timer, 1, &handle),
              FRAME_ERR_INVALID_ARGUMENT);
    ASSERT_EQ(table.acquire(valid, handle_type::timer, 1, &handle), FRAME_OK);
    EXPECT_EQ(table.get(handle, valid, handle_type::timer, nullptr), FRAME_ERR_INVALID_PTR);
}

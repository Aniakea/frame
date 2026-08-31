#include <cstddef>
#include <cstdint>

#include "frame_poc/frame_abi.h"
#include "frame_poc/frame_handle.hh"
#include "frame_poc/lifecycle_state.hh"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, std::size_t size) {
    if (size < 4) {
        return 0;
    }

    const auto state = static_cast<frame::poc::plugin_state>(data[0] % 10U);
    const auto event = static_cast<frame::poc::lifecycle_event>(data[1] % 11U);
    const auto result = frame::poc::transition(state, event);
    if (result.error != FRAME_OK && result.next != state) {
        __builtin_trap();
    }

    const frame_abi_header_t abi{
        .struct_size = static_cast<uint32_t>(data[2]) * 8U,
        .abi_major = data[0],
        .abi_minor = data[1],
        .feature_bits = static_cast<uint64_t>(data[3]),
    };
    (void)frame_abi_supports(&abi, FRAME_ABI_MAJOR, FRAME_ABI_MINOR, sizeof(frame_abi_header_t),
                             data[3] & UINT8_C(0x0F));

    frame::poc::handle_table<uint32_t, 2> table;
    const frame::poc::handle_owner owner{
        .instance = static_cast<uint64_t>(data[0]) + 1U,
        .generation = static_cast<uint64_t>(data[1]) + 1U,
    };
    uint64_t handle = 0;
    if (table.acquire(owner, frame::poc::handle_type::buffer, data[2], &handle) == FRAME_OK) {
        uint32_t* value = nullptr;
        (void)table.get(handle, owner, frame::poc::handle_type::buffer, &value);
        (void)table.release(handle, owner, frame::poc::handle_type::buffer);
    }
    return 0;
}

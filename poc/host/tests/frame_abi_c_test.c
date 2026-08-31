#include <stddef.h>

#include "frame_poc/frame_abi.h"

_Static_assert(sizeof(frame_abi_header_t) == 16, "ABI header size changed");
_Static_assert(offsetof(frame_abi_header_t, struct_size) == 0, "ABI header prefix changed");
_Static_assert(sizeof(plugin_instance_handle_t) == sizeof(uint64_t), "handle width changed");
_Static_assert(FRAME_ERR_RATE_LIMITED == -29, "error value changed");

int main(void) {
    const frame_abi_header_t abi = {
        .struct_size = sizeof(frame_abi_header_t),
        .abi_major = FRAME_ABI_MAJOR,
        .abi_minor = FRAME_ABI_MINOR,
        .feature_bits = 0,
    };

    return frame_abi_supports(&abi, FRAME_ABI_MAJOR, FRAME_ABI_MINOR, sizeof(frame_abi_header_t), 0)
               ? 0
               : 1;
}

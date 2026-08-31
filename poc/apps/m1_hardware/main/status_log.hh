#pragma once

#include <cstddef>
#include <cstdint>

#include "hardware_status.hh"

namespace frame::m1 {

[[nodiscard]] std::size_t format_status_log(const status_snapshot& status, uint64_t sequence,
                                            int64_t uptime_us, char* output, std::size_t capacity,
                                            const char* event = "status",
                                            const char* mode = "normal");

} // namespace frame::m1

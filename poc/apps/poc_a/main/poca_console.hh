#pragma once

#include "esp_err.h"

namespace frame::poca {

// Registers the `poca` console command group and starts the USB-Serial-JTAG
// REPL. Returns ESP_OK once the console task is running.
esp_err_t start_console();

// Stack size of the console REPL task (pinned core 1); the T9 soak reports
// it as the stack budget of the task the soak loop runs on.
inline constexpr uint32_t kPocaReplStackBytes = 6144;

} // namespace frame::poca

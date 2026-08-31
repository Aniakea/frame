#pragma once

#include "esp_err.h"

namespace frame::poca {

// Registers the `poca` console command group and starts the USB-Serial-JTAG
// REPL. Returns ESP_OK once the console task is running.
esp_err_t start_console();

} // namespace frame::poca

#include <cstdio>

#include "esp_log.h"
#include "poca_console.hh"

namespace {

constexpr char kTag[] = "poc-a";

} // namespace

extern "C" void app_main(void) {
    ESP_LOGI(kTag, "frame poc-a dynamic-elf harness; loader elf_loader 1.3.3 @6526c5b1");
    const esp_err_t console_result = frame::poca::start_console();
    ESP_LOGI(kTag, "console=%s", esp_err_to_name(console_result));
}

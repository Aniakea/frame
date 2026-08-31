#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

namespace frame::m1 {

enum class test_pattern : uint8_t {
    all_black,
    all_white,
    checker,
    border,
};

class st7305_panel {
  public:
    st7305_panel() = default;
    ~st7305_panel();

    st7305_panel(const st7305_panel&) = delete;
    st7305_panel& operator=(const st7305_panel&) = delete;

    esp_err_t initialize();
    esp_err_t draw_full(const uint8_t* row_major_i1, std::size_t size);
    esp_err_t draw_pattern(test_pattern pattern);

  private:
    static bool transfer_done(esp_lcd_panel_io_handle_t panel_io,
                              esp_lcd_panel_io_event_data_t* event_data, void* context);
    esp_err_t reset();
    esp_err_t command(uint8_t value);
    esp_err_t command(uint8_t value, const uint8_t* parameters, std::size_t count);

    esp_lcd_panel_io_handle_t io_{};
    bool bus_initialized_{};
    uint8_t* native_buffer_{};
    SemaphoreHandle_t transfer_complete_{};
    volatile bool transfer_pending_{};
};

} // namespace frame::m1

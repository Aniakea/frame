#pragma once

#include <cstdint>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

#include "hardware_status.hh"
#include "st7305_panel.hh"

namespace frame::m1 {

class display_service {
  public:
    explicit display_service(hardware_status& status) : status_(status) {}

    display_service(const display_service&) = delete;
    display_service& operator=(const display_service&) = delete;

    esp_err_t start();

  private:
    static void task_entry(void* context);
    static void flush(lv_display_t* display, const lv_area_t* area, uint8_t* pixels);
    esp_err_t initialize_lvgl();
    void render_status(const status_snapshot& value);
    void run();

    hardware_status& status_;
    st7305_panel panel_{};
    lv_display_t* display_{};
    lv_obj_t* health_label_{};
    lv_obj_t* platform_label_{};
    lv_obj_t* peripherals_label_{};
    lv_obj_t* network_label_{};
    lv_obj_t* memory_label_{};
    uint8_t* framebuffer_{};
    uint8_t* draw_buffer_one_{};
    uint8_t* draw_buffer_two_{};
    TaskHandle_t task_{};
};

} // namespace frame::m1

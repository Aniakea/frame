#pragma once

#include "driver/i2c_master.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "hardware_status.hh"

namespace frame::m1 {

class board_service {
  public:
    explicit board_service(hardware_status& status) : status_(status) {}
    ~board_service();

    board_service(const board_service&) = delete;
    board_service& operator=(const board_service&) = delete;

    esp_err_t start();

  private:
    static void task_entry(void* context);
    static uint8_t crc8(const uint8_t* data, std::size_t size);
    esp_err_t initialize_i2c();
    esp_err_t sample_sensor();
    esp_err_t send_sensor_sleep();
    esp_err_t read_rtc();
    void run();

    hardware_status& status_;
    i2c_master_bus_handle_t bus_{};
    i2c_master_dev_handle_t sensor_{};
    i2c_master_dev_handle_t rtc_{};
    TaskHandle_t task_{};
};

} // namespace frame::m1

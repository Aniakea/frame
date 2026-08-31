#pragma once

#include <cstdint>

#include "driver/i2c_master.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "hardware_status.hh"

namespace frame::m1 {

class board_service {
  public:
    explicit board_service(hardware_status& status)
        : status_(status), i2c_mutex_(xSemaphoreCreateMutex()) {}
    ~board_service();

    board_service(const board_service&) = delete;
    board_service& operator=(const board_service&) = delete;

    esp_err_t start();

    // Sets the PCF85063 clock from UTC unix seconds; safe from the console or SNTP context
    // (I2C access is serialized against read_rtc with i2c_mutex_).
    esp_err_t set_rtc_time(int64_t unix_seconds);
    // Reads PCF85063 registers 0x00-0x0A for diagnostics; 11 bytes into `registers`.
    esp_err_t read_rtc_raw(uint8_t registers[11]);
    // Civil UTC date/time to unix seconds; shared by the RTC read and console `rtc set`.
    static int64_t unix_from_utc(int year, unsigned month, unsigned day, unsigned hour,
                                 unsigned minute, unsigned second);

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
    SemaphoreHandle_t i2c_mutex_{};
    TaskHandle_t task_{};
};

} // namespace frame::m1

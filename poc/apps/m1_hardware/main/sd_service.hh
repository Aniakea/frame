#pragma once

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdmmc_cmd.h"

#include "hardware_status.hh"

namespace frame::m1 {

class sd_service {
  public:
    explicit sd_service(hardware_status& status) : status_(status) {}
    ~sd_service();

    sd_service(const sd_service&) = delete;
    sd_service& operator=(const sd_service&) = delete;

    esp_err_t start();
    void request_probe();

  private:
    static void task_entry(void* context);
    esp_err_t mount_and_check();
    esp_err_t health_check();
    esp_err_t append_status_log();
    void unmount();
    void run();

    hardware_status& status_;
    sdmmc_card_t* card_{};
    TaskHandle_t task_{};
    uint64_t log_sequence_{};
};

} // namespace frame::m1

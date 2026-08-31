#pragma once

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "board_service.hh"
#include "hardware_status.hh"

namespace frame::m1 {

// Starts SNTP once the station has an IP and mirrors the synced system clock into the
// PCF85063 RTC exactly once per sync (rising edge of time() crossing the sync threshold).
class time_sync {
  public:
    time_sync(hardware_status& status, board_service& board) : status_(status), board_(board) {}

    time_sync(const time_sync&) = delete;
    time_sync& operator=(const time_sync&) = delete;

    esp_err_t start();

  private:
    static void task_entry(void* context);
    void run();

    hardware_status& status_;
    board_service& board_;
    TaskHandle_t task_{};
    bool sntp_started_{};
    bool rtc_synced_{};
};

} // namespace frame::m1

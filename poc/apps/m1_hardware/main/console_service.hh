#pragma once

#include "esp_err.h"

#include "board_service.hh"
#include "hardware_status.hh"
#include "sd_service.hh"
#include "wifi_provision.hh"
#include "wifi_service.hh"

namespace frame::m1 {

class console_service {
  public:
    console_service(hardware_status& status, board_service& board, sd_service& storage,
                    wifi_service& wifi, wifi_provision& provision)
        : status_(status), board_(board), storage_(storage), wifi_(wifi), provision_(provision) {}

    esp_err_t start();

  private:
    static int status_command(int argc, char** argv);
    static int wifi_command(int argc, char** argv);
    static int storage_command(int argc, char** argv);
    static int selftest_command(int argc, char** argv);
    static int prov_command(int argc, char** argv);
    static int rtc_command(int argc, char** argv);
    static int metrics_command(int argc, char** argv);
    void print_status(bool json) const;
    void print_rtc_status() const;
    void print_metrics() const;
    int print_rtc_raw() const;
    int run_rtc_set(const char* date_text, const char* time_text);
    void run_wifi_config_selftest() const;

    inline static console_service* instance_{};
    hardware_status& status_;
    board_service& board_;
    sd_service& storage_;
    wifi_service& wifi_;
    wifi_provision& provision_;
};

} // namespace frame::m1

#pragma once

#include "esp_err.h"

#include "hardware_status.hh"
#include "sd_service.hh"
#include "wifi_provision.hh"
#include "wifi_service.hh"

namespace frame::m1 {

class console_service {
  public:
    console_service(hardware_status& status, sd_service& storage, wifi_service& wifi,
                    wifi_provision& provision)
        : status_(status), storage_(storage), wifi_(wifi), provision_(provision) {}

    esp_err_t start();

  private:
    static int status_command(int argc, char** argv);
    static int wifi_command(int argc, char** argv);
    static int storage_command(int argc, char** argv);
    static int selftest_command(int argc, char** argv);
    static int prov_command(int argc, char** argv);
    void print_status(bool json) const;
    void run_wifi_config_selftest() const;

    inline static console_service* instance_{};
    hardware_status& status_;
    sd_service& storage_;
    wifi_service& wifi_;
    wifi_provision& provision_;
};

} // namespace frame::m1

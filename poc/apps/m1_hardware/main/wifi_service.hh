#pragma once

#include <atomic>
#include <cstddef>

#include "esp_err.h"
#include "esp_event.h"

#include "hardware_status.hh"

namespace frame::m1 {

class wifi_service {
  public:
    explicit wifi_service(hardware_status& status) : status_(status) {}

    wifi_service(const wifi_service&) = delete;
    wifi_service& operator=(const wifi_service&) = delete;

    esp_err_t initialize();
    esp_err_t connect_ephemeral(const char* ssid, const char* password, bool hidden);
    esp_err_t connect_stored(const char* ssid, const char* password, bool hidden);
    esp_err_t disconnect();
    esp_err_t reconnect();
    [[nodiscard]] bool configured() const { return configured_.load(); }

  private:
    static void event_handler(void* context, esp_event_base_t base, int32_t id, void* data);
    void handle_event(esp_event_base_t base, int32_t id, void* data);
    esp_err_t connect_sta(const char* ssid, const char* password, bool hidden);

    hardware_status& status_;
    bool initialized_{};
    std::atomic<bool> configured_{};
};

} // namespace frame::m1

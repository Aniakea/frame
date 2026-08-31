#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "esp_err.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "hardware_status.hh"
#include "wifi_service.hh"

namespace frame::m1 {

struct provision_ap_info {
    char ssid[33]{};
    int8_t rssi{};
    bool secure{};
};

class wifi_provision {
  public:
    wifi_provision(hardware_status& status, wifi_service& wifi) : status_(status), wifi_(wifi) {
        mutex_ = xSemaphoreCreateMutex();
    }

    wifi_provision(const wifi_provision&) = delete;
    wifi_provision& operator=(const wifi_provision&) = delete;

    esp_err_t initialize();
    esp_err_t config_load(char ssid[33], char password[64], bool& hidden);
    esp_err_t config_save(const char* ssid, const char* password, bool hidden);
    esp_err_t config_clear();
    esp_err_t start_saved_or_pending();
    esp_err_t start_provision();
    esp_err_t stop_provision();

    [[nodiscard]] bool ap_active() const { return ap_active_.load(); }
    [[nodiscard]] bool config_present();
    esp_err_t scan_networks(provision_ap_info* results, std::size_t capacity, std::size_t* count);

    static esp_err_t parse_config(const char* text, char ssid[33], char password[64], bool& hidden);

  private:
    enum class pending_action : uint8_t { none, sta_connect, restart_ap, stop_ap };

    static void monitor_trampoline(void* context);
    static void event_trampoline(void* context, esp_event_base_t base, int32_t id, void* data);
    static esp_err_t root_handler(httpd_req_t* request);
    static esp_err_t scan_handler(httpd_req_t* request);
    static esp_err_t connect_handler(httpd_req_t* request);
    static esp_err_t clear_handler(httpd_req_t* request);

    void monitor_loop();
    void handle_event(esp_event_base_t base, int32_t id);
    esp_err_t ensure_runtime();
    esp_err_t start_httpd();
    void build_ap_name(char (&name)[17]) const;
    esp_err_t write_config_atomic(const char* text);

    hardware_status& status_;
    wifi_service& wifi_;
    SemaphoreHandle_t mutex_{};
    httpd_handle_t server_{};
    TaskHandle_t monitor_{};
    bool mounted_{};
    bool ap_netif_created_{};
    char pending_ssid_[33]{};
    char pending_password_[64]{};
    bool pending_hidden_{};
    std::atomic<bool> runtime_ready_{};
    std::atomic<bool> ap_active_{};
    std::atomic<bool> has_ip_{};
    std::atomic<pending_action> pending_{pending_action::none};
    std::atomic<int> disconnect_count_{};
    std::atomic<int> no_ip_ticks_{};
};

} // namespace frame::m1

#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

namespace frame::m1 {

enum class health_level : uint8_t {
    ok,
    degraded,
    failed,
    recovery,
};

enum class sd_state : uint8_t {
    absent,
    health_checking,
    ready,
    failed,
    recovering,
};

enum class wifi_state : uint8_t {
    unconfigured,
    disconnected,
    connecting,
    connected,
};

struct sensor_sample {
    int16_t temperature_tenths_celsius{};
    uint16_t humidity_tenths_percent{};
    bool valid{};
    int64_t sampled_at_us{};
};

struct rtc_time {
    int64_t unix_seconds{};
    bool valid{};
    int64_t read_at_us{};
};

struct status_snapshot {
    char device_id[13]{};
    char elf_sha256[65]{};
    char image_sha256[65]{};
    char app_version[32]{};
    char idf_version[32]{};
    char toolchain_version[48]{};
    char ip_address[16]{};
    std::size_t flash_bytes{};
    std::size_t psram_bytes{};
    std::size_t internal_free_bytes{};
    std::size_t internal_largest_block{};
    std::size_t psram_free_bytes{};
    std::size_t psram_largest_block{};
    uint32_t chip_revision{};
    uint32_t cpu_cores{};
    uint32_t reset_reason{};
    uint64_t key_short_presses{};
    uint64_t key_long_presses{};
    uint64_t boot_short_presses{};
    uint64_t boot_long_presses{};
    uint32_t reset_count{};
    bool target_ok{};
    bool flash_ok{};
    bool psram_ok{};
    bool partitions_ok{};
    bool rtc_present{};
    bool sensor_present{};
    bool ssid_configured{};
    bool ap_active{};
    bool ap_mode_requested{};
    bool key_pressed{};
    bool nvs_encrypted{};
    bool tf_logging_ok{};
    sd_state storage{sd_state::absent};
    wifi_state wifi{wifi_state::unconfigured};
    sensor_sample sensor{};
    rtc_time rtc{};
    esp_err_t display_error{ESP_ERR_INVALID_STATE};
    esp_err_t storage_error{ESP_ERR_INVALID_STATE};
    esp_err_t board_error{ESP_ERR_INVALID_STATE};
    esp_err_t wifi_error{ESP_ERR_INVALID_STATE};
    esp_err_t logging_error{ESP_ERR_INVALID_STATE};
};

class hardware_status {
  public:
    hardware_status();

    hardware_status(const hardware_status&) = delete;
    hardware_status& operator=(const hardware_status&) = delete;

    void initialize_platform();
    void refresh_memory_metrics();
    [[nodiscard]] status_snapshot snapshot() const;

    template <typename Function> void update(Function&& function) {
        if (xSemaphoreTake(mutex_, portMAX_DELAY) == pdTRUE) {
            function(value_);
            xSemaphoreGive(mutex_);
        }
    }

    [[nodiscard]] static health_level overall_health(const status_snapshot& value);
    [[nodiscard]] static const char* health_name(health_level value);
    [[nodiscard]] static const char* sd_state_name(sd_state value);
    [[nodiscard]] static const char* wifi_state_name(wifi_state value);

  private:
    mutable SemaphoreHandle_t mutex_{};
    status_snapshot value_{};
};

} // namespace frame::m1

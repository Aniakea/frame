#include "time_sync.hh"

#include <cinttypes>
#include <ctime>

#include "esp_log.h"
#include "esp_netif_sntp.h"

namespace frame::m1 {
namespace {

constexpr char kTag[] = "frame-time";
constexpr const char* kSntpServers[] = {"ntp.aliyun.com", "pool.ntp.org"};
constexpr int kPollIntervalMs = 1000;
// SNTP timestamps before 2020-01-01 mean the system clock never synced.
constexpr time_t kSyncThreshold = 1577836800;

} // namespace

esp_err_t time_sync::start() {
    const BaseType_t created =
        xTaskCreatePinnedToCore(&time_sync::task_entry, "frame-time", 4096, this, 5, &task_, 0);
    return created == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

void time_sync::task_entry(void* context) { static_cast<time_sync*>(context)->run(); }

void time_sync::run() {
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(kPollIntervalMs));

        const status_snapshot value = status_.snapshot();
        if (!sntp_started_ && value.wifi == wifi_state::connected) {
            esp_sntp_config_t config{};
            config.start = true;
            config.wait_for_sync = false;
            config.num_of_servers = 2;
            config.servers[0] = kSntpServers[0];
            config.servers[1] = kSntpServers[1];
            const esp_err_t result = esp_netif_sntp_init(&config);
            if (result == ESP_OK) {
                sntp_started_ = true;
                ESP_LOGI(kTag, "sntp started (%s, %s)", kSntpServers[0], kSntpServers[1]);
            } else {
                ESP_LOGW(kTag, "sntp start failed: %s", esp_err_to_name(result));
            }
        }

        if (sntp_started_ && !rtc_synced_ && time(nullptr) > kSyncThreshold) {
            const int64_t unix_seconds = static_cast<int64_t>(time(nullptr));
            const esp_err_t result = board_.set_rtc_time(unix_seconds);
            if (result == ESP_OK) {
                rtc_synced_ = true;
                ESP_LOGI(kTag, "rtc synced from sntp (unix %" PRId64 ")", unix_seconds);
            } else {
                ESP_LOGW(kTag, "rtc sync from sntp failed: %s", esp_err_to_name(result));
            }
        }
    }
}

} // namespace frame::m1

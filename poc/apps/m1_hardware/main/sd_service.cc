#include "sd_service.hh"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>
#include <unistd.h>

#include "board_config.hh"
#include "driver/sdmmc_host.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "psa/crypto.h"
#include "status_log.hh"

namespace frame::m1 {
namespace {

constexpr char kTag[] = "frame-sd";
constexpr char kHealthDirectory[] = "/frame/.health";
constexpr char kHealthPath[] = "/frame/.health/m1.tmp";
constexpr char kHealthPayload[] = "frame-m1-sd-health-v1\n";
constexpr char kLogDirectory[] = "/frame/logs";
constexpr char kLogPath[] = "/frame/logs/m1-current.jsonl";
constexpr int64_t kLogIntervalUs = 10LL * 1000LL * 1000LL;

} // namespace

sd_service::~sd_service() { unmount(); }

esp_err_t sd_service::start() {
    const BaseType_t created =
        xTaskCreatePinnedToCore(&sd_service::task_entry, "frame-sd", 6144, this, 7, &task_, 1);
    return created == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

void sd_service::request_probe() {
    if (task_ != nullptr) {
        xTaskNotifyGive(task_);
    }
}

void sd_service::task_entry(void* context) { static_cast<sd_service*>(context)->run(); }

esp_err_t sd_service::mount_and_check() {
    status_.update([](status_snapshot& value) { value.storage = sd_state::health_checking; });

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 1;
    slot.clk = board::kSdClock;
    slot.cmd = board::kSdCommand;
    slot.d0 = board::kSdData0;
    slot.d1 = GPIO_NUM_NC;
    slot.d2 = GPIO_NUM_NC;
    slot.d3 = GPIO_NUM_NC;
    slot.cd = SDMMC_SLOT_NO_CD;
    slot.wp = SDMMC_SLOT_NO_WP;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    esp_vfs_fat_sdmmc_mount_config_t mount{};
    mount.format_if_mount_failed = false;
    mount.max_files = 8;
    mount.allocation_unit_size = 16 * 1024;

    esp_err_t result = esp_vfs_fat_sdmmc_mount(board::kSdMount, &host, &slot, &mount, &card_);
    if (result != ESP_OK) {
        card_ = nullptr;
        const bool card_absent = result == ESP_ERR_NOT_FOUND || result == ESP_ERR_TIMEOUT;
        status_.update([&](status_snapshot& value) {
            value.storage = card_absent ? sd_state::absent : sd_state::failed;
            value.storage_error = result;
        });
        return result;
    }

    result = health_check();
    status_.update([&](status_snapshot& value) {
        value.storage = result == ESP_OK ? sd_state::ready : sd_state::failed;
        value.storage_error = result;
    });
    if (result != ESP_OK) {
        unmount();
    }
    return result;
}

esp_err_t sd_service::health_check() {
    if (mkdir(kHealthDirectory, 0755) != 0 && errno != EEXIST) {
        return ESP_FAIL;
    }
    if (mkdir(kLogDirectory, 0755) != 0 && errno != EEXIST) {
        return ESP_FAIL;
    }
    FILE* output = std::fopen(kHealthPath, "wb");
    if (output == nullptr) {
        return ESP_FAIL;
    }
    const std::size_t written = std::fwrite(kHealthPayload, 1, sizeof(kHealthPayload) - 1U, output);
    const int flush_result = std::fflush(output);
    const int sync_result = fsync(fileno(output));
    const int close_result = std::fclose(output);
    if (written != sizeof(kHealthPayload) - 1U || flush_result != 0 || sync_result != 0 ||
        close_result != 0) {
        unlink(kHealthPath);
        return ESP_FAIL;
    }

    char readback[sizeof(kHealthPayload) - 1U]{};
    FILE* input = std::fopen(kHealthPath, "rb");
    if (input == nullptr) {
        return ESP_FAIL;
    }
    const std::size_t read = std::fread(readback, 1, sizeof(readback), input);
    const int read_close_result = std::fclose(input);
    uint8_t expected_hash[32]{};
    uint8_t actual_hash[32]{};
    std::size_t expected_size = 0;
    std::size_t actual_size = 0;
    const psa_status_t expected_result =
        psa_hash_compute(PSA_ALG_SHA_256, reinterpret_cast<const uint8_t*>(kHealthPayload),
                         sizeof(readback), expected_hash, sizeof(expected_hash), &expected_size);
    const psa_status_t actual_result =
        psa_hash_compute(PSA_ALG_SHA_256, reinterpret_cast<const uint8_t*>(readback),
                         sizeof(readback), actual_hash, sizeof(actual_hash), &actual_size);
    unlink(kHealthPath);
    return read == sizeof(readback) && read_close_result == 0 && expected_result == PSA_SUCCESS &&
                   actual_result == PSA_SUCCESS && expected_size == sizeof(expected_hash) &&
                   actual_size == sizeof(actual_hash) &&
                   std::memcmp(expected_hash, actual_hash, sizeof(expected_hash)) == 0
               ? ESP_OK
               : ESP_FAIL;
}

void sd_service::unmount() {
    if (card_ != nullptr) {
        esp_vfs_fat_sdcard_unmount(board::kSdMount, card_);
        card_ = nullptr;
    }
}

esp_err_t sd_service::append_status_log() {
    // Must exceed the ~550-byte payload: format_status_log returns 0 on truncation (= ESP_FAIL).
    char line[640];
    const status_snapshot snapshot = status_.snapshot();
    const std::size_t length =
        format_status_log(snapshot, ++log_sequence_, esp_timer_get_time(), line, sizeof(line));
    if (length == 0) {
        return ESP_FAIL;
    }

    const std::size_t mirrored = std::fwrite(line, 1, length, stdout);
    std::fflush(stdout);
    if (mirrored != length) {
        return ESP_FAIL;
    }

    FILE* output = std::fopen(kLogPath, "a");
    if (output == nullptr) {
        return ESP_FAIL;
    }
    const std::size_t written = std::fwrite(line, 1, length, output);
    const int flush_result = std::fflush(output);
    const int sync_result = fsync(fileno(output));
    const int close_result = std::fclose(output);
    if (written != length || flush_result != 0 || sync_result != 0 || close_result != 0) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

void sd_service::run() {
    int64_t next_log_at = 0;
    while (true) {
        const int64_t now = esp_timer_get_time();
        if (card_ == nullptr) {
            const esp_err_t result = mount_and_check();
            if (result != ESP_OK) {
                ESP_LOGW(kTag, "TF probe failed: %s", esp_err_to_name(result));
            } else {
                next_log_at = now;
            }
        } else {
            const esp_err_t result = sdmmc_get_status(card_);
            if (result != ESP_OK) {
                ESP_LOGW(kTag, "TF removed or unhealthy: %s", esp_err_to_name(result));
                status_.update([&](status_snapshot& value) {
                    value.storage = sd_state::recovering;
                    value.storage_error = result;
                });
                unmount();
                status_.update([](status_snapshot& value) {
                    value.storage = sd_state::absent;
                    value.tf_logging_ok = false;
                });
            } else if (now >= next_log_at) {
                const esp_err_t log_result = append_status_log();
                status_.update([&](status_snapshot& value) {
                    value.tf_logging_ok = log_result == ESP_OK;
                    value.logging_error = log_result;
                });
                next_log_at = now + kLogIntervalUs;
            }
        }
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(2000));
    }
}

} // namespace frame::m1

#include "board_config.hh"
#include "board_service.hh"
#include "console_service.hh"
#include "display_service.hh"
#include "esp_log.h"
#include "hardware_status.hh"
#include "nvs_flash.h"
#include "sd_service.hh"
#include "time_sync.hh"
#include "wifi_provision.hh"
#include "wifi_service.hh"
#include <cinttypes>

namespace {

constexpr char kTag[] = "frame-m1";

} // namespace

extern "C" void app_main(void) {
    using namespace frame::m1;

    static hardware_status status;
    status.initialize_platform();
    const status_snapshot initial = status.snapshot();
    ESP_LOGI(kTag, "Frame M1 %s; IDF %s; toolchain %s", board::kFirmwareVersion,
             initial.idf_version, initial.toolchain_version);
    ESP_LOGI(kTag, "board=%s module=%s device=%s", board::kProduct, board::kModule,
             initial.device_id);
    ESP_LOGI(kTag,
             "cores=%" PRIu32 " flash=%u psram=%u part=%s reset=%" PRIu32 " image_sha256=%.16s",
             initial.cpu_cores, static_cast<unsigned>(initial.flash_bytes),
             static_cast<unsigned>(initial.psram_bytes), initial.partitions_ok ? "OK" : "FAIL",
             initial.reset_reason, initial.image_sha256);

    const esp_err_t nvs_result = nvs_flash_init();
    if (nvs_result != ESP_OK) {
        ESP_LOGW(kTag, "NVS unavailable; Wi-Fi credentials will remain RAM-only: %s",
                 esp_err_to_name(nvs_result));
    }

    static board_service board(status);
    static sd_service storage(status);
    static wifi_service wifi(status);
    static wifi_provision provision(status, wifi);
    static display_service display(status);
    static console_service console(status, board, storage, wifi, provision);
    static time_sync timesync(status, board);

    const esp_err_t board_result = board.start();
    const esp_err_t storage_result = storage.start();
    const esp_err_t provision_mount_result = provision.initialize();
    const esp_err_t wifi_result = wifi.initialize();
    const esp_err_t display_result = display.start();
    const esp_err_t console_result = console.start();
    const esp_err_t saved_result = provision.start_saved_or_pending();
    const esp_err_t timesync_result = timesync.start();

    ESP_LOGI(kTag, "services board=%s storage=%s wifi=%s display=%s console=%s",
             esp_err_to_name(board_result), esp_err_to_name(storage_result),
             esp_err_to_name(wifi_result), esp_err_to_name(display_result),
             esp_err_to_name(console_result));
    ESP_LOGI(kTag, "provision mount=%s saved=%s time=%s", esp_err_to_name(provision_mount_result),
             esp_err_to_name(saved_result), esp_err_to_name(timesync_result));
}

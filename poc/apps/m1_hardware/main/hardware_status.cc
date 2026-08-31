#include "hardware_status.hh"

#include <cstdio>
#include <cstring>

#include "board_config.hh"
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_mac.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_psram.h"
#include "esp_system.h"
#include "psa/crypto.h"
#include "sdkconfig.h"

#ifndef FRAME_TOOLCHAIN_VERSION
#define FRAME_TOOLCHAIN_VERSION "unknown"
#endif

namespace frame::m1 {
namespace {

constexpr char kHex[] = "0123456789abcdef";

struct expected_partition {
    const char* label;
    esp_partition_type_t type;
    esp_partition_subtype_t subtype;
    uint32_t offset;
    uint32_t size;
};

// Mirrors partitions.csv; validated on boot per M1-002 (PART-001 identity).
constexpr expected_partition kExpectedPartitions[]{
    {"nvs", ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS, 0x9000, 0x14000},
    {"nvs_keys", ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS_KEYS, 0x1D000, 0x1000},
    {"otadata", ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_OTA, 0x1E000, 0x2000},
    {"coredump", ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_COREDUMP, 0x20000, 0x20000},
    {"ota_0", ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, 0x40000, 0x500000},
    {"ota_1", ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_1, 0x540000, 0x500000},
    {"plugin_fs", ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_LITTLEFS, 0xA40000, 0x4C0000},
    {"system_fs", ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_LITTLEFS, 0xF00000, 0x100000},
};

bool verify_partitions() {
    for (const expected_partition& expected : kExpectedPartitions) {
        const esp_partition_t* partition =
            esp_partition_find_first(expected.type, expected.subtype, expected.label);
        if (partition == nullptr || partition->address != expected.offset ||
            partition->size != expected.size) {
            return false;
        }
    }
    return true;
}

void bytes_to_hex(const uint8_t* bytes, std::size_t count, char* output) {
    for (std::size_t index = 0; index < count; ++index) {
        output[index * 2U] = kHex[bytes[index] >> 4U];
        output[index * 2U + 1U] = kHex[bytes[index] & 0x0FU];
    }
    output[count * 2U] = '\0';
}

void derive_device_id(char* output) {
    constexpr char domain[] = "frame-device-id-v1";
    uint8_t mac[6]{};
    uint8_t input[sizeof(domain) - 1U + sizeof(mac)]{};
    uint8_t digest[32]{};

    if (esp_efuse_mac_get_default(mac) != ESP_OK) {
        std::memcpy(output, "000000000000", 13U);
        return;
    }
    std::memcpy(input, domain, sizeof(domain) - 1U);
    std::memcpy(input + sizeof(domain) - 1U, mac, sizeof(mac));
    std::size_t digest_size = 0;
    const psa_status_t result = psa_hash_compute(PSA_ALG_SHA_256, input, sizeof(input), digest,
                                                 sizeof(digest), &digest_size);
    if (result != PSA_SUCCESS || digest_size != sizeof(digest)) {
        std::memcpy(output, "000000000000", 13U);
        return;
    }
    bytes_to_hex(digest, 6U, output);
}

} // namespace

hardware_status::hardware_status() : mutex_(xSemaphoreCreateMutex()) {
    configASSERT(mutex_ != nullptr);
}

void hardware_status::initialize_platform() {
    status_snapshot initial{};
    esp_chip_info_t chip{};
    uint32_t flash_size = 0;

    esp_chip_info(&chip);
    derive_device_id(initial.device_id);
    initial.chip_revision = chip.revision;
    initial.cpu_cores = chip.cores;
    initial.reset_reason = static_cast<uint32_t>(esp_reset_reason());
    initial.target_ok = chip.model == CHIP_ESP32S3 && chip.cores == 2U;

    if (esp_flash_get_size(nullptr, &flash_size) == ESP_OK) {
        initial.flash_bytes = flash_size;
        initial.flash_ok = flash_size == board::kFlashBytes;
    }

    initial.psram_bytes = esp_psram_get_size();
    initial.psram_ok = initial.psram_bytes == board::kPsramBytes;
    initial.internal_free_bytes = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    initial.internal_largest_block =
        heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    initial.psram_free_bytes = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    initial.psram_largest_block =
        heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    const esp_app_desc_t* app = esp_app_get_description();
    bytes_to_hex(app->app_elf_sha256, sizeof(app->app_elf_sha256), initial.elf_sha256);
    std::snprintf(initial.app_version, sizeof(initial.app_version), "%s", app->version);
    std::snprintf(initial.idf_version, sizeof(initial.idf_version), "%s", app->idf_ver);
    std::snprintf(initial.toolchain_version, sizeof(initial.toolchain_version), "%s",
                  FRAME_TOOLCHAIN_VERSION);

    uint8_t image_digest[32]{};
    const esp_partition_t* running = esp_ota_get_running_partition();
    if (running != nullptr && esp_partition_get_sha256(running, image_digest) == ESP_OK) {
        bytes_to_hex(image_digest, sizeof(image_digest), initial.image_sha256);
    }
    initial.partitions_ok = verify_partitions();
    initial.nvs_encrypted = false;

    update([&](status_snapshot& value) { value = initial; });
}

void hardware_status::refresh_memory_metrics() {
    const std::size_t internal_free =
        heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const std::size_t internal_largest =
        heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const std::size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    const std::size_t psram_largest =
        heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    update([&](status_snapshot& value) {
        value.internal_free_bytes = internal_free;
        value.internal_largest_block = internal_largest;
        value.psram_free_bytes = psram_free;
        value.psram_largest_block = psram_largest;
    });
}

status_snapshot hardware_status::snapshot() const {
    status_snapshot result{};
    if (xSemaphoreTake(mutex_, portMAX_DELAY) == pdTRUE) {
        result = value_;
        xSemaphoreGive(mutex_);
    }
    return result;
}

health_level hardware_status::overall_health(const status_snapshot& value) {
    if (!value.target_ok || !value.flash_ok || !value.psram_ok || !value.partitions_ok ||
        value.display_error != ESP_OK) {
        return health_level::failed;
    }
    // M1 policy: RAM-only Wi-Fi credentials are expected (SEC-010 blocks encrypted NVS on this
    // board), so an unconfigured SSID or plaintext NVS never lowers M1 health.
    if (value.storage != sd_state::ready || !value.rtc_present || !value.sensor_present ||
        value.board_error != ESP_OK || !value.sensor.valid) {
        return health_level::degraded;
    }
    return health_level::ok;
}

const char* hardware_status::health_name(health_level value) {
    switch (value) {
    case health_level::ok:
        return "OK";
    case health_level::degraded:
        return "DEGRADED";
    case health_level::failed:
        return "FAILED";
    case health_level::recovery:
        return "RECOVERY";
    }
    return "UNKNOWN";
}

const char* hardware_status::sd_state_name(sd_state value) {
    switch (value) {
    case sd_state::absent:
        return "ABSENT";
    case sd_state::health_checking:
        return "HEALTH_CHECKING";
    case sd_state::ready:
        return "READY";
    case sd_state::failed:
        return "FAILED";
    case sd_state::recovering:
        return "RECOVERING";
    }
    return "UNKNOWN";
}

const char* hardware_status::wifi_state_name(wifi_state value) {
    switch (value) {
    case wifi_state::unconfigured:
        return "UNCONFIG";
    case wifi_state::disconnected:
        return "DISCONNECTED";
    case wifi_state::connecting:
        return "CONNECTING";
    case wifi_state::connected:
        return "CONNECTED";
    }
    return "UNKNOWN";
}

} // namespace frame::m1

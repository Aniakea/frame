#include "status_log.hh"

#include <cinttypes>
#include <cstdio>
#include <ctime>

namespace frame::m1 {
namespace {

uint32_t crc32c(const uint8_t* data, std::size_t size) {
    uint32_t crc = UINT32_MAX;
    for (std::size_t index = 0; index < size; ++index) {
        crc ^= data[index];
        for (int bit = 0; bit < 8; ++bit) {
            const uint32_t mask = 0U - (crc & 1U);
            crc = (crc >> 1U) ^ (UINT32_C(0x82F63B78) & mask);
        }
    }
    return ~crc;
}

} // namespace

std::size_t format_status_log(const status_snapshot& status, uint64_t sequence, int64_t uptime_us,
                              char* output, std::size_t capacity, const char* event,
                              const char* mode) {
    if (output == nullptr || capacity == 0 || event == nullptr || mode == nullptr) {
        return 0;
    }

    char utc[32]{};
    if (status.rtc.valid) {
        const time_t seconds = static_cast<time_t>(status.rtc.unix_seconds);
        struct tm utc_time {};
        if (gmtime_r(&seconds, &utc_time) != nullptr) {
            const unsigned year = static_cast<unsigned>(utc_time.tm_year + 1900);
            std::snprintf(
                utc, sizeof(utc), "%04u-%02u-%02uT%02u:%02u:%02uZ", year,
                static_cast<unsigned>(utc_time.tm_mon + 1), static_cast<unsigned>(utc_time.tm_mday),
                static_cast<unsigned>(utc_time.tm_hour), static_cast<unsigned>(utc_time.tm_min),
                static_cast<unsigned>(utc_time.tm_sec));
        }
    }

    const int payload_length = std::snprintf(
        output, capacity,
        "{\"schema_version\":1,\"sequence\":%" PRIu64 ",\"uptime_us\":%" PRId64
        ",\"utc_valid\":%s,\"utc\":\"%s\",\"source\":\"m1\",\"event\":\"%s\","
        "\"mode\":\"%s\",\"status\":\"%s\",\"device_id\":\"%s\","
        "\"app\":\"%s\",\"idf\":\"%s\",\"toolchain\":\"%s\",\"image_sha256\":\"%s\","
        "\"partitions\":%s,\"reset_reason\":%" PRIu32 ",\"tf\":\"%s\",\"wifi\":\"%s\","
        "\"ap_setup\":%s,"
        "\"rtc\":%s,\"shtc3\":%s,\"sensor_valid\":%s,"
        "\"temperature_tenths_c\":%d,\"humidity_tenths_percent\":%u,"
        "\"key_short\":%" PRIu64 ",\"key_long\":%" PRIu64,
        sequence, uptime_us, status.rtc.valid ? "true" : "false", utc, event, mode,
        hardware_status::health_name(hardware_status::overall_health(status)), status.device_id,
        status.app_version, status.idf_version, status.toolchain_version, status.image_sha256,
        status.partitions_ok ? "true" : "false", status.reset_reason,
        hardware_status::sd_state_name(status.storage),
        hardware_status::wifi_state_name(status.wifi), status.ap_active ? "true" : "false",
        status.rtc_present ? "true" : "false", status.sensor_present ? "true" : "false",
        status.sensor.valid ? "true" : "false",
        status.sensor.valid ? static_cast<int>(status.sensor.temperature_tenths_celsius) : 0,
        status.sensor.valid ? static_cast<unsigned>(status.sensor.humidity_tenths_percent) : 0U,
        status.key_short_presses, status.key_long_presses);
    if (payload_length < 0 || static_cast<std::size_t>(payload_length) >= capacity) {
        output[0] = '\0';
        return 0;
    }

    const std::size_t payload_size = static_cast<std::size_t>(payload_length);
    const uint32_t checksum = crc32c(reinterpret_cast<const uint8_t*>(output), payload_size);
    const int suffix_length = std::snprintf(output + payload_size, capacity - payload_size,
                                            ",\"crc32c\":\"%08" PRIx32 "\"}\n", checksum);
    if (suffix_length < 0 || static_cast<std::size_t>(suffix_length) >= capacity - payload_size) {
        output[0] = '\0';
        return 0;
    }
    return payload_size + static_cast<std::size_t>(suffix_length);
}

} // namespace frame::m1

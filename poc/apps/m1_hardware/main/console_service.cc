#include "console_service.hh"

#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <ctime>

#include "board_config.hh"
#include "esp_console.h"
#include "linenoise/linenoise.h"

namespace frame::m1 {
namespace {

constexpr char kSelftestSsid[] = "frame-selftest";
constexpr char kSelftestPassword[] = "frame-selftest-pass";

} // namespace

esp_err_t console_service::start() {
    if (instance_ != nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    instance_ = this;

    const esp_console_cmd_t status_cmd{
        .command = "status",
        .help = "Show Frame M1 status; add --json for machine output",
        .hint = nullptr,
        .func = &console_service::status_command,
        .argtable = nullptr,
        .func_w_context = nullptr,
        .context = nullptr,
    };
    const esp_console_cmd_t wifi_cmd{
        .command = "wifi",
        .help = "wifi set <ssid> [hidden] | wifi clear | wifi reconnect | wifi status; "
                "provisioning: prov status | prov start | prov scan",
        .hint = nullptr,
        .func = &console_service::wifi_command,
        .argtable = nullptr,
        .func_w_context = nullptr,
        .context = nullptr,
    };
    const esp_console_cmd_t prov_cmd{
        .command = "prov",
        .help = "prov status | prov start | prov scan",
        .hint = nullptr,
        .func = &console_service::prov_command,
        .argtable = nullptr,
        .func_w_context = nullptr,
        .context = nullptr,
    };
    const esp_console_cmd_t storage_cmd{
        .command = "storage",
        .help = "storage probe | storage status",
        .hint = nullptr,
        .func = &console_service::storage_command,
        .argtable = nullptr,
        .func_w_context = nullptr,
        .context = nullptr,
    };
    const esp_console_cmd_t selftest_cmd{
        .command = "selftest",
        .help = "Run the safe M1 status self-test",
        .hint = nullptr,
        .func = &console_service::selftest_command,
        .argtable = nullptr,
        .func_w_context = nullptr,
        .context = nullptr,
    };

    esp_err_t result = esp_console_cmd_register(&status_cmd);
    if (result == ESP_OK) {
        result = esp_console_cmd_register(&wifi_cmd);
    }
    if (result == ESP_OK) {
        result = esp_console_cmd_register(&prov_cmd);
    }
    if (result == ESP_OK) {
        result = esp_console_cmd_register(&storage_cmd);
    }
    if (result == ESP_OK) {
        result = esp_console_cmd_register(&selftest_cmd);
    }
    if (result != ESP_OK) {
        return result;
    }

    linenoiseSetDumbMode(1);
    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_config.prompt = "frame> ";
    repl_config.max_cmdline_length = 160;
    repl_config.max_cmdline_args = 8;
    repl_config.task_stack_size = 6144;
    repl_config.task_priority = 5;
    repl_config.task_core_id = 1;
    esp_console_dev_usb_serial_jtag_config_t device_config =
        ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    esp_console_repl_t* repl = nullptr;
    result = esp_console_new_repl_usb_serial_jtag(&device_config, &repl_config, &repl);
    return result == ESP_OK ? esp_console_start_repl(repl) : result;
}

int console_service::status_command(int argc, char** argv) {
    const bool json = argc == 2 && std::strcmp(argv[1], "--json") == 0;
    if (argc > 2 || (argc == 2 && !json)) {
        std::printf("usage: status [--json]\n");
        return 1;
    }
    instance_->print_status(json);
    return 0;
}

int console_service::wifi_command(int argc, char** argv) {
    if (argc == 2 && std::strcmp(argv[1], "status") == 0) {
        instance_->print_status(false);
        return 0;
    }
    if (argc == 2 && std::strcmp(argv[1], "clear") == 0) {
        const esp_err_t result = instance_->wifi_.disconnect();
        std::printf("wifi clear: %s\n", esp_err_to_name(result));
        return result == ESP_OK ? 0 : 1;
    }
    if (argc == 2 && std::strcmp(argv[1], "reconnect") == 0) {
        const esp_err_t result = instance_->wifi_.reconnect();
        std::printf("wifi reconnect: %s\n", esp_err_to_name(result));
        return result == ESP_OK ? 0 : 1;
    }
    if ((argc == 3 || argc == 4) && std::strcmp(argv[1], "set") == 0) {
        const bool hidden = argc == 4 && std::strcmp(argv[3], "hidden") == 0;
        if (argc == 4 && !hidden) {
            std::printf("usage: wifi set <ssid> [hidden]\n");
            return 1;
        }
        char password[64]{};
        std::printf("Password (input is not stored): ");
        std::fflush(stdout);
        if (std::fgets(password, sizeof(password), stdin) == nullptr) {
            std::printf("\nwifi set: input failed\n");
            return 1;
        }
        password[std::strcspn(password, "\r\n")] = '\0';
        const esp_err_t result = instance_->wifi_.connect_ephemeral(argv[2], password, hidden);
        std::memset(password, 0, sizeof(password));
        std::printf("wifi set: %s; credentials remain in RAM only\n", esp_err_to_name(result));
        return result == ESP_OK ? 0 : 1;
    }
    std::printf("usage: wifi set <ssid> [hidden] | wifi clear | wifi reconnect | wifi status\n");
    return 1;
}

int console_service::prov_command(int argc, char** argv) {
    if (argc == 2 && std::strcmp(argv[1], "status") == 0) {
        std::printf("AP: %s\n",
                    instance_->provision_.ap_active() ? "up http://192.168.4.1/" : "down");
        std::printf("saved config: %s\n",
                    instance_->provision_.config_present() ? "present" : "absent");
        const status_snapshot value = instance_->status_.snapshot();
        std::printf("wifi: %s\n", hardware_status::wifi_state_name(value.wifi));
        return 0;
    }
    if (argc == 2 && std::strcmp(argv[1], "start") == 0) {
        if (instance_->provision_.ap_active()) {
            std::printf("provision AP already up\n");
            return 0;
        }
        instance_->wifi_.disconnect();
        const esp_err_t result = instance_->provision_.start_provision();
        std::printf("prov start: %s\n", esp_err_to_name(result));
        return result == ESP_OK ? 0 : 1;
    }
    if (argc == 2 && std::strcmp(argv[1], "scan") == 0) {
        provision_ap_info networks[15]{};
        std::size_t count = 0;
        const esp_err_t result = instance_->provision_.scan_networks(networks, 15, &count);
        if (result != ESP_OK) {
            std::printf("prov scan: %s\n", esp_err_to_name(result));
            return 1;
        }
        if (count == 0) {
            std::printf("no networks found\n");
        }
        for (std::size_t entry = 0; entry < count; ++entry) {
            std::printf("%2u) %-32s %4d dBm  %s\n", static_cast<unsigned>(entry + 1),
                        networks[entry].ssid, networks[entry].rssi,
                        networks[entry].secure ? "secure" : "open");
        }
        return 0;
    }
    std::printf("usage: prov status | prov start | prov scan\n");
    return 1;
}

int console_service::storage_command(int argc, char** argv) {
    if (argc == 2 && std::strcmp(argv[1], "probe") == 0) {
        instance_->storage_.request_probe();
        std::printf("storage probe requested\n");
        return 0;
    }
    if (argc == 2 && std::strcmp(argv[1], "status") == 0) {
        instance_->print_status(false);
        return 0;
    }
    std::printf("usage: storage probe | storage status\n");
    return 1;
}

int console_service::selftest_command(int argc, char** argv) {
    if (argc != 1) {
        std::printf("usage: selftest\n");
        return 1;
    }
    instance_->storage_.request_probe();
    instance_->print_status(true);
    instance_->run_wifi_config_selftest();
    return 0;
}

void console_service::run_wifi_config_selftest() const {
    char prior_ssid[33]{};
    char prior_password[64]{};
    bool prior_hidden = false;
    const bool had_config =
        provision_.config_load(prior_ssid, prior_password, prior_hidden) == ESP_OK;
    const esp_err_t save_result = provision_.config_save(kSelftestSsid, kSelftestPassword, false);
    char ssid[33]{};
    char password[64]{};
    bool hidden = true;
    esp_err_t load_result = ESP_ERR_INVALID_STATE;
    if (save_result == ESP_OK) {
        load_result = provision_.config_load(ssid, password, hidden);
    }
    const bool round_trip = save_result == ESP_OK && load_result == ESP_OK &&
                            std::strcmp(ssid, kSelftestSsid) == 0 &&
                            std::strcmp(password, kSelftestPassword) == 0 && !hidden;
    std::printf("wifi_config: %s\n", round_trip ? "ok" : "FAIL");
    if (had_config) {
        provision_.config_save(prior_ssid, prior_password, prior_hidden);
    } else {
        provision_.config_clear();
    }
    std::memset(prior_ssid, 0, sizeof(prior_ssid));
    std::memset(prior_password, 0, sizeof(prior_password));
    char bad_ssid[33]{};
    char bad_password[64]{};
    bool bad_hidden = false;
    const esp_err_t bad_result =
        wifi_provision::parse_config("{not json", bad_ssid, bad_password, bad_hidden);
    std::printf("wifi_config: %s\n", bad_result == ESP_ERR_NOT_FOUND ? "bad ignored" : "FAIL");
}

void console_service::print_status(bool json) const {
    const status_snapshot value = status_.snapshot();
    const char* health = hardware_status::health_name(hardware_status::overall_health(value));
    if (json) {
        char utc[32]{};
        if (value.rtc.valid) {
            const time_t seconds = static_cast<time_t>(value.rtc.unix_seconds);
            struct tm utc_time {};
            if (gmtime_r(&seconds, &utc_time) != nullptr) {
                const unsigned year = static_cast<unsigned>(utc_time.tm_year + 1900);
                std::snprintf(utc, sizeof(utc), "%04u-%02u-%02uT%02u:%02u:%02uZ", year,
                              static_cast<unsigned>(utc_time.tm_mon + 1),
                              static_cast<unsigned>(utc_time.tm_mday),
                              static_cast<unsigned>(utc_time.tm_hour),
                              static_cast<unsigned>(utc_time.tm_min),
                              static_cast<unsigned>(utc_time.tm_sec));
            }
        }
        std::printf(
            "{\"schema_version\":1,\"firmware\":\"%s\",\"idf\":\"%s\","
            "\"toolchain\":\"%s\",\"device_id\":\"%s\",\"health\":\"%s\",\"target_ok\":%s,"
            "\"cores\":%" PRIu32 ",\"flash_bytes\":%u,\"psram_bytes\":%u,\"partitions_ok\":%s,"
            "\"reset_reason\":%" PRIu32 ",\"display\":\"%s\",\"tf\":\"%s\",\"rtc\":%s,"
            "\"rtc_valid\":%s,\"utc\":\"%s\",\"shtc3\":%s,\"sensor_valid\":%s,"
            "\"wifi\":\"%s\",\"ap_active\":%s,\"credentials_persisted\":%s,"
            "\"tf_logging\":%s,\"logging_error\":%d,\"temperature_tenths_c\":%d,"
            "\"humidity_tenths_percent\":%u,\"key_short\":%" PRIu64 ",\"key_long\":%" PRIu64
            ",\"elf_sha256\":\"%s\",\"image_sha256\":\"%s\"}\n",
            board::kFirmwareVersion, board::kIdfTag, value.toolchain_version, value.device_id,
            health, value.target_ok ? "true" : "false", value.cpu_cores,
            static_cast<unsigned>(value.flash_bytes), static_cast<unsigned>(value.psram_bytes),
            value.partitions_ok ? "true" : "false", value.reset_reason,
            value.display_error == ESP_OK ? "OK" : "FAILED",
            hardware_status::sd_state_name(value.storage), value.rtc_present ? "true" : "false",
            value.rtc.valid ? "true" : "false", utc, value.sensor_present ? "true" : "false",
            value.sensor.valid ? "true" : "false", hardware_status::wifi_state_name(value.wifi),
            value.ap_active ? "true" : "false", provision_.config_present() ? "true" : "false",
            value.tf_logging_ok ? "true" : "false", static_cast<int>(value.logging_error),
            value.sensor.valid ? static_cast<int>(value.sensor.temperature_tenths_celsius) : 0,
            value.sensor.valid ? static_cast<unsigned>(value.sensor.humidity_tenths_percent) : 0U,
            value.key_short_presses, value.key_long_presses, value.elf_sha256, value.image_sha256);
        return;
    }

    char rtc_text[32]{};
    if (value.rtc.valid) {
        const time_t seconds = static_cast<time_t>(value.rtc.unix_seconds);
        struct tm utc_time {};
        if (gmtime_r(&seconds, &utc_time) != nullptr) {
            const unsigned year = static_cast<unsigned>(utc_time.tm_year + 1900);
            std::snprintf(
                rtc_text, sizeof(rtc_text), "%04u-%02u-%02u %02u:%02u:%02u", year,
                static_cast<unsigned>(utc_time.tm_mon + 1), static_cast<unsigned>(utc_time.tm_mday),
                static_cast<unsigned>(utc_time.tm_hour), static_cast<unsigned>(utc_time.tm_min),
                static_cast<unsigned>(utc_time.tm_sec));
        }
    }
    std::printf("Frame M1 %s (%s)\n", board::kFirmwareVersion, health);
    std::printf("  device: %s  image: %.16s\n  IDF: %s  toolchain: %s\n", value.device_id,
                value.image_sha256, value.idf_version, value.toolchain_version);
    std::printf("  target: %s  cores: %" PRIu32 "  flash: %u  PSRAM: %u  part: %s\n",
                value.target_ok ? "OK" : "FAIL", value.cpu_cores,
                static_cast<unsigned>(value.flash_bytes), static_cast<unsigned>(value.psram_bytes),
                value.partitions_ok ? "OK" : "FAIL");
    std::printf("  display: %s  TF: %s  logging: %s  reset: %" PRIu32 "\n",
                value.display_error == ESP_OK ? "OK" : "FAILED",
                hardware_status::sd_state_name(value.storage), value.tf_logging_ok ? "OK" : "WAIT",
                value.reset_reason);
    std::printf("  RTC: %s %s  SHTC3: %s  KEY: %" PRIu64 "/%" PRIu64 "\n",
                value.rtc_present ? "OK" : "MISS", rtc_text[0] == '\0' ? "-" : rtc_text,
                value.sensor_present ? "OK" : "MISS", value.key_short_presses,
                value.key_long_presses);
    std::printf("  WiFi: %s  SSID: %s  IPv4: %s  credentials: system_fs (dev)\n",
                hardware_status::wifi_state_name(value.wifi), value.ssid_configured ? "set" : "-",
                value.ip_address[0] == '\0' ? "-" : value.ip_address);
    if (value.ap_active) {
        std::printf("  AP: up 192.168.4.1\n");
    }
}

} // namespace frame::m1

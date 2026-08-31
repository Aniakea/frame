#include "wifi_provision.hh"

#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstring>

#include <fcntl.h>
#include <unistd.h>

#include "cJSON.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"

namespace frame::m1 {
namespace {

constexpr char kTag[] = "frame-prov";
constexpr char kMountPoint[] = "/system";
constexpr char kPartitionLabel[] = "system_fs";
constexpr char kConfigPath[] = "/system/wifi.json";
constexpr char kTempPath[] = "/system/wifi.json.tmp";
constexpr std::size_t kMaxBodyLength = 512;
constexpr std::size_t kMaxScanRecords = 24;
constexpr std::size_t kMaxScanResults = 15;
constexpr int kFallbackDisconnects = 5;
constexpr int kFallbackNoIpSeconds = 60;
constexpr int kMonitorTickMs = 1000;

constexpr char kPageHead[] =
    "<!DOCTYPE html>\n"
    "<html lang=\"en\">\n"
    "<head>\n"
    "<meta charset=\"utf-8\">\n"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">\n"
    "<title>Frame Setup</title>\n"
    "<style>\n"
    "body{font-family:sans-serif;background:#111;color:#eee;max-width:30rem;margin:2rem auto;"
    "padding:0 1rem}\n"
    "form{background:#1d1d1d;border:1px solid #333;border-radius:8px;padding:1rem;margin:1rem 0}\n"
    "label{display:block;margin:.6rem 0 .2rem;font-size:.9rem;color:#aaa}\n"
    "select,input[type=password]{width:100%;box-sizing:border-box;padding:.5rem;font-size:1rem;"
    "border-radius:4px;border:1px solid #444;background:#111;color:#eee}\n"
    "button{margin-top:1rem;padding:.6rem 1.4rem;font-size:1rem;border:0;border-radius:4px;"
    "background:#4a7dff;color:#fff;cursor:pointer}\n"
    ".small{color:#888;font-size:.8rem}\n"
    "</style>\n"
    "</head>\n"
    "<body>\n"
    "<h1>Frame Setup</h1>\n"
    "<p class=\"small\">Pick a Wi-Fi network for this Frame device.</p>\n"
    "<form method=\"post\" action=\"/connect\">\n"
    "<label for=\"ssid\">Network</label>\n"
    "<select id=\"ssid\" name=\"ssid\" required>\n";

constexpr char kPageTail[] =
    "</select>\n"
    "<label for=\"password\">Password</label>\n"
    "<input id=\"password\" type=\"password\" name=\"password\" minlength=\"8\" maxlength=\"63\" "
    "required>\n"
    "<label><input type=\"checkbox\" name=\"hidden\" value=\"on\"> Hidden network</label>\n"
    "<button type=\"submit\">Save &amp; connect</button>\n"
    "</form>\n"
    "<form method=\"post\" action=\"/clear\">\n"
    "<button type=\"submit\">Forget saved network</button>\n"
    "</form>\n"
    "<p class=\"small\">After saving, the device connects and this hotspot disappears. "
    "If it fails, the hotspot returns.</p>\n"
    "</body>\n"
    "</html>\n";

constexpr char kConnectPage[] =
    "<!DOCTYPE html>\n"
    "<html lang=\"en\">\n"
    "<head><meta charset=\"utf-8\"><meta name=\"viewport\" "
    "content=\"width=device-width,initial-scale=1\"><title>Frame Setup</title></head>\n"
    "<body style=\"font-family:sans-serif;background:#111;color:#eee;max-width:30rem;"
    "margin:2rem auto;padding:0 1rem\">\n"
    "<h1>Saved</h1>\n"
    "<p>Connecting &mdash; check the device screen; if it fails the AP returns.</p>\n"
    "</body>\n"
    "</html>\n";

constexpr char kClearPage[] =
    "<!DOCTYPE html>\n"
    "<html lang=\"en\">\n"
    "<head><meta charset=\"utf-8\"><meta name=\"viewport\" "
    "content=\"width=device-width,initial-scale=1\"><title>Frame Setup</title></head>\n"
    "<body style=\"font-family:sans-serif;background:#111;color:#eee;max-width:30rem;"
    "margin:2rem auto;padding:0 1rem\">\n"
    "<h1>Cleared</h1>\n"
    "<p>Saved Wi-Fi credentials removed. The setup hotspot is restarting.</p>\n"
    "</body>\n"
    "</html>\n";

int hex_value(char digit) {
    if (digit >= '0' && digit <= '9') {
        return digit - '0';
    }
    if (digit >= 'a' && digit <= 'f') {
        return digit - 'a' + 10;
    }
    if (digit >= 'A' && digit <= 'F') {
        return digit - 'A' + 10;
    }
    return -1;
}

std::size_t url_decode(const char* source, std::size_t length, char* destination,
                       std::size_t capacity) {
    std::size_t out = 0;
    for (std::size_t i = 0; i < length && out + 1 < capacity; ++i) {
        char decoded = source[i];
        if (decoded == '+') {
            decoded = ' ';
        } else if (decoded == '%' && i + 2 < length) {
            const int high = hex_value(source[i + 1]);
            const int low = hex_value(source[i + 2]);
            if (high >= 0 && low >= 0) {
                decoded = static_cast<char>((high << 4) | low);
                i += 2;
            }
        }
        destination[out++] = decoded;
    }
    destination[out] = '\0';
    return out;
}

void parse_form(const char* body, char (&ssid)[129], char (&password)[129], bool& hidden) {
    ssid[0] = '\0';
    password[0] = '\0';
    hidden = false;
    std::size_t cursor = 0;
    const std::size_t length = std::strlen(body);
    while (cursor <= length) {
        std::size_t pair_end = cursor;
        while (pair_end < length && body[pair_end] != '&') {
            ++pair_end;
        }
        const void* found = std::memchr(body + cursor, '=', pair_end - cursor);
        if (found != nullptr) {
            const std::size_t equal =
                static_cast<std::size_t>(static_cast<const char*>(found) - (body + cursor));
            char key[32]{};
            char value[129]{};
            url_decode(body + cursor, equal < sizeof(key) - 1 ? equal : 0, key, sizeof(key));
            url_decode(body + cursor + equal + 1, pair_end - cursor - equal - 1, value,
                       sizeof(value));
            if (std::strcmp(key, "ssid") == 0) {
                std::memcpy(ssid, value, sizeof(ssid));
            } else if (std::strcmp(key, "password") == 0) {
                std::memcpy(password, value, sizeof(password));
            } else if (std::strcmp(key, "hidden") == 0) {
                hidden = std::strcmp(value, "on") == 0;
            }
        }
        if (pair_end >= length) {
            break;
        }
        cursor = pair_end + 1;
    }
}

void html_escape(const char* text, char (&output)[193]) {
    std::size_t out = 0;
    for (std::size_t i = 0; text[i] != '\0' && out + 6 < sizeof(output); ++i) {
        const char digit = text[i];
        if (digit == '&') {
            std::memcpy(output + out, "&amp;", 5);
            out += 5;
        } else if (digit == '<') {
            std::memcpy(output + out, "&lt;", 4);
            out += 4;
        } else if (digit == '>') {
            std::memcpy(output + out, "&gt;", 4);
            out += 4;
        } else if (digit == '"') {
            std::memcpy(output + out, "&#34;", 5);
            out += 5;
        } else if (digit == '\'') {
            std::memcpy(output + out, "&#39;", 5);
            out += 5;
        } else if (digit >= ' ') {
            output[out++] = digit;
        }
    }
    output[out] = '\0';
}

void wipe_credentials(char* ssid, char* password) {
    std::memset(ssid, 0, 33);
    std::memset(password, 0, 64);
}

std::size_t bounded_length(const char* text, std::size_t capacity) {
    std::size_t length = 0;
    while (length < capacity && text[length] != '\0') {
        ++length;
    }
    return length;
}

} // namespace

esp_err_t wifi_provision::initialize() {
    esp_vfs_littlefs_conf_t config{};
    config.base_path = kMountPoint;
    config.partition_label = kPartitionLabel;
    config.format_if_mount_failed = false;
    const esp_err_t result = esp_vfs_littlefs_register(&config);
    if (result != ESP_OK) {
        mounted_ = false;
        ESP_LOGW(kTag, "system_fs unavailable: %s", esp_err_to_name(result));
        return ESP_ERR_INVALID_STATE;
    }
    mounted_ = true;
    ESP_LOGI(kTag, "system_fs mounted");
    return ESP_OK;
}

esp_err_t wifi_provision::config_load(char ssid[33], char password[64], bool& hidden) {
    FILE* file = std::fopen(kConfigPath, "r");
    if (file == nullptr) {
        return ESP_ERR_NOT_FOUND;
    }
    char text[512]{};
    const std::size_t length = std::fread(text, 1, sizeof(text) - 1, file);
    const bool overlong = length == sizeof(text) - 1 && std::fgetc(file) != EOF;
    std::fclose(file);
    if (overlong) {
        return ESP_ERR_NOT_FOUND;
    }
    text[length] = '\0';
    return parse_config(text, ssid, password, hidden);
}

esp_err_t wifi_provision::parse_config(const char* text, char ssid[33], char password[64],
                                       bool& hidden) {
    if (text == nullptr) {
        return ESP_ERR_NOT_FOUND;
    }
    cJSON* root = cJSON_Parse(text);
    if (root == nullptr) {
        return ESP_ERR_NOT_FOUND;
    }
    const cJSON* ssid_item = cJSON_GetObjectItem(root, "ssid");
    const cJSON* password_item = cJSON_GetObjectItem(root, "password");
    const cJSON* hidden_item = cJSON_GetObjectItem(root, "hidden");
    esp_err_t result = ESP_ERR_NOT_FOUND;
    if (cJSON_IsString(ssid_item) && cJSON_IsString(password_item) &&
        (hidden_item == nullptr || cJSON_IsBool(hidden_item))) {
        const char* ssid_value = ssid_item->valuestring;
        const char* password_value = password_item->valuestring;
        if (ssid_value != nullptr && password_value != nullptr) {
            const std::size_t ssid_length = std::strlen(ssid_value);
            const std::size_t password_length = std::strlen(password_value);
            if (ssid_length >= 1 && ssid_length <= 32 && password_length >= 8 &&
                password_length <= 63) {
                std::memcpy(ssid, ssid_value, ssid_length + 1);
                std::memcpy(password, password_value, password_length + 1);
                hidden = cJSON_IsTrue(hidden_item) != 0;
                result = ESP_OK;
            }
        }
    }
    cJSON_Delete(root);
    return result;
}

esp_err_t wifi_provision::config_save(const char* ssid, const char* password, bool hidden) {
    if (!mounted_ || ssid == nullptr || password == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    const std::size_t ssid_length = std::strlen(ssid);
    const std::size_t password_length = std::strlen(password);
    if (ssid_length == 0 || ssid_length > 32 || password_length < 8 || password_length > 63) {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON* root = cJSON_CreateObject();
    if (root == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t result = ESP_ERR_NO_MEM;
    if (cJSON_AddStringToObject(root, "ssid", ssid) != nullptr &&
        cJSON_AddStringToObject(root, "password", password) != nullptr &&
        cJSON_AddBoolToObject(root, "hidden", hidden) != nullptr) {
        char* text = cJSON_Print(root);
        if (text != nullptr) {
            result = write_config_atomic(text);
            cJSON_free(text);
        }
    }
    cJSON_Delete(root);
    return result;
}

esp_err_t wifi_provision::write_config_atomic(const char* text) {
    const int descriptor = open(kTempPath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (descriptor < 0) {
        return ESP_ERR_INVALID_STATE;
    }
    const std::size_t length = std::strlen(text);
    std::size_t written = 0;
    while (written < length) {
        const ssize_t chunk = write(descriptor, text + written, length - written);
        if (chunk <= 0) {
            close(descriptor);
            std::remove(kTempPath);
            return ESP_FAIL;
        }
        written += static_cast<std::size_t>(chunk);
    }
    const int sync_result = fsync(descriptor);
    const int close_result = close(descriptor);
    if (sync_result != 0 || close_result != 0) {
        std::remove(kTempPath);
        return ESP_FAIL;
    }
    if (std::rename(kTempPath, kConfigPath) != 0) {
        std::remove(kTempPath);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t wifi_provision::config_clear() {
    if (std::remove(kConfigPath) != 0 && errno != ENOENT) {
        return ESP_FAIL;
    }
    if (std::remove(kTempPath) != 0 && errno != ENOENT) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

bool wifi_provision::config_present() {
    char ssid[33]{};
    char password[64]{};
    bool hidden = false;
    const bool present = config_load(ssid, password, hidden) == ESP_OK;
    wipe_credentials(ssid, password);
    return present;
}

esp_err_t wifi_provision::start_saved_or_pending() {
    const esp_err_t runtime = ensure_runtime();
    if (runtime != ESP_OK) {
        return runtime;
    }
    char ssid[33]{};
    char password[64]{};
    bool hidden = false;
    const esp_err_t result = config_load(ssid, password, hidden);
    if (result == ESP_OK) {
        const esp_err_t connect = wifi_.connect_stored(ssid, password, hidden);
        wipe_credentials(ssid, password);
        return connect;
    }
    wipe_credentials(ssid, password);
    ESP_LOGI(kTag, "wifi provision pending");
    return start_provision();
}

esp_err_t wifi_provision::ensure_runtime() {
    if (runtime_ready_.load()) {
        return ESP_OK;
    }
    esp_err_t result = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                  &wifi_provision::event_trampoline, this);
    if (result == ESP_OK) {
        result = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                            &wifi_provision::event_trampoline, this);
    }
    if (result == ESP_OK && monitor_ == nullptr) {
        if (xTaskCreate(&wifi_provision::monitor_trampoline, "prov-mon", 6144, this, 5,
                        &monitor_) != pdPASS) {
            monitor_ = nullptr;
            result = ESP_ERR_NO_MEM;
        }
    }
    if (result == ESP_OK) {
        runtime_ready_.store(true);
    }
    return result;
}

void wifi_provision::build_ap_name(char (&name)[17]) const {
    const status_snapshot value = status_.snapshot();
    const std::size_t length = bounded_length(value.device_id, sizeof(value.device_id));
    const char* suffix = length >= 4 ? value.device_id + length - 4 : "0000";
    std::snprintf(name, sizeof(name), "Frame-Setup-%.4s", suffix);
}

esp_err_t wifi_provision::start_provision() {
    if (mutex_ == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(mutex_, portMAX_DELAY);
    if (ap_active_.load()) {
        xSemaphoreGive(mutex_);
        return ESP_OK;
    }
    esp_err_t result = ensure_runtime();
    if (result == ESP_OK) {
        result = esp_wifi_stop();
    }
    if (result == ESP_OK) {
        result = esp_wifi_set_mode(WIFI_MODE_APSTA);
    }
    if (result == ESP_OK && !ap_netif_created_) {
        if (esp_netif_create_default_wifi_ap() == nullptr) {
            result = ESP_ERR_NO_MEM;
        } else {
            ap_netif_created_ = true;
        }
    }
    char ap_name[17]{};
    if (result == ESP_OK) {
        build_ap_name(ap_name);
        wifi_config_t ap{};
        const std::size_t ssid_length = std::strlen(ap_name);
        std::memcpy(ap.ap.ssid, ap_name, ssid_length);
        ap.ap.ssid_len = static_cast<uint8_t>(ssid_length);
        ap.ap.channel = 1;
        ap.ap.authmode = WIFI_AUTH_OPEN;
        ap.ap.max_connection = 2;
        ap.ap.beacon_interval = 100;
        result = esp_wifi_set_config(WIFI_IF_AP, &ap);
    }
    if (result == ESP_OK) {
        result = esp_wifi_start();
    }
    if (result == ESP_OK) {
        result = start_httpd();
    }
    if (result == ESP_OK) {
        ap_active_.store(true);
        status_.update([](status_snapshot& value) { value.ap_active = true; });
        ESP_LOGI(kTag, "provision AP up %s at 192.168.4.1", ap_name);
    } else {
        ESP_LOGW(kTag, "provision AP start failed: %s", esp_err_to_name(result));
        status_.update([](status_snapshot& value) { value.ap_active = false; });
        esp_wifi_set_mode(WIFI_MODE_STA);
        esp_wifi_start();
    }
    xSemaphoreGive(mutex_);
    return result;
}

esp_err_t wifi_provision::stop_provision() {
    if (mutex_ == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(mutex_, portMAX_DELAY);
    if (!ap_active_.load()) {
        xSemaphoreGive(mutex_);
        return ESP_OK;
    }
    if (server_ != nullptr) {
        httpd_stop(server_);
        server_ = nullptr;
    }
    const esp_err_t result = esp_wifi_set_mode(WIFI_MODE_STA);
    if (result == ESP_OK) {
        ap_active_.store(false);
        status_.update([](status_snapshot& value) { value.ap_active = false; });
        ESP_LOGI(kTag, "provision AP down");
    } else {
        ESP_LOGW(kTag, "provision AP stop failed: %s", esp_err_to_name(result));
    }
    xSemaphoreGive(mutex_);
    return result;
}

esp_err_t wifi_provision::start_httpd() {
    if (server_ != nullptr) {
        return ESP_OK;
    }
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 8192;
    config.max_uri_handlers = 8;
    config.lru_purge_enable = true;
    esp_err_t result = httpd_start(&server_, &config);
    if (result != ESP_OK) {
        server_ = nullptr;
        return result;
    }

    const httpd_uri_t root{
        .uri = "/",
        .method = HTTP_GET,
        .handler = &wifi_provision::root_handler,
        .user_ctx = this,
    };
    const httpd_uri_t scan{
        .uri = "/scan",
        .method = HTTP_GET,
        .handler = &wifi_provision::scan_handler,
        .user_ctx = this,
    };
    const httpd_uri_t connect{
        .uri = "/connect",
        .method = HTTP_POST,
        .handler = &wifi_provision::connect_handler,
        .user_ctx = this,
    };
    const httpd_uri_t clear{
        .uri = "/clear",
        .method = HTTP_POST,
        .handler = &wifi_provision::clear_handler,
        .user_ctx = this,
    };
    result = httpd_register_uri_handler(server_, &root);
    if (result == ESP_OK) {
        result = httpd_register_uri_handler(server_, &scan);
    }
    if (result == ESP_OK) {
        result = httpd_register_uri_handler(server_, &connect);
    }
    if (result == ESP_OK) {
        result = httpd_register_uri_handler(server_, &clear);
    }
    if (result != ESP_OK) {
        httpd_stop(server_);
        server_ = nullptr;
    }
    return result;
}

void wifi_provision::monitor_trampoline(void* context) {
    static_cast<wifi_provision*>(context)->monitor_loop();
}

void wifi_provision::event_trampoline(void* context, esp_event_base_t base, int32_t id,
                                      void* /*data*/) {
    static_cast<wifi_provision*>(context)->handle_event(base, id);
}

void wifi_provision::handle_event(esp_event_base_t base, int32_t id) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        has_ip_.store(false);
        disconnect_count_.fetch_add(1);
        return;
    }
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        has_ip_.store(true);
        disconnect_count_.store(0);
        no_ip_ticks_.store(0);
        if (ap_active_.load()) {
            pending_action expected = pending_action::none;
            pending_.compare_exchange_strong(expected, pending_action::stop_ap);
        }
    }
}

void wifi_provision::monitor_loop() {
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(kMonitorTickMs));
        const pending_action action = pending_.exchange(pending_action::none);
        if (action == pending_action::sta_connect) {
            stop_provision();
            disconnect_count_.store(0);
            no_ip_ticks_.store(0);
            wifi_.connect_stored(pending_ssid_, pending_password_, pending_hidden_);
            std::memset(pending_ssid_, 0, sizeof(pending_ssid_));
            std::memset(pending_password_, 0, sizeof(pending_password_));
            continue;
        }
        if (action == pending_action::restart_ap) {
            stop_provision();
            disconnect_count_.store(0);
            no_ip_ticks_.store(0);
            start_provision();
            continue;
        }
        if (action == pending_action::stop_ap) {
            stop_provision();
            continue;
        }

        bool portal_requested = false;
        status_.update([&](status_snapshot& value) {
            if (value.ap_mode_requested) {
                value.ap_mode_requested = false;
                portal_requested = true;
            }
        });
        if (portal_requested) {
            wifi_.disconnect();
            disconnect_count_.store(0);
            no_ip_ticks_.store(0);
            start_provision();
            continue;
        }

        const int disconnects = disconnect_count_.load();
        if (disconnects >= kFallbackDisconnects) {
            ESP_LOGI(kTag, "wifi fallback: %d disconnects without IP", disconnects);
            wifi_.disconnect();
            disconnect_count_.store(0);
            no_ip_ticks_.store(0);
            start_provision();
            continue;
        }
        if (wifi_.configured() && !has_ip_.load()) {
            const int ticks = no_ip_ticks_.fetch_add(1) + 1;
            if (ticks >= kFallbackNoIpSeconds) {
                ESP_LOGI(kTag, "wifi fallback: no IP for %ds", ticks);
                wifi_.disconnect();
                disconnect_count_.store(0);
                no_ip_ticks_.store(0);
                start_provision();
            }
        } else {
            no_ip_ticks_.store(0);
        }
    }
}

esp_err_t wifi_provision::scan_networks(provision_ap_info* results, std::size_t capacity,
                                        std::size_t* count) {
    *count = 0;
    if (results == nullptr || capacity == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (mutex_ == nullptr || xSemaphoreTake(mutex_, pdMS_TO_TICKS(5000)) != pdTRUE) {
        return ESP_ERR_INVALID_STATE;
    }
    static wifi_ap_record_t records[kMaxScanRecords];
    wifi_scan_config_t scan{};
    scan.scan_time.active.min = 0;
    scan.scan_time.active.max = 80;
    scan.scan_time.passive = 120;
    esp_err_t result = esp_wifi_scan_start(&scan, true);
    if (result == ESP_OK) {
        uint16_t number = static_cast<uint16_t>(kMaxScanRecords);
        result = esp_wifi_scan_get_ap_records(&number, records);
        if (result == ESP_OK) {
            provision_ap_info unique[kMaxScanRecords]{};
            std::size_t unique_count = 0;
            for (uint16_t i = 0; i < number; ++i) {
                const char* ssid = reinterpret_cast<const char*>(records[i].ssid);
                const std::size_t ssid_length = bounded_length(ssid, 32);
                if (ssid_length == 0) {
                    continue;
                }
                std::size_t found = unique_count;
                for (std::size_t entry = 0; entry < unique_count; ++entry) {
                    if (std::memcmp(unique[entry].ssid, ssid, ssid_length) == 0 &&
                        unique[entry].ssid[ssid_length] == '\0') {
                        found = entry;
                        break;
                    }
                }
                if (found < unique_count) {
                    if (records[i].rssi > unique[found].rssi) {
                        unique[found].rssi = records[i].rssi;
                        unique[found].secure = records[i].authmode != WIFI_AUTH_OPEN;
                    }
                    continue;
                }
                if (unique_count < kMaxScanRecords) {
                    std::memcpy(unique[unique_count].ssid, ssid, ssid_length);
                    unique[unique_count].rssi = records[i].rssi;
                    unique[unique_count].secure = records[i].authmode != WIFI_AUTH_OPEN;
                    ++unique_count;
                }
            }
            for (std::size_t outer = 1; outer < unique_count; ++outer) {
                const provision_ap_info candidate = unique[outer];
                std::size_t inner = outer;
                while (inner > 0 && unique[inner - 1].rssi < candidate.rssi) {
                    unique[inner] = unique[inner - 1];
                    --inner;
                }
                unique[inner] = candidate;
            }
            *count = unique_count < capacity ? unique_count : capacity;
            std::memcpy(results, unique, *count * sizeof(provision_ap_info));
        }
    }
    xSemaphoreGive(mutex_);
    return result;
}

esp_err_t wifi_provision::root_handler(httpd_req_t* request) {
    auto* self = static_cast<wifi_provision*>(request->user_ctx);
    httpd_resp_set_type(request, HTTPD_TYPE_TEXT);
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    httpd_resp_send_chunk(request, kPageHead, HTTPD_RESP_USE_STRLEN);

    provision_ap_info networks[kMaxScanResults]{};
    std::size_t count = 0;
    const esp_err_t scan_result = self->scan_networks(networks, kMaxScanResults, &count);
    if (scan_result != ESP_OK || count == 0) {
        httpd_resp_send_chunk(request, "<option value=\"\">(no networks found)</option>\n",
                              HTTPD_RESP_USE_STRLEN);
    }
    for (std::size_t entry = 0; entry < count; ++entry) {
        char escaped_value[193]{};
        char escaped_text[193]{};
        html_escape(networks[entry].ssid, escaped_value);
        html_escape(networks[entry].ssid, escaped_text);
        char option[512]{};
        const int written =
            std::snprintf(option, sizeof(option), "<option value=\"%s\">%s (%d dBm)</option>\n",
                          escaped_value, escaped_text, networks[entry].rssi);
        if (written > 0) {
            httpd_resp_send_chunk(request, option, static_cast<std::size_t>(written));
        }
    }
    httpd_resp_send_chunk(request, kPageTail, HTTPD_RESP_USE_STRLEN);
    httpd_resp_send_chunk(request, nullptr, 0);
    return ESP_OK;
}

esp_err_t wifi_provision::scan_handler(httpd_req_t* request) {
    auto* self = static_cast<wifi_provision*>(request->user_ctx);
    provision_ap_info networks[kMaxScanResults]{};
    std::size_t count = 0;
    const esp_err_t scan_result = self->scan_networks(networks, kMaxScanResults, &count);
    cJSON* root = cJSON_CreateObject();
    cJSON* array = cJSON_AddArrayToObject(root, "aps");
    if (root == nullptr || array == nullptr) {
        cJSON_Delete(root);
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "scan failed");
    }
    if (scan_result == ESP_OK) {
        for (std::size_t entry = 0; entry < count; ++entry) {
            cJSON* item = cJSON_CreateObject();
            if (item == nullptr ||
                cJSON_AddStringToObject(item, "ssid", networks[entry].ssid) == nullptr ||
                cJSON_AddNumberToObject(item, "rssi", static_cast<double>(networks[entry].rssi)) ==
                    nullptr ||
                cJSON_AddBoolToObject(item, "secure", networks[entry].secure ? 1 : 0) == nullptr) {
                cJSON_Delete(item);
                continue;
            }
            cJSON_AddItemToArray(array, item);
        }
    }
    char* text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (text == nullptr) {
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "scan failed");
    }
    httpd_resp_set_type(request, HTTPD_TYPE_JSON);
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    const esp_err_t result = httpd_resp_send(request, text, HTTPD_RESP_USE_STRLEN);
    cJSON_free(text);
    return result;
}

esp_err_t wifi_provision::connect_handler(httpd_req_t* request) {
    auto* self = static_cast<wifi_provision*>(request->user_ctx);
    if (request->content_len > kMaxBodyLength) {
        return httpd_resp_send_err(request, HTTPD_413_CONTENT_TOO_LARGE, "body too large");
    }
    char body[kMaxBodyLength + 1]{};
    std::size_t received = 0;
    while (received < request->content_len) {
        const int chunk = httpd_req_recv(request, body + received,
                                         static_cast<std::size_t>(request->content_len) - received);
        if (chunk <= 0) {
            return httpd_resp_send_err(request, HTTPD_408_REQ_TIMEOUT, "body incomplete");
        }
        received += static_cast<std::size_t>(chunk);
    }

    char ssid[129]{};
    char password[129]{};
    bool hidden = false;
    parse_form(body, ssid, password, hidden);
    std::memset(body, 0, sizeof(body));
    const std::size_t ssid_length = std::strlen(ssid);
    const std::size_t password_length = std::strlen(password);
    if (ssid_length == 0 || ssid_length > 32 || password_length < 8 || password_length > 63) {
        std::memset(ssid, 0, sizeof(ssid));
        std::memset(password, 0, sizeof(password));
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST,
                                   "usage: ssid 1..32 characters, password 8..63 characters");
    }
    const esp_err_t save_result = self->config_save(ssid, password, hidden);
    if (save_result != ESP_OK) {
        std::memset(ssid, 0, sizeof(ssid));
        std::memset(password, 0, sizeof(password));
        ESP_LOGW(kTag, "provision save failed: %s", esp_err_to_name(save_result));
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "save failed on device storage");
    }
    std::memcpy(self->pending_ssid_, ssid, ssid_length + 1);
    std::memcpy(self->pending_password_, password, password_length + 1);
    self->pending_hidden_ = hidden;
    self->pending_.store(pending_action::sta_connect);
    std::memset(ssid, 0, sizeof(ssid));
    std::memset(password, 0, sizeof(password));

    httpd_resp_set_type(request, HTTPD_TYPE_TEXT);
    return httpd_resp_send(request, kConnectPage, HTTPD_RESP_USE_STRLEN);
}

esp_err_t wifi_provision::clear_handler(httpd_req_t* request) {
    auto* self = static_cast<wifi_provision*>(request->user_ctx);
    const esp_err_t clear_result = self->config_clear();
    if (clear_result != ESP_OK) {
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "clear failed");
    }
    self->wifi_.disconnect();
    self->pending_.store(pending_action::restart_ap);
    httpd_resp_set_type(request, HTTPD_TYPE_TEXT);
    return httpd_resp_send(request, kClearPage, HTTPD_RESP_USE_STRLEN);
}

} // namespace frame::m1

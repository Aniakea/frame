#include "wifi_service.hh"

#include <cstdio>
#include <cstring>

#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"

namespace frame::m1 {
namespace {

constexpr char kTag[] = "frame-wifi";

} // namespace

esp_err_t wifi_service::initialize() {
    esp_err_t result = esp_netif_init();
    if (result != ESP_OK && result != ESP_ERR_INVALID_STATE) {
        return result;
    }
    result = esp_event_loop_create_default();
    if (result != ESP_OK && result != ESP_ERR_INVALID_STATE) {
        return result;
    }
    if (esp_netif_create_default_wifi_sta() == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    wifi_init_config_t configuration = WIFI_INIT_CONFIG_DEFAULT();
    result = esp_wifi_init(&configuration);
    if (result != ESP_OK) {
        return result;
    }
    result = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_service::event_handler,
                                        this);
    if (result != ESP_OK) {
        return result;
    }
    result = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_service::event_handler,
                                        this);
    if (result != ESP_OK) {
        return result;
    }
    result = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (result == ESP_OK) {
        result = esp_wifi_set_mode(WIFI_MODE_STA);
    }
    if (result == ESP_OK) {
        result = esp_wifi_start();
    }
    initialized_ = result == ESP_OK;
    status_.update([&](status_snapshot& value) {
        value.wifi = wifi_state::unconfigured;
        value.wifi_error = result;
    });
    return result;
}

esp_err_t wifi_service::connect_ephemeral(const char* ssid, const char* password, bool hidden) {
    return connect_sta(ssid, password, hidden);
}

esp_err_t wifi_service::connect_stored(const char* ssid, const char* password, bool hidden) {
    return connect_sta(ssid, password, hidden);
}

esp_err_t wifi_service::connect_sta(const char* ssid, const char* password, bool hidden) {
    if (!initialized_ || ssid == nullptr || password == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    const std::size_t ssid_length = std::strlen(ssid);
    const std::size_t password_length = std::strlen(password);
    if (ssid_length == 0 || ssid_length > 32 || password_length < 8 || password_length > 63) {
        return ESP_ERR_INVALID_ARG;
    }

    wifi_config_t config{};
    std::memcpy(config.sta.ssid, ssid, ssid_length);
    std::memcpy(config.sta.password, password, password_length);
    config.sta.scan_method = hidden ? WIFI_FAST_SCAN : WIFI_ALL_CHANNEL_SCAN;
    config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;

    configured_.store(false);
    esp_wifi_disconnect();
    esp_err_t result = esp_wifi_set_config(WIFI_IF_STA, &config);
    if (result == ESP_OK) {
        status_.update([&](status_snapshot& value) {
            value.ssid_configured = true;
            value.ip_address[0] = '\0';
            value.wifi = wifi_state::connecting;
            value.wifi_error = ESP_OK;
        });
        configured_.store(true);
        result = esp_wifi_connect();
    } else {
        status_.update([](status_snapshot& value) {
            value.ssid_configured = false;
            value.ip_address[0] = '\0';
            value.wifi = wifi_state::unconfigured;
            value.wifi_error = ESP_ERR_WIFI_STATE;
        });
    }

    std::memset(&config, 0, sizeof(config));
    return result;
}

esp_err_t wifi_service::disconnect() {
    configured_.store(false);
    esp_err_t result = esp_wifi_disconnect();
    if (result == ESP_ERR_WIFI_NOT_CONNECT) {
        result = ESP_OK;
    }
    wifi_config_t empty_config{};
    const esp_err_t clear_result = esp_wifi_set_config(WIFI_IF_STA, &empty_config);
    if (result == ESP_OK) {
        result = clear_result;
    }
    status_.update([&](status_snapshot& value) {
        std::memset(value.ip_address, 0, sizeof(value.ip_address));
        value.ssid_configured = false;
        value.wifi = wifi_state::unconfigured;
        value.wifi_error = result;
    });
    return result;
}

esp_err_t wifi_service::reconnect() {
    if (!initialized_ || !configured_.load()) {
        return ESP_ERR_INVALID_STATE;
    }
    status_.update([](status_snapshot& value) { value.wifi = wifi_state::connecting; });
    return esp_wifi_connect();
}

void wifi_service::event_handler(void* context, esp_event_base_t base, int32_t id, void* data) {
    static_cast<wifi_service*>(context)->handle_event(base, id, data);
}

void wifi_service::handle_event(esp_event_base_t base, int32_t id, void* data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const bool configured = configured_.load();
        status_.update([&](status_snapshot& value) {
            value.ip_address[0] = '\0';
            value.wifi = configured ? wifi_state::disconnected : wifi_state::unconfigured;
        });
        if (configured) {
            const esp_err_t result = esp_wifi_connect();
            if (result != ESP_OK) {
                ESP_LOGW(kTag, "Reconnect failed: %s", esp_err_to_name(result));
            }
        }
        return;
    }
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const auto* event = static_cast<const ip_event_got_ip_t*>(data);
        status_.update([&](status_snapshot& value) {
            std::snprintf(value.ip_address, sizeof(value.ip_address), IPSTR,
                          IP2STR(&event->ip_info.ip));
            value.wifi = wifi_state::connected;
            value.wifi_error = ESP_OK;
        });
    }
}

} // namespace frame::m1

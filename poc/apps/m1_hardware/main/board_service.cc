#include "board_service.hh"

#include "board_config.hh"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"

namespace frame::m1 {
namespace {

constexpr char kTag[] = "frame-board";
constexpr uint16_t kSensorWake = 0x3517;
constexpr uint16_t kSensorMeasure = 0x7866;
constexpr uint16_t kSensorSleep = 0xB098;
constexpr uint8_t kRtcSecondsRegister = 0x04;
// KEY release bands: <1s short press, 1s..<3s long press, >=3s provisioning portal command.
constexpr int64_t kKeyLongPressUs = 1000000;
constexpr int64_t kKeyPortalHoldUs = 3000000;

// Days offset from 1970-01-01 (Howard Hinnant, public domain); avoids libc timegm portability.
int64_t days_from_civil(int64_t year, unsigned month, unsigned day) {
    year -= month <= 2;
    const int64_t era = (year >= 0 ? year : year - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(year - era * 400);
    const unsigned doy = (153U * (month + (month > 2 ? -3U : 9U)) + 2U) / 5U + day - 1U;
    const unsigned doe = yoe * 365U + yoe / 4U - yoe / 100U + doy;
    return era * 146097LL + static_cast<int64_t>(doe) - 719468LL;
}

uint8_t from_bcd(uint8_t value) {
    return static_cast<uint8_t>((value >> 4U) * 10U + (value & 0x0FU));
}

} // namespace

board_service::~board_service() {
    if (sensor_ != nullptr) {
        i2c_master_bus_rm_device(sensor_);
    }
    if (rtc_ != nullptr) {
        i2c_master_bus_rm_device(rtc_);
    }
    if (bus_ != nullptr) {
        i2c_del_master_bus(bus_);
    }
}

esp_err_t board_service::start() {
    gpio_config_t key_config{};
    key_config.pin_bit_mask = UINT64_C(1) << board::kKey;
    key_config.mode = GPIO_MODE_INPUT;
    key_config.pull_up_en = GPIO_PULLUP_ENABLE;
    key_config.intr_type = GPIO_INTR_DISABLE;
    esp_err_t result = gpio_config(&key_config);
    if (result != ESP_OK) {
        return result;
    }

    result = initialize_i2c();
    status_.update([&](status_snapshot& value) { value.board_error = result; });
    if (result != ESP_OK) {
        return result;
    }

    const BaseType_t created = xTaskCreatePinnedToCore(&board_service::task_entry, "frame-board",
                                                       4096, this, 8, &task_, 0);
    return created == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

void board_service::task_entry(void* context) { static_cast<board_service*>(context)->run(); }

uint8_t board_service::crc8(const uint8_t* data, std::size_t size) {
    uint8_t crc = 0xFF;
    for (std::size_t index = 0; index < size; ++index) {
        crc ^= data[index];
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x80U) != 0U ? static_cast<uint8_t>((crc << 1U) ^ 0x31U)
                                      : static_cast<uint8_t>(crc << 1U);
        }
    }
    return crc;
}

esp_err_t board_service::initialize_i2c() {
    i2c_master_bus_config_t bus_config{};
    bus_config.i2c_port = board::kI2cPort;
    bus_config.sda_io_num = board::kI2cSda;
    bus_config.scl_io_num = board::kI2cScl;
    bus_config.clk_source = I2C_CLK_SRC_DEFAULT;
    bus_config.glitch_ignore_cnt = 7;
    bus_config.flags.enable_internal_pullup = true;
    esp_err_t result = i2c_new_master_bus(&bus_config, &bus_);
    if (result != ESP_OK) {
        return result;
    }

    const bool rtc_present = i2c_master_probe(bus_, board::kRtcAddress, 100) == ESP_OK;
    const bool sensor_present = i2c_master_probe(bus_, board::kSensorAddress, 100) == ESP_OK;
    status_.update([&](status_snapshot& value) {
        value.rtc_present = rtc_present;
        value.sensor_present = sensor_present;
    });

    if (rtc_present) {
        i2c_device_config_t rtc_config{};
        rtc_config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
        rtc_config.device_address = board::kRtcAddress;
        rtc_config.scl_speed_hz = 400000;
        const esp_err_t rtc_result = i2c_master_bus_add_device(bus_, &rtc_config, &rtc_);
        if (rtc_result != ESP_OK) {
            return rtc_result;
        }
    }
    if (!sensor_present) {
        return rtc_present ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    }

    i2c_device_config_t sensor_config{};
    sensor_config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    sensor_config.device_address = board::kSensorAddress;
    sensor_config.scl_speed_hz = 400000;
    return i2c_master_bus_add_device(bus_, &sensor_config, &sensor_);
}

esp_err_t board_service::send_sensor_sleep() {
    const uint8_t sleep[]{static_cast<uint8_t>(kSensorSleep >> 8U),
                          static_cast<uint8_t>(kSensorSleep)};
    return i2c_master_transmit(sensor_, sleep, sizeof(sleep), 100);
}

esp_err_t board_service::read_rtc() {
    if (rtc_ == nullptr) {
        return ESP_ERR_NOT_FOUND;
    }
    uint8_t registers[7]{};
    esp_err_t result = i2c_master_transmit(rtc_, &kRtcSecondsRegister, 1, 100);
    if (result == ESP_OK) {
        result = i2c_master_receive(rtc_, registers, sizeof(registers), 100);
    }
    if (result != ESP_OK) {
        status_.update([](status_snapshot& value) { value.rtc.valid = false; });
        return result;
    }

    const unsigned second = from_bcd(registers[0] & 0x7FU);
    const bool oscillator_stopped = (registers[0] & 0x80U) != 0U;
    const unsigned minute = from_bcd(registers[1] & 0x7FU);
    const unsigned hour = from_bcd(registers[2] & 0x3FU);
    const unsigned day = from_bcd(registers[3] & 0x3FU);
    const unsigned month = from_bcd(registers[5] & 0x1FU);
    const int64_t year = 2000 + from_bcd(registers[6]);

    const bool plausible = !oscillator_stopped && second < 60 && minute < 60 && hour < 24 &&
                           day >= 1 && day <= 31 && month >= 1 && month <= 12 && year <= 2099;
    const int64_t unix_seconds = days_from_civil(year, month, day) * 86400LL +
                                 static_cast<int64_t>(hour) * 3600LL +
                                 static_cast<int64_t>(minute) * 60LL + static_cast<int64_t>(second);
    const bool valid = plausible && unix_seconds > 0;
    status_.update([&](status_snapshot& value) {
        value.rtc = {.unix_seconds = valid ? unix_seconds : 0,
                     .valid = valid,
                     .read_at_us = esp_timer_get_time()};
    });
    return valid ? ESP_OK : ESP_ERR_INVALID_CRC;
}

esp_err_t board_service::sample_sensor() {
    const uint8_t wake[]{static_cast<uint8_t>(kSensorWake >> 8U),
                         static_cast<uint8_t>(kSensorWake)};
    esp_err_t result = i2c_master_transmit(sensor_, wake, sizeof(wake), 100);
    if (result != ESP_OK) {
        return result;
    }
    vTaskDelay(pdMS_TO_TICKS(1));

    const uint8_t measure[]{static_cast<uint8_t>(kSensorMeasure >> 8U),
                            static_cast<uint8_t>(kSensorMeasure)};
    result = i2c_master_transmit(sensor_, measure, sizeof(measure), 100);
    if (result != ESP_OK) {
        send_sensor_sleep();
        return result;
    }
    vTaskDelay(pdMS_TO_TICKS(20));

    uint8_t data[6]{};
    result = i2c_master_receive(sensor_, data, sizeof(data), 100);
    if (result == ESP_OK && (crc8(data, 2) != data[2] || crc8(data + 3, 2) != data[5])) {
        result = ESP_ERR_INVALID_CRC;
    }
    if (result == ESP_OK) {
        const uint16_t raw_temperature = static_cast<uint16_t>(data[0] << 8U) | data[1];
        const uint16_t raw_humidity = static_cast<uint16_t>(data[3] << 8U) | data[4];
        const int32_t temperature =
            -450 + static_cast<int32_t>((1750LL * raw_temperature) / 65536LL);
        const uint32_t humidity = static_cast<uint32_t>((1000ULL * raw_humidity) / 65536ULL);
        if (temperature < -400 || temperature > 1250 || humidity > 1000) {
            result = ESP_ERR_INVALID_RESPONSE;
        }
        if (result == ESP_OK) {
            status_.update([&](status_snapshot& value) {
                value.sensor = {
                    .temperature_tenths_celsius = static_cast<int16_t>(temperature),
                    .humidity_tenths_percent = static_cast<uint16_t>(humidity),
                    .valid = true,
                    .sampled_at_us = esp_timer_get_time(),
                };
            });
        }
    }
    send_sensor_sleep();
    return result;
}

void board_service::run() {
    bool last_pressed = gpio_get_level(board::kKey) == 0;
    bool stable_pressed = last_pressed;
    int64_t changed_at = esp_timer_get_time();
    int64_t pressed_at = stable_pressed ? changed_at : 0;
    int64_t next_sensor_at = 0;

    while (true) {
        const int64_t now = esp_timer_get_time();
        const bool pressed = gpio_get_level(board::kKey) == 0;
        if (pressed != last_pressed) {
            last_pressed = pressed;
            changed_at = now;
        }
        if (pressed != stable_pressed && now - changed_at >= 30000) {
            stable_pressed = pressed;
            if (pressed) {
                pressed_at = now;
            } else if (pressed_at != 0) {
                const int64_t held_us = now - pressed_at;
                status_.update([&](status_snapshot& value) {
                    value.key_pressed = false;
                    if (held_us >= kKeyPortalHoldUs) {
                        value.ap_mode_requested = true;
                    } else if (held_us >= kKeyLongPressUs) {
                        ++value.key_long_presses;
                    } else {
                        ++value.key_short_presses;
                    }
                });
                pressed_at = 0;
            }
            if (pressed) {
                status_.update([](status_snapshot& value) { value.key_pressed = true; });
            }
        }

        if (now >= next_sensor_at) {
            const esp_err_t sensor_result = sample_sensor();
            if (sensor_result != ESP_OK) {
                ESP_LOGW(kTag, "SHTC3 sample failed: %s", esp_err_to_name(sensor_result));
                status_.update([](status_snapshot& value) { value.sensor.valid = false; });
            }
            const esp_err_t rtc_result = read_rtc();
            if (rtc_result != ESP_OK) {
                ESP_LOGW(kTag, "PCF85063 read failed: %s", esp_err_to_name(rtc_result));
            }
            next_sensor_at = now + 10LL * 1000LL * 1000LL;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

} // namespace frame::m1

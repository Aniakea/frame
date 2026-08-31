#pragma once

#include <cstddef>
#include <cstdint>

#include "driver/gpio.h"
#include "driver/i2c_types.h"
#include "driver/spi_master.h"

namespace frame::m1::board {

inline constexpr char kProduct[] = "Waveshare ESP32-S3-RLCD-4.2";
inline constexpr char kModule[] = "ESP32-S3-WROOM-1-N16R8";
inline constexpr char kFirmwareVersion[] = "0.1.0-dev.1";
inline constexpr char kIdfTag[] = "v6.0.2";

inline constexpr std::size_t kFlashBytes = 16U * 1024U * 1024U;
inline constexpr std::size_t kPsramBytes = 8U * 1024U * 1024U;

inline constexpr int kDisplayWidth = 400;
inline constexpr int kDisplayHeight = 300;
inline constexpr std::size_t kDisplayBufferBytes =
    static_cast<std::size_t>(kDisplayWidth) * static_cast<std::size_t>(kDisplayHeight) / 8U;
inline constexpr spi_host_device_t kDisplayHost = SPI3_HOST;
inline constexpr int kDisplayClockHz = 10 * 1000 * 1000;
inline constexpr gpio_num_t kDisplaySck = GPIO_NUM_11;
inline constexpr gpio_num_t kDisplayMosi = GPIO_NUM_12;
inline constexpr gpio_num_t kDisplayDc = GPIO_NUM_5;
inline constexpr gpio_num_t kDisplayCs = GPIO_NUM_40;
inline constexpr gpio_num_t kDisplayReset = GPIO_NUM_41;
inline constexpr gpio_num_t kDisplayTe = GPIO_NUM_6;

inline constexpr i2c_port_num_t kI2cPort = I2C_NUM_0;
inline constexpr gpio_num_t kI2cSda = GPIO_NUM_13;
inline constexpr gpio_num_t kI2cScl = GPIO_NUM_14;
inline constexpr uint16_t kRtcAddress = 0x51;
inline constexpr uint16_t kSensorAddress = 0x70;

inline constexpr gpio_num_t kKey = GPIO_NUM_18;
// BOOT is a strapping pin; runtime config is input + pull-up read-only, never driven.
inline constexpr gpio_num_t kBoot = GPIO_NUM_0;

// Human-facing local time offset: China Standard Time (UTC+8), no daylight-saving transitions.
// Internal storage, JSONL, and machine `utc` fields stay strictly UTC.
inline constexpr int32_t kLocalUtcOffsetSeconds = 8 * 3600;

inline constexpr gpio_num_t kSdClock = GPIO_NUM_38;
inline constexpr gpio_num_t kSdCommand = GPIO_NUM_21;
inline constexpr gpio_num_t kSdData0 = GPIO_NUM_39;
inline constexpr char kSdMount[] = "/frame";

} // namespace frame::m1::board

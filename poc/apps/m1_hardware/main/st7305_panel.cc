#include "st7305_panel.hh"

#include <array>
#include <cstring>

#include "board_config.hh"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace frame::m1 {
namespace {

struct init_command {
    uint8_t command;
    std::array<uint8_t, 10> data;
    uint8_t length;
};

constexpr std::array kInitialization{
    init_command{0xD6, {0x17, 0x02}, 2},
    init_command{0xD1, {0x01}, 1},
    init_command{0xC0, {0x11, 0x04}, 2},
    init_command{0xC1, {0x69, 0x69, 0x69, 0x69}, 4},
    init_command{0xC2, {0x19, 0x19, 0x19, 0x19}, 4},
    init_command{0xC4, {0x4B, 0x4B, 0x4B, 0x4B}, 4},
    init_command{0xC5, {0x19, 0x19, 0x19, 0x19}, 4},
    init_command{0xD8, {0x80, 0xE9}, 2},
    init_command{0xB2, {0x02}, 1},
    init_command{0xB3, {0xE5, 0xF6, 0x05, 0x46, 0x77, 0x77, 0x77, 0x77, 0x76, 0x45}, 10},
    init_command{0xB4, {0x05, 0x46, 0x77, 0x77, 0x77, 0x77, 0x76, 0x45}, 8},
    init_command{0x62, {0x32, 0x03, 0x1F}, 3},
    init_command{0xB7, {0x13}, 1},
    init_command{0xB0, {0x64}, 1},
};

} // namespace

st7305_panel::~st7305_panel() {
    if (io_ != nullptr) {
        esp_lcd_panel_io_del(io_);
    }
    if (bus_initialized_) {
        spi_bus_free(board::kDisplayHost);
    }
    heap_caps_free(native_buffer_);
    if (transfer_complete_ != nullptr) {
        vSemaphoreDelete(transfer_complete_);
    }
}

esp_err_t st7305_panel::initialize() {
    spi_bus_config_t bus{};
    bus.miso_io_num = GPIO_NUM_NC;
    bus.mosi_io_num = board::kDisplayMosi;
    bus.sclk_io_num = board::kDisplaySck;
    bus.quadwp_io_num = GPIO_NUM_NC;
    bus.quadhd_io_num = GPIO_NUM_NC;
    bus.max_transfer_sz = static_cast<int>(board::kDisplayBufferBytes);

    esp_err_t result = spi_bus_initialize(board::kDisplayHost, &bus, SPI_DMA_CH_AUTO);
    if (result != ESP_OK) {
        return result;
    }
    bus_initialized_ = true;

    esp_lcd_panel_io_spi_config_t io_config{};
    io_config.dc_gpio_num = board::kDisplayDc;
    io_config.cs_gpio_num = board::kDisplayCs;
    io_config.pclk_hz = board::kDisplayClockHz;
    io_config.spi_mode = 0;
    io_config.trans_queue_depth = 4;
    io_config.lcd_cmd_bits = 8;
    io_config.lcd_param_bits = 8;
    result = esp_lcd_new_panel_io_spi(board::kDisplayHost, &io_config, &io_);
    if (result != ESP_OK) {
        return result;
    }
    native_buffer_ = static_cast<uint8_t*>(
        heap_caps_malloc(board::kDisplayBufferBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
    transfer_complete_ = xSemaphoreCreateBinary();
    if (native_buffer_ == nullptr || transfer_complete_ == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    const esp_lcd_panel_io_callbacks_t callbacks{
        .on_color_trans_done = &st7305_panel::transfer_done,
    };
    result = esp_lcd_panel_io_register_event_callbacks(io_, &callbacks, this);
    if (result != ESP_OK) {
        return result;
    }

    gpio_config_t reset_config{};
    reset_config.pin_bit_mask = UINT64_C(1) << board::kDisplayReset;
    reset_config.mode = GPIO_MODE_OUTPUT;
    reset_config.pull_up_en = GPIO_PULLUP_ENABLE;
    result = gpio_config(&reset_config);
    if (result != ESP_OK) {
        return result;
    }

    result = reset();
    if (result != ESP_OK) {
        return result;
    }
    for (const init_command& item : kInitialization) {
        result = command(item.command, item.data.data(), item.length);
        if (result != ESP_OK) {
            return result;
        }
    }
    result = command(0x11);
    if (result != ESP_OK) {
        return result;
    }
    vTaskDelay(pdMS_TO_TICKS(200));

    const uint8_t c9[]{0x00};
    const uint8_t memory_access[]{0x48};
    const uint8_t pixel_format[]{0x11};
    const uint8_t b9[]{0x20};
    const uint8_t b8[]{0x29};
    const uint8_t columns[]{0x12, 0x2A};
    const uint8_t rows[]{0x00, 0xC7};
    const uint8_t tearing[]{0x00};
    const uint8_t d0[]{0xFF};
    const std::array tail{
        init_command{0xC9, {c9[0]}, 1},
        init_command{0x36, {memory_access[0]}, 1},
        init_command{0x3A, {pixel_format[0]}, 1},
        init_command{0xB9, {b9[0]}, 1},
        init_command{0xB8, {b8[0]}, 1},
        init_command{0x21, {}, 0},
        init_command{0x2A, {columns[0], columns[1]}, 2},
        init_command{0x2B, {rows[0], rows[1]}, 2},
        init_command{0x35, {tearing[0]}, 1},
        init_command{0xD0, {d0[0]}, 1},
        init_command{0x38, {}, 0},
        init_command{0x29, {}, 0},
    };
    for (const init_command& item : tail) {
        result = command(item.command, item.data.data(), item.length);
        if (result != ESP_OK) {
            return result;
        }
    }
    return ESP_OK;
}

esp_err_t st7305_panel::draw_full(const uint8_t* row_major_i1, std::size_t size) {
    if (io_ == nullptr || row_major_i1 == nullptr || size != board::kDisplayBufferBytes) {
        return ESP_ERR_INVALID_ARG;
    }
    if (transfer_pending_) {
        return ESP_ERR_INVALID_STATE;
    }

    if (native_buffer_ == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    std::memset(native_buffer_, 0, board::kDisplayBufferBytes);

    constexpr int source_stride = board::kDisplayWidth / 8;
    constexpr int native_column_bytes = board::kDisplayHeight / 4;
    for (int y = 0; y < board::kDisplayHeight; ++y) {
        for (int x = 0; x < board::kDisplayWidth; ++x) {
            const bool white =
                (row_major_i1[y * source_stride + x / 8] & (UINT8_C(1) << (7 - x % 8))) != 0;
            const int inverted_y = board::kDisplayHeight - 1 - y;
            const std::size_t target =
                static_cast<std::size_t>((x / 2) * native_column_bytes + inverted_y / 4);
            const uint8_t bit = static_cast<uint8_t>(7 - ((inverted_y % 4) * 2 + (x % 2)));
            if (white) {
                native_buffer_[target] |= static_cast<uint8_t>(UINT8_C(1) << bit);
            } else {
                native_buffer_[target] &= static_cast<uint8_t>(~(UINT8_C(1) << bit));
            }
        }
    }

    const uint8_t columns[]{0x12, 0x2A};
    const uint8_t rows[]{0x00, 0xC7};
    esp_err_t result = command(0x2A, columns, sizeof(columns));
    if (result == ESP_OK) {
        result = command(0x2B, rows, sizeof(rows));
    }
    if (result == ESP_OK) {
        transfer_pending_ = true;
        result = esp_lcd_panel_io_tx_color(io_, 0x2C, native_buffer_, board::kDisplayBufferBytes);
        if (result != ESP_OK) {
            transfer_pending_ = false;
        }
    }
    if (result == ESP_OK && xSemaphoreTake(transfer_complete_, pdMS_TO_TICKS(2000)) != pdTRUE) {
        result = ESP_ERR_TIMEOUT;
    }
    return result;
}

bool st7305_panel::transfer_done(esp_lcd_panel_io_handle_t, esp_lcd_panel_io_event_data_t*,
                                 void* context) {
    auto* panel = static_cast<st7305_panel*>(context);
    panel->transfer_pending_ = false;
    BaseType_t task_woken = pdFALSE;
    xSemaphoreGiveFromISR(panel->transfer_complete_, &task_woken);
    return task_woken == pdTRUE;
}

esp_err_t st7305_panel::draw_pattern(test_pattern pattern) {
    auto* buffer =
        static_cast<uint8_t*>(heap_caps_malloc(board::kDisplayBufferBytes, MALLOC_CAP_SPIRAM));
    if (buffer == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    std::memset(buffer, pattern == test_pattern::all_black ? 0x00 : 0xFF,
                board::kDisplayBufferBytes);
    if (pattern == test_pattern::checker || pattern == test_pattern::border) {
        constexpr int stride = board::kDisplayWidth / 8;
        for (int y = 0; y < board::kDisplayHeight; ++y) {
            for (int x = 0; x < board::kDisplayWidth; ++x) {
                const bool ring = x == 0 || y == 0 || x == board::kDisplayWidth - 1 ||
                                  y == board::kDisplayHeight - 1;
                const bool black = pattern == test_pattern::border
                                       ? ring
                                       : ring || (((x / 20) + (y / 20)) % 2 == 0);
                if (black) {
                    buffer[y * stride + x / 8] &=
                        static_cast<uint8_t>(~(UINT8_C(1) << (7 - x % 8)));
                }
            }
        }
    }
    const esp_err_t result = draw_full(buffer, board::kDisplayBufferBytes);
    heap_caps_free(buffer);
    return result;
}

esp_err_t st7305_panel::reset() {
    esp_err_t result = gpio_set_level(board::kDisplayReset, 1);
    if (result != ESP_OK) {
        return result;
    }
    vTaskDelay(pdMS_TO_TICKS(50));
    result = gpio_set_level(board::kDisplayReset, 0);
    if (result != ESP_OK) {
        return result;
    }
    vTaskDelay(pdMS_TO_TICKS(20));
    result = gpio_set_level(board::kDisplayReset, 1);
    vTaskDelay(pdMS_TO_TICKS(50));
    return result;
}

esp_err_t st7305_panel::command(uint8_t value) { return command(value, nullptr, 0); }

esp_err_t st7305_panel::command(uint8_t value, const uint8_t* parameters, std::size_t count) {
    return esp_lcd_panel_io_tx_param(io_, value, parameters, count);
}

} // namespace frame::m1

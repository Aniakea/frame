#include "display_service.hh"

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <ctime>

#include "board_config.hh"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

namespace frame::m1 {
namespace {

constexpr char kTag[] = "frame-display";
constexpr int kDrawRows = 40;
constexpr std::size_t kDrawBytes =
    board::kDisplayWidth / 8U * static_cast<std::size_t>(kDrawRows) + 2U * sizeof(lv_color32_t);
constexpr int64_t kRenderIntervalUs = 10LL * 1000LL * 1000LL;
constexpr int64_t kFastCheckIntervalUs = 500LL * 1000LL;

uint32_t tick_milliseconds() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }

// Volatile-field digest; a change means the visible status text would change, so the
// 500ms fast path re-renders without waiting for the 10s heartbeat.
uint64_t volatile_signature(const status_snapshot& value) {
    uint64_t signature = UINT64_C(1469598103934665603);
    const auto fold = [&signature](uint64_t field) {
        signature = (signature ^ field) * UINT64_C(1099511628211);
    };
    fold(value.key_short_presses);
    fold(value.key_long_presses);
    fold(value.boot_short_presses);
    fold(value.boot_long_presses);
    fold(value.key_pressed ? 1U : 0U);
    fold(static_cast<uint64_t>(value.wifi));
    fold(static_cast<uint64_t>(value.storage));
    fold(value.tf_logging_ok ? 1U : 0U);
    fold(value.ap_active ? 1U : 0U);
    fold(value.ssid_configured ? 1U : 0U);
    fold(value.rtc.valid ? 1U : 0U);
    fold(value.rtc.valid ? static_cast<uint64_t>(value.rtc.unix_seconds / 60) : 0U);
    return signature;
}

// Human-facing RTC line: local UTC+8 rendering (China has no DST, fixed offset).
void format_rtc_time(const status_snapshot& value, char* output, std::size_t capacity) {
    output[0] = '\0';
    if (!value.rtc.valid) {
        return;
    }
    const time_t seconds =
        static_cast<time_t>(value.rtc.unix_seconds + board::kLocalUtcOffsetSeconds);
    struct tm local_time {};
    if (gmtime_r(&seconds, &local_time) != nullptr) {
        const unsigned year = static_cast<unsigned>(local_time.tm_year + 1900);
        std::snprintf(
            output, capacity, "%04u-%02u-%02u %02u:%02u:%02u UTC+8", year,
            static_cast<unsigned>(local_time.tm_mon + 1), static_cast<unsigned>(local_time.tm_mday),
            static_cast<unsigned>(local_time.tm_hour), static_cast<unsigned>(local_time.tm_min),
            static_cast<unsigned>(local_time.tm_sec));
    }
}

} // namespace

esp_err_t display_service::start() {
    esp_err_t result = panel_.initialize();
    const test_pattern patterns[]{test_pattern::all_black, test_pattern::all_white,
                                  test_pattern::checker, test_pattern::border};
    for (const test_pattern pattern : patterns) {
        if (result == ESP_OK) {
            result = panel_.draw_pattern(pattern);
        }
        vTaskDelay(pdMS_TO_TICKS(400));
    }
    status_.update([&](status_snapshot& value) { value.display_error = result; });
    if (result != ESP_OK) {
        return result;
    }

    const BaseType_t created = xTaskCreatePinnedToCore(&display_service::task_entry,
                                                       "frame-display", 8192, this, 12, &task_, 0);
    return created == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

void display_service::task_entry(void* context) { static_cast<display_service*>(context)->run(); }

void display_service::flush(lv_display_t* display, const lv_area_t* area, uint8_t* pixels) {
    auto* self = static_cast<display_service*>(lv_display_get_user_data(display));
    lv_draw_buf_t* active_buffer = lv_display_get_buf_active(display);
    pixels = static_cast<uint8_t*>(lv_draw_buf_goto_xy(active_buffer, 0, 0));
    const int width = lv_area_get_width(area);
    const int height = lv_area_get_height(area);
    const int source_stride = width / 8;
    constexpr int target_stride = board::kDisplayWidth / 8;

    for (int row = 0; row < height; ++row) {
        std::memcpy(self->framebuffer_ + (area->y1 + row) * target_stride + area->x1 / 8,
                    pixels + row * source_stride, static_cast<std::size_t>(source_stride));
    }
    if (lv_display_flush_is_last(display)) {
        const esp_err_t result =
            self->panel_.draw_full(self->framebuffer_, board::kDisplayBufferBytes);
        self->status_.update([&](status_snapshot& value) { value.display_error = result; });
    }
    lv_display_flush_ready(display);
}

esp_err_t display_service::initialize_lvgl() {
    framebuffer_ = static_cast<uint8_t*>(
        heap_caps_malloc(board::kDisplayBufferBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    draw_buffer_one_ = static_cast<uint8_t*>(
        heap_caps_calloc(1, kDrawBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    draw_buffer_two_ = static_cast<uint8_t*>(
        heap_caps_calloc(1, kDrawBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (framebuffer_ == nullptr || draw_buffer_one_ == nullptr || draw_buffer_two_ == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    std::memset(framebuffer_, 0xFF, board::kDisplayBufferBytes);

    lv_init();
    lv_tick_set_cb(&tick_milliseconds);
    display_ = lv_display_create(board::kDisplayWidth, board::kDisplayHeight);
    if (display_ == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    lv_display_set_user_data(display_, this);
    lv_display_set_color_format(display_, LV_COLOR_FORMAT_I1);
    lv_display_set_buffers(display_, draw_buffer_one_, draw_buffer_two_, kDrawBytes,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display_, &display_service::flush);

    lv_obj_t* screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_color(screen, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_pad_all(screen, 12, LV_PART_MAIN);
    lv_obj_set_flex_flow(screen, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(screen, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    lv_obj_t* title = lv_label_create(screen);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, LV_PART_MAIN);
    lv_label_set_text(title, "Frame System Status");
    health_label_ = lv_label_create(screen);

    lv_obj_t* columns = lv_obj_create(screen);
    lv_obj_remove_style_all(columns);
    lv_obj_set_width(columns, LV_PCT(100));
    lv_obj_set_flex_grow(columns, 1);
    lv_obj_set_flex_flow(columns, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(columns, 18, LV_PART_MAIN);

    lv_obj_t* left = lv_obj_create(columns);
    lv_obj_remove_style_all(left);
    lv_obj_set_flex_grow(left, 1);
    lv_obj_set_height(left, LV_PCT(100));
    lv_obj_set_flex_flow(left, LV_FLEX_FLOW_COLUMN);
    platform_label_ = lv_label_create(left);
    peripherals_label_ = lv_label_create(left);

    lv_obj_t* right = lv_obj_create(columns);
    lv_obj_remove_style_all(right);
    lv_obj_set_flex_grow(right, 1);
    lv_obj_set_height(right, LV_PCT(100));
    lv_obj_set_flex_flow(right, LV_FLEX_FLOW_COLUMN);
    network_label_ = lv_label_create(right);
    memory_label_ = lv_label_create(right);

    lv_obj_set_style_text_font(health_label_, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_font(platform_label_, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_font(peripherals_label_, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_font(network_label_, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_font(memory_label_, &lv_font_montserrat_16, LV_PART_MAIN);
    return ESP_OK;
}

void display_service::render_status(const status_snapshot& value) {
    const health_level health = hardware_status::overall_health(value);
    lv_label_set_text_fmt(health_label_, "Health: %s\nDevice: %s",
                          hardware_status::health_name(health), value.device_id);
    lv_label_set_text_fmt(platform_label_,
                          "Chip: S3 rev%" PRIu32 " / %" PRIu32 " cores\n"
                          "Flash: %u MiB [%s]\nPSRAM: %u MiB [%s]\nPart: [%s]",
                          value.chip_revision, value.cpu_cores,
                          static_cast<unsigned>(value.flash_bytes / (1024U * 1024U)),
                          value.flash_ok ? "OK" : "FAIL",
                          static_cast<unsigned>(value.psram_bytes / (1024U * 1024U)),
                          value.psram_ok ? "OK" : "FAIL", value.partitions_ok ? "OK" : "FAIL");
    char rtc_text[32]{};
    format_rtc_time(value, rtc_text, sizeof(rtc_text));
    // The ~179px status column cannot hold "YYYY-MM-DD HH:MM:SS UTC+8" on one line
    // (montserrat16: 210px); break after the date so both lines fit without clipping.
    char* time_break = std::strchr(rtc_text, ' ');
    if (time_break != nullptr) {
        *time_break = '\n';
    }
    lv_label_set_text_fmt(
        peripherals_label_,
        "Display: %s\nTF: %s / log:%s\nRTC: %s %s\nSHTC3: %s\n"
        "KEY:%" PRIu64 "/%" PRIu64 " BOOT:%" PRIu64 "/%" PRIu64 "\nRST:%" PRIu32,
        value.display_error == ESP_OK ? "OK" : "FAIL",
        hardware_status::sd_state_name(value.storage), value.tf_logging_ok ? "OK" : "WAIT",
        value.rtc_present ? "OK" : "MISS", rtc_text[0] == '\0' ? "-" : rtc_text,
        value.sensor_present ? "OK" : "MISS", value.key_short_presses, value.key_long_presses,
        value.boot_short_presses, value.boot_long_presses, value.reset_count);
    lv_label_set_text_fmt(
        network_label_, "WiFi: %s\nSSID: %s\nIPv4: %s\nCredentials: system_fs (dev)",
        value.ap_active ? "AP-SETUP 192.168.4.1" : hardware_status::wifi_state_name(value.wifi),
        value.ssid_configured ? "set" : "-", value.ip_address[0] == '\0' ? "-" : value.ip_address);
    lv_label_set_text_fmt(memory_label_,
                          "RAM free: %u KiB\n"
                          "RAM block: %u KiB\n"
                          "PS free: %u KiB\n"
                          "PS block: %u KiB",
                          static_cast<unsigned>(value.internal_free_bytes / 1024U),
                          static_cast<unsigned>(value.internal_largest_block / 1024U),
                          static_cast<unsigned>(value.psram_free_bytes / 1024U),
                          static_cast<unsigned>(value.psram_largest_block / 1024U));
}

void display_service::run() {
    const esp_err_t result = initialize_lvgl();
    status_.update([&](status_snapshot& value) { value.display_error = result; });
    if (result != ESP_OK) {
        ESP_LOGE(kTag, "LVGL initialization failed: %s", esp_err_to_name(result));
        vTaskDelete(nullptr);
        return;
    }

    int64_t next_render_at = 0;
    int64_t next_check_at = 0;
    uint64_t last_signature = 0;
    while (true) {
        const int64_t now = esp_timer_get_time();
        if (now >= next_render_at) {
            status_.refresh_memory_metrics();
            const status_snapshot value = status_.snapshot();
            render_status(value);
            last_signature = volatile_signature(value);
            next_render_at = now + kRenderIntervalUs;
            next_check_at = now + kFastCheckIntervalUs;
        } else if (now >= next_check_at) {
            const status_snapshot value = status_.snapshot();
            const uint64_t signature = volatile_signature(value);
            if (signature != last_signature) {
                render_status(value);
                last_signature = signature;
            }
            next_check_at = now + kFastCheckIntervalUs;
        }
        const uint32_t delay = std::clamp(lv_timer_handler(), UINT32_C(10), UINT32_C(1000));
        vTaskDelay(pdMS_TO_TICKS(delay));
    }
}

} // namespace frame::m1

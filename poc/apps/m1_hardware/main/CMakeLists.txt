idf_component_register(
    SRCS
        "app_main.cc"
        "board_service.cc"
        "console_service.cc"
        "display_service.cc"
        "hardware_status.cc"
        "sd_service.cc"
        "st7305_panel.cc"
        "status_log.cc"
        "wifi_provision.cc"
        "wifi_service.cc"
    INCLUDE_DIRS "."
    REQUIRES
        app_update
        cjson
        console
        esp_driver_gpio
        esp_driver_i2c
        esp_driver_sdmmc
        esp_driver_spi
        esp_event
        esp_http_server
        esp_hw_support
        esp_lcd
        esp_netif
        esp_psram
        esp_timer
        esp_wifi
        fatfs
        littlefs
        lvgl
        mbedtls
        nvs_flash
        spi_flash
)

target_compile_options(${COMPONENT_LIB} PRIVATE
    -std=c++26
    -Wall
    -Wextra
    -Wconversion
    -Werror=return-type
    -fno-exceptions
    -fno-rtti
    -fno-unwind-tables
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    FRAME_TOOLCHAIN_VERSION="gcc-${CMAKE_CXX_COMPILER_VERSION}"
)

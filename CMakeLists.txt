cmake_minimum_required(VERSION 3.25)

if(NOT DEFINED IDF_TARGET)
	set(IDF_TARGET "esp32s3" CACHE STRING "ESP-IDF target")
endif()

if(NOT DEFINED SDKCONFIG)
	set(SDKCONFIG "${CMAKE_BINARY_DIR}/sdkconfig")
endif()

if(NOT DEFINED SDKCONFIG_DEFAULTS)
	set(SDKCONFIG_DEFAULTS
		"${CMAKE_SOURCE_DIR}/sdkconfig.defaults;${CMAKE_SOURCE_DIR}/sdkconfig.defaults.dev")
endif()

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp_study)
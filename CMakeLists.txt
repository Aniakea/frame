cmake_minimum_required(VERSION 3.25)

set(FRAME_REQUIRED_IDF_TAG "v6.0.2" CACHE STRING
    "Exact ESP-IDF release tag required by the architecture")
set(FRAME_REQUIRED_IDF_COMMIT "7101770dc6db2667b3c477cc31365dd1acd6db4e" CACHE STRING
    "Exact ESP-IDF commit required by the architecture")
set(FRAME_REQUIRED_COMPILER_VERSION "15.2.0" CACHE STRING
    "Exact Xtensa GCC version required by the architecture")

include("${CMAKE_CURRENT_LIST_DIR}/cmake/FramePlatform.cmake")
frame_require_esp_idf("${FRAME_REQUIRED_IDF_TAG}")

if(NOT DEFINED SDKCONFIG)
	set(SDKCONFIG "${CMAKE_BINARY_DIR}/sdkconfig")
endif()

if(NOT DEFINED SDKCONFIG_DEFAULTS)
	set(SDKCONFIG_DEFAULTS
		"${CMAKE_SOURCE_DIR}/sdkconfig.defaults;${CMAKE_SOURCE_DIR}/sdkconfig.defaults.dev")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp_study)

frame_verify_platform("${FRAME_REQUIRED_IDF_COMMIT}" "${FRAME_REQUIRED_COMPILER_VERSION}")

# ESP-IDF v6.0.2 deliberately defaults components to GNU C++26. Frame code is
# compiled as strict C++26 by appending the stricter dialect after IDF options.
idf_build_set_property(CXX_COMPILE_OPTIONS "-std=c++26" APPEND)

if(CMAKE_CXX_STANDARD LESS 26)
	message(FATAL_ERROR "The selected toolchain does not provide C++26 mode.")
endif()

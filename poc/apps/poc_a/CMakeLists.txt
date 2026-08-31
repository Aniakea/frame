cmake_minimum_required(VERSION 3.25)

set(FRAME_REQUIRED_IDF_TAG "v6.0.2" CACHE STRING
    "Exact ESP-IDF release tag required by the architecture")
set(FRAME_REQUIRED_IDF_COMMIT "7101770dc6db2667b3c477cc31365dd1acd6db4e" CACHE STRING
    "Exact ESP-IDF commit required by the architecture")
set(FRAME_REQUIRED_COMPILER_VERSION "15.2.0" CACHE STRING
    "Exact Xtensa GCC version required by the architecture")

include("${CMAKE_CURRENT_LIST_DIR}/../../../cmake/FramePlatform.cmake")
frame_require_esp_idf("${FRAME_REQUIRED_IDF_TAG}")

set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/../../common/components")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(frame_poc_a)

frame_verify_platform("${FRAME_REQUIRED_IDF_COMMIT}" "${FRAME_REQUIRED_COMPILER_VERSION}")
idf_build_set_property(CXX_COMPILE_OPTIONS "-std=c++26" APPEND)

cmake_minimum_required(VERSION 3.25)

project(frame_poc_host LANGUAGES C CXX)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(ENABLE_TSAN "Enable ThreadSanitizer" OFF)
option(ENABLE_COVERAGE "Enable LLVM source-based coverage" OFF)
option(ENABLE_FUZZER "Build the Clang libFuzzer contract target" OFF)
option(ENABLE_CLANG_TIDY "Run clang-tidy on Frame host targets" OFF)

include(FetchContent)
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG 52eb8108c5bdec04579160ae17225d66034bd723
    GIT_SHALLOW FALSE
)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)
include(GoogleTest)

add_library(frame_poc_contract INTERFACE)
target_include_directories(frame_poc_contract INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/../common/components/frame_poc_runtime/include")
target_compile_options(frame_poc_contract INTERFACE
    $<$<COMPILE_LANGUAGE:C>:-Wall;-Wextra;-Werror;-Wpedantic>
    $<$<COMPILE_LANGUAGE:CXX>:-Wall;-Wextra;-Werror;-Wpedantic>)

if((ENABLE_SANITIZERS AND ENABLE_TSAN) OR
   (ENABLE_FUZZER AND (ENABLE_SANITIZERS OR ENABLE_TSAN OR ENABLE_COVERAGE)))
    message(FATAL_ERROR "Sanitizer, TSan, coverage, and fuzz modes require separate builds")
endif()

if(ENABLE_FUZZER)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "ENABLE_FUZZER requires Clang")
    endif()
    add_executable(frame_contract_fuzz tests/frame_contract_fuzz.cc)
    target_link_libraries(frame_contract_fuzz PRIVATE frame_poc_contract)
    target_compile_options(frame_contract_fuzz PRIVATE
        -fno-omit-frame-pointer -fsanitize=fuzzer,address,undefined)
    target_link_options(frame_contract_fuzz PRIVATE
        -fno-omit-frame-pointer -fsanitize=fuzzer,address,undefined)
endif()

if(ENABLE_SANITIZERS)
    target_compile_options(frame_poc_contract INTERFACE
        -fno-omit-frame-pointer -fsanitize=address,undefined)
    target_link_options(frame_poc_contract INTERFACE
        -fno-omit-frame-pointer -fsanitize=address,undefined)
endif()

if(ENABLE_TSAN)
    target_compile_options(frame_poc_contract INTERFACE
        -fno-omit-frame-pointer -fsanitize=thread)
    target_link_options(frame_poc_contract INTERFACE
        -fno-omit-frame-pointer -fsanitize=thread)
endif()

if(ENABLE_COVERAGE)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "ENABLE_COVERAGE requires Clang")
    endif()
    target_compile_options(frame_poc_contract INTERFACE
        -fprofile-instr-generate -fcoverage-mapping)
    target_link_options(frame_poc_contract INTERFACE
        -fprofile-instr-generate -fcoverage-mapping)
endif()

enable_testing()

add_executable(frame_abi_test tests/frame_abi_test.cc)
target_link_libraries(frame_abi_test PRIVATE frame_poc_contract GTest::gtest_main)
gtest_discover_tests(frame_abi_test)

add_executable(frame_abi_c_test tests/frame_abi_c_test.c)
target_link_libraries(frame_abi_c_test PRIVATE frame_poc_contract)
add_test(NAME frame_abi_c_test COMMAND frame_abi_c_test)

add_executable(frame_handle_test tests/frame_handle_test.cc)
target_link_libraries(frame_handle_test PRIVATE frame_poc_contract GTest::gtest_main)
gtest_discover_tests(frame_handle_test)

add_executable(lifecycle_state_test tests/lifecycle_state_test.cc)
target_link_libraries(lifecycle_state_test PRIVATE frame_poc_contract GTest::gtest_main)
gtest_discover_tests(lifecycle_state_test)

if(ENABLE_CLANG_TIDY)
    find_program(CLANG_TIDY_EXECUTABLE NAMES clang-tidy-18 clang-tidy REQUIRED)
    foreach(target frame_abi_test frame_handle_test lifecycle_state_test)
        set_property(TARGET ${target} PROPERTY CXX_CLANG_TIDY "${CLANG_TIDY_EXECUTABLE}")
    endforeach()
endif()

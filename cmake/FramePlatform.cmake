include_guard(GLOBAL)

macro(frame_require_esp_idf required_tag)
	if(NOT DEFINED ENV{IDF_PATH} OR "$ENV{IDF_PATH}" STREQUAL "")
		message(FATAL_ERROR
			"IDF_PATH is not set. Export the ESP-IDF ${required_tag} environment first.")
	endif()

	if(NOT EXISTS "$ENV{IDF_PATH}/tools/cmake/project.cmake")
		message(FATAL_ERROR
			"IDF_PATH does not point to an ESP-IDF checkout: $ENV{IDF_PATH}")
	endif()

	execute_process(
		COMMAND git -C "$ENV{IDF_PATH}" describe --tags --exact-match
		RESULT_VARIABLE FRAME_IDF_TAG_RESULT
		OUTPUT_VARIABLE FRAME_IDF_TAG
		ERROR_QUIET
		OUTPUT_STRIP_TRAILING_WHITESPACE)

	if(NOT FRAME_IDF_TAG_RESULT EQUAL 0 OR
	   NOT "${FRAME_IDF_TAG}" STREQUAL "${required_tag}")
		message(FATAL_ERROR
			"ESP-IDF must be checked out at exact tag ${required_tag}; detected '${FRAME_IDF_TAG}'.")
	endif()

	if(NOT DEFINED IDF_TARGET)
		set(IDF_TARGET "esp32s3" CACHE STRING "ESP-IDF target")
	endif()

	if(NOT IDF_TARGET STREQUAL "esp32s3")
		message(FATAL_ERROR "This project only supports IDF_TARGET=esp32s3.")
	endif()

	set(CMAKE_CXX_STANDARD 26)
	set(CMAKE_CXX_STANDARD_REQUIRED ON)
	set(CMAKE_CXX_EXTENSIONS OFF)
endmacro()

function(frame_verify_platform required_commit required_compiler_version)
	if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
		message(FATAL_ERROR
			"Frame requires the ESP-IDF GNU Xtensa compiler; detected ${CMAKE_CXX_COMPILER_ID}.")
	endif()

	if(NOT CMAKE_CXX_COMPILER_VERSION VERSION_EQUAL required_compiler_version)
		message(FATAL_ERROR
			"Frame requires Xtensa GCC ${required_compiler_version}; detected ${CMAKE_CXX_COMPILER_VERSION}.")
	endif()

	execute_process(
		COMMAND git -C "$ENV{IDF_PATH}" rev-parse HEAD
		RESULT_VARIABLE commit_result
		OUTPUT_VARIABLE detected_commit
		ERROR_QUIET
		OUTPUT_STRIP_TRAILING_WHITESPACE)
	if(NOT commit_result EQUAL 0 OR NOT detected_commit STREQUAL required_commit)
		message(FATAL_ERROR
			"ESP-IDF commit must be ${required_commit}; detected '${detected_commit}'.")
	endif()

	execute_process(
		COMMAND git -C "$ENV{IDF_PATH}" status --short
		RESULT_VARIABLE status_result
		OUTPUT_VARIABLE idf_changes
		ERROR_QUIET
		OUTPUT_STRIP_TRAILING_WHITESPACE)
	if(NOT status_result EQUAL 0 OR NOT idf_changes STREQUAL "")
		message(FATAL_ERROR "The ESP-IDF checkout must be clean for a reproducible Frame build.")
	endif()

	execute_process(
		COMMAND "${CMAKE_CXX_COMPILER}" -dumpmachine
		RESULT_VARIABLE machine_result
		OUTPUT_VARIABLE compiler_machine
		ERROR_QUIET
		OUTPUT_STRIP_TRAILING_WHITESPACE)
	if(NOT machine_result EQUAL 0 OR NOT compiler_machine STREQUAL "xtensa-esp-elf")
		message(FATAL_ERROR
			"Frame requires compiler target xtensa-esp-elf; detected '${compiler_machine}'.")
	endif()

	message(STATUS
		"Frame platform: IDF ${required_commit}, ${compiler_machine} GCC ${CMAKE_CXX_COMPILER_VERSION}, ${IDF_TARGET}")
endfunction()

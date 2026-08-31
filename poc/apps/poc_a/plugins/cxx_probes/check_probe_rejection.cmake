# Expected-failure wrapper around the positive-pipeline gates (task T6).
#
# Invoked as: cmake -D PROBE_NAME=<n> -D ELF=<file> -D READELF=<tool>
#                  -D NM=<tool> -D EXPECT_ENTRY=<sym>
#                  -D "NEEDLES=<substr;...>"
#                  -P cxx_probes/check_probe_rejection.cmake
#
# Runs the REAL gate script (check_plugin_elf.cmake, the same one that
# protects every positive poca_plugin build) against a C++ feature probe
# ELF and asserts it FAILS with the expected reason. A probe that the
# gates ACCEPT is a matrix failure (a forbidden feature slipped through),
# so success of the inner gate is fatal here; the actual rejection text is
# echoed verbatim so the evidence log carries the real gate output, not a
# summary of it.

execute_process(
    COMMAND "${CMAKE_COMMAND}"
        -D "PLUGIN_NAME=${PROBE_NAME}"
        -D "ELF=${ELF}"
        -D "READELF=${READELF}"
        -D "NM=${NM}"
        -D "EXPECT_ENTRY=${EXPECT_ENTRY}"
        -D "EXPECT_UNDEFINED="
        -P "${CHECK_SCRIPT}"
    OUTPUT_VARIABLE gate_out
    RESULT_VARIABLE gate_res
    ERROR_VARIABLE gate_err)

if(gate_res EQUAL 0)
    message(FATAL_ERROR
        "[cxx-probe] ${PROBE_NAME}: NOT REJECTED by the pipeline gates - "
        "the gates accepted a forbidden C++ feature (gate output: ${gate_out})")
endif()

set(gate_text "${gate_err}\n${gate_out}")
set(missing "")
foreach(needle IN LISTS NEEDLES)
    string(FIND "${gate_text}" "${needle}" needle_idx)
    if(needle_idx EQUAL -1)
        list(APPEND missing "${needle}")
    endif()
endforeach()
if(missing)
    message(FATAL_ERROR
        "[cxx-probe] ${PROBE_NAME}: gates failed (rc=${gate_res}) but the "
        "rejection reason is unexpected; missing [${missing}] in: ${gate_text}")
endif()

message(STATUS "[cxx-probe] ${PROBE_NAME}: pipeline gates REJECTED the probe as required "
    "(exit ${gate_res}); verbatim gate output follows")
string(REPLACE "\n" ";" gate_lines "${gate_err}")
foreach(line IN LISTS gate_lines)
    if(NOT line STREQUAL "")
        message(STATUS "[cxx-probe]   ${PROBE_NAME}: | ${line}")
    endif()
endforeach()

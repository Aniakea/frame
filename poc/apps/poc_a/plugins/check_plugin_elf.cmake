# Static gates on a built plugin ELF (task T4; feeds the T5 allowlist work).
#
# Invoked as: cmake -D PLUGIN_NAME=<n> -D ELF=<file> -D READELF=<tool>
#                  -D NM=<tool> -D EXPECT_ENTRY=<sym>
#                  -D "EXPECT_UNDEFINED=<sym;...>" -P check_plugin_elf.cmake
#
# Gate 1 (relocation allowlist): every dynamic relocation emitted into the
#   final image must be one of the types the vendored elf_loader implements
#   on xtensa (esp_elf_xtensa.c): R_XTENSA_NONE/RELATIVE/RTLD/GLOB_DAT/
#   JMP_SLOT. Anything else is a pipeline bug (fix the link flags), not a
#   loader configuration problem.
# Gate 2 (closed-world imports): every undefined symbol must appear in
#   EXPECT_UNDEFINED, the plugin's declared import list (T5). Device-side
#   enforcement is separate: only symbols registered at runtime through
#   esp_elf_register_symbol() actually resolve, anything else fails the
#   load with -ENOSYS before execute.
# Gate 3 (shape): ELF32, little-endian, Xtensa machine, ET_DYN, non-zero
#   entry, and the exported query entry present as a defined GLOBAL FUNC.

set(allowed_types R_XTENSA_NONE R_XTENSA_RELATIVE R_XTENSA_RTLD R_XTENSA_GLOB_DAT R_XTENSA_JMP_SLOT)

execute_process(
    COMMAND "${READELF}" -rW "${ELF}"
    OUTPUT_VARIABLE reloc_out
    RESULT_VARIABLE reloc_res
    ERROR_VARIABLE reloc_err)
if(NOT reloc_res EQUAL 0)
    message(FATAL_ERROR "[poca-plugin] ${PLUGIN_NAME}: readelf -r failed: ${reloc_err}")
endif()

string(REPLACE "\n" ";" reloc_lines "${reloc_out}")
set(type_names "")
foreach(line IN LISTS reloc_lines)
    string(REGEX MATCH "R_XTENSA_[A-Z0-9_]+" reloc_type "${line}")
    if(reloc_type)
        list(APPEND type_names "${reloc_type}")
    endif()
endforeach()

set(remaining "${type_names}")
set(offenders "")
set(summary "")
foreach(type IN LISTS allowed_types)
    set(matches "${remaining}")
    list(FILTER matches INCLUDE REGEX "^${type}$")
    list(LENGTH matches count)
    list(APPEND summary "${type}=${count}")
    list(FILTER remaining EXCLUDE REGEX "^${type}$")
endforeach()
if(remaining)
    list(REMOVE_DUPLICATES remaining)
    set(offenders "${remaining}")
endif()

if(offenders)
    message(FATAL_ERROR
        "[poca-plugin] ${PLUGIN_NAME}: relocation allowlist violated (${offenders}); "
        "fix the plugin link flags - the vendored loader rejects these on target")
endif()

execute_process(
    COMMAND "${NM}" -u "${ELF}"
    OUTPUT_VARIABLE undef_out
    RESULT_VARIABLE undef_res
    ERROR_VARIABLE undef_err)
if(NOT undef_res EQUAL 0)
    message(FATAL_ERROR "[poca-plugin] ${PLUGIN_NAME}: nm -u failed: ${undef_err}")
endif()
string(STRIP "${undef_out}" undef_out)
set(unexpected "${undef_out}")
if(unexpected)
    string(REPLACE "\n" ";" undef_symbols "${undef_out}")
    set(unexpected "")
    foreach(sym IN LISTS undef_symbols)
        string(STRIP "${sym}" sym)
        string(REGEX REPLACE "^U[ ]+" "" sym "${sym}")
        if(NOT sym STREQUAL "")
            list(FIND EXPECT_UNDEFINED "${sym}" idx)
            if(idx EQUAL -1)
                list(APPEND unexpected "${sym}")
            endif()
        endif()
    endforeach()
endif()
if(unexpected)
    message(FATAL_ERROR "[poca-plugin] ${PLUGIN_NAME}: import gate failed, undeclared undefined symbols: ${unexpected} (declared: ${EXPECT_UNDEFINED})")
endif()
list(LENGTH EXPECT_UNDEFINED declared_count)
message(STATUS "[poca-plugin] ${PLUGIN_NAME}: declared imports=${declared_count} undefined-undeclared=0")

execute_process(
    COMMAND "${READELF}" -hW "${ELF}"
    OUTPUT_VARIABLE hdr_out
    RESULT_VARIABLE hdr_res
    ERROR_VARIABLE hdr_err)
if(NOT hdr_res EQUAL 0)
    message(FATAL_ERROR "[poca-plugin] ${PLUGIN_NAME}: readelf -h failed: ${hdr_err}")
endif()
if(NOT hdr_out MATCHES "Class:.+ELF32")
    message(FATAL_ERROR "[poca-plugin] ${PLUGIN_NAME}: expected ELF32 image")
endif()
if(NOT hdr_out MATCHES "Data:.+2.s complement, little endian")
    message(FATAL_ERROR "[poca-plugin] ${PLUGIN_NAME}: expected little-endian image")
endif()
if(NOT hdr_out MATCHES "Machine:.+Xtensa")
    message(FATAL_ERROR "[poca-plugin] ${PLUGIN_NAME}: expected Xtensa machine")
endif()
if(NOT hdr_out MATCHES "Type:.+DYN")
    message(FATAL_ERROR "[poca-plugin] ${PLUGIN_NAME}: expected ET_DYN shared image")
endif()
if(hdr_out MATCHES "Entry point address:[ \\t]+0x0*[1-9a-fA-F]")
    # non-zero entry address
else()
    message(FATAL_ERROR "[poca-plugin] ${PLUGIN_NAME}: expected non-zero entry address")
endif()

execute_process(
    COMMAND "${READELF}" -sW "${ELF}"
    OUTPUT_VARIABLE sym_out
    RESULT_VARIABLE sym_res
    ERROR_VARIABLE sym_err)
if(NOT sym_res EQUAL 0)
    message(FATAL_ERROR "[poca-plugin] ${PLUGIN_NAME}: readelf -s failed: ${sym_err}")
endif()
string(REGEX MATCH "\n[ ]*[0-9]+:[^\n]*FUNC[ \t]+GLOBAL[ \t]+DEFAULT[^\n]*[ \t]${EXPECT_ENTRY}([ \t\r\n]|$)" entry_line "${sym_out}")
if(NOT entry_line)
    message(FATAL_ERROR "[poca-plugin] ${PLUGIN_NAME}: exported entry ${EXPECT_ENTRY} missing (GLOBAL FUNC DEFAULT)")
endif()

string(REPLACE ";" " " summary_line "${summary}")
message(STATUS "[poca-plugin] ${PLUGIN_NAME}: gates OK relocs(${summary_line}) undefined=0 entry=${EXPECT_ENTRY}")

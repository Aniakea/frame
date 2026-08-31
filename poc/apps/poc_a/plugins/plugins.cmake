# poca_plugin(<name> SOURCE <file.c> VERSION <semver> ENTRY <symbol> MANIFEST <json>
#             [IMPORTS <sym>...])
#
# PoC-A plugin build pipeline (task T4 of poc-a-dynamic-elf). Produces, under
# ${CMAKE_BINARY_DIR}/plugins/:
#   <name>.o           object compiled with the firmware build's xtensa gcc
#   <name>/<name>.elf  plugin ELF loadable by the vendored elf_loader
#   <name>.mpb         signed MPB container built by tools/frame_tools
#                      manifest_builder.py (frame-manifest-builder)
#   generated/poca_<name>_meta.h   size + sha256 of the mpb, compiled into the
#                      firmware so the device can detect a stale embed
#
# Flag provenance: the compile/link/strip flag sets mirror the vendored
# component poc/apps/poc_a/components/elf_loader/elf_loader.cmake
# project_elf()/project_so() macros (esp-iot-solution @6526c5b1, ADR-0002
# coupling). Deviations, each deliberate and the only ones:
#   - the -Wl,--strip-* link trio is dropped and stripping is deferred to the
#     strip step, because the firmware finds the query entry through the
#     symbol table (--keep-symbol=<entry>) instead of assuming the ELF header
#     entry point;
#   - --keep-symbol=${ENTRY} is added to --strip-unneeded so exactly one
#     exported symbol survives in .symtab;
#   - -Wl,--build-id=none (plus the matching strip removal) keeps note
#     sections out of the image;
#   - the compile adds -std=c11 -Os -ffreestanding -fno-builtin because the
#     plugin is a single freestanding C file with no newlib/libgcc references
#     (the upstream macros compile inside a full ESP-IDF component context).
#
# IMPORTS (T5): declares the plugin's host imports (the allowlisted
# undefined symbols that gate 2 accepts). This is a BUILD-side intent
# declaration only; the device still refuses to load the plugin unless the
# same names are registered at runtime through esp_elf_register_symbol().
#
# The pipeline additionally enforces what the vendored loader expects but does
# not itself guarantee (check_plugin_elf.cmake, feeds the T5 allowlist work):
#   1. every emitted dynamic relocation type is inside
#      {R_XTENSA_RELATIVE, R_XTENSA_RTLD, R_XTENSA_GLOB_DAT, R_XTENSA_JMP_SLOT}
#      (plus R_XTENSA_NONE);
#   2. undefined symbols are exactly the declared IMPORTS set;
#   3. ELF32 / little-endian / Xtensa / ET_DYN with a live <ENTRY> symbol.

# Capture the directory of this file at include time; CMAKE_CURRENT_LIST_DIR
# inside the functions below would otherwise resolve to the caller's
# directory (main/).
set(POCA_PLUGINS_DIR "${CMAKE_CURRENT_LIST_DIR}")
set(POCA_REPO_ROOT "${POCA_PLUGINS_DIR}/../../../..")

function(poca_plugin name)
    cmake_parse_arguments(PLUGIN "" "SOURCE;VERSION;ENTRY;MANIFEST" "IMPORTS" ${ARGN})
    if(NOT PLUGIN_SOURCE OR NOT PLUGIN_VERSION OR NOT PLUGIN_ENTRY OR NOT PLUGIN_MANIFEST)
        message(FATAL_ERROR "poca_plugin(${name}): SOURCE, VERSION, ENTRY and MANIFEST are required")
    endif()
    if(NOT PLUGIN_IMPORTS)
        set(PLUGIN_IMPORTS "")
    endif()

    set(plugins_dir "${POCA_PLUGINS_DIR}")
    set(out_dir "${CMAKE_BINARY_DIR}/plugins")
    set(gen_dir "${out_dir}/generated")
    file(MAKE_DIRECTORY "${out_dir}/${name}" "${gen_dir}")

    set(obj "${out_dir}/${name}/${name}.o")
    set(elf_raw "${out_dir}/${name}/${name}.raw.elf")
    set(elf "${out_dir}/${name}/${name}.elf")
    set(mpb "${out_dir}/${name}.mpb")
    set(stamp "${out_dir}/${name}/gates.stamp")
    set(meta_header "${gen_dir}/poca_${name}_meta.h")

    # Tool siblings of the firmware build's xtensa gcc, same discovery trick
    # as elf_loader.cmake (string(REPLACE -gcc -strip ...)).
    string(REPLACE "-gcc" "-readelf" plugin_readelf "${CMAKE_C_COMPILER}")
    string(REPLACE "-gcc" "-strip" plugin_strip "${CMAKE_C_COMPILER}")
    string(REPLACE "-gcc" "-nm" plugin_nm "${CMAKE_C_COMPILER}")

    set(plugin_compile_flags -c -std=c11 -Os -g0
        -fPIC -fvisibility=hidden
        -fdata-sections -ffunction-sections
        -ffreestanding -fno-builtin
        -Wall -Wextra -Werror)

    set(plugin_link_flags -shared -fPIC -static-libgcc
        -nostdlib -nostartfiles
        -fdata-sections -ffunction-sections
        -Wl,--gc-sections
        -fvisibility=hidden
        -Wl,--allow-shlib-undefined
        -Wl,--build-id=none
        -e ${PLUGIN_ENTRY})

    set(plugin_strip_flags --strip-unneeded
        --keep-symbol=${PLUGIN_ENTRY}
        --remove-section=.comment
        --remove-section=.got.loc
        --remove-section=.dynamic
        --remove-section=.note.gnu.build-id)
    if(CONFIG_IDF_TARGET_ARCH_XTENSA)
        list(APPEND plugin_strip_flags
            --remove-section=.xt.lit
            --remove-section=.xt.prop
            --remove-section=.xtensa.info)
    endif()

    add_custom_command(
        OUTPUT "${obj}"
        COMMAND "${CMAKE_C_COMPILER}" ${plugin_compile_flags}
                -I "${plugins_dir}" -c "${PLUGIN_SOURCE}" -o "${obj}"
        DEPENDS "${PLUGIN_SOURCE}" "${plugins_dir}/plugin_abi.h"
        COMMENT "[poca-plugin] compile ${name}.o"
        VERBATIM)

    add_custom_command(
        OUTPUT "${elf_raw}"
        COMMAND "${CMAKE_C_COMPILER}" ${plugin_link_flags} "${obj}" -o "${elf_raw}"
        DEPENDS "${obj}"
        COMMENT "[poca-plugin] link ${name}.elf"
        VERBATIM)

    add_custom_command(
        OUTPUT "${elf}"
        COMMAND "${plugin_strip}" ${plugin_strip_flags} "${elf_raw}" -o "${elf}"
        DEPENDS "${elf_raw}"
        COMMENT "[poca-plugin] strip ${name}.elf (keep ${PLUGIN_ENTRY})"
        VERBATIM)

    add_custom_command(
        OUTPUT "${stamp}"
        COMMAND "${CMAKE_COMMAND}"
            -D "PLUGIN_NAME=${name}"
            -D "ELF=${elf}"
            -D "READELF=${plugin_readelf}"
            -D "NM=${plugin_nm}"
            -D "EXPECT_ENTRY=${PLUGIN_ENTRY}"
            -D "EXPECT_UNDEFINED=${PLUGIN_IMPORTS}"
            -P "${plugins_dir}/check_plugin_elf.cmake"
        COMMAND "${CMAKE_COMMAND}" -E touch "${stamp}"
        DEPENDS "${elf}"
        COMMENT "[poca-plugin] allowlist/import gates on ${name}.elf"
        VERBATIM)

    set(repo_root "${POCA_REPO_ROOT}")
    set(signing_key "${repo_root}/tools/frame_tools/testdata/keys/poc_a_test_signing_key.pem")
    if(NOT EXISTS "${signing_key}")
        message(FATAL_ERROR "poca_plugin(${name}): test signing key missing: ${signing_key}")
    endif()
    if(Python_EXECUTABLE)
        set(plugin_python "${Python_EXECUTABLE}")
    else()
        set(plugin_python python3)
    endif()

    # frame-manifest-builder entry point, invoked through the module so the
    # step also runs inside the plain ESP-IDF CI container (no uv there).
    add_custom_command(
        OUTPUT "${mpb}"
        BYPRODUCTS "${meta_header}"
        COMMAND "${CMAKE_COMMAND}" -E env "PYTHONPATH=${repo_root}/tools"
            "${plugin_python}" -m frame_tools.manifest_builder build
            --elf "${elf}" --name "${name}" --version "${PLUGIN_VERSION}"
            --manifest-json "${PLUGIN_MANIFEST}"
            --key "${signing_key}" --epoch 1 --output "${mpb}"
        COMMAND "${CMAKE_COMMAND}"
            -D "PLUGIN_NAME=${name}" -D "PLUGIN_VERSION=${PLUGIN_VERSION}"
            -D "PLUGIN_ENTRY=${PLUGIN_ENTRY}"
            -D "MPB=${mpb}" -D "OUT=${meta_header}"
            -P "${plugins_dir}/gen_meta_header.cmake"
        DEPENDS "${stamp}" "${PLUGIN_MANIFEST}" "${signing_key}"
        COMMENT "[poca-plugin] package+sign ${name}.mpb"
        VERBATIM)

    add_custom_target("poca_plugin_${name}" DEPENDS "${mpb}")
    message(STATUS "[poca-plugin] registered ${name} (entry ${PLUGIN_ENTRY}, version ${PLUGIN_VERSION})")
endfunction()

# poca_negative_corpus(BASELINE_ELF <elf> ...)
#
# T5 negative corpus: runs build_negative_corpus.py against the gated
# baseline.elf to fabricate the handcrafted negative containers (out-of-
# allowlist relocation types, malformed ELF identity, inflated PT_LOAD
# budget). These .mpb files deliberately bypass the check_plugin_elf.cmake
# gates - the gates protect positive builds, negatives are handcrafted -
# and are signed with the same TEST-only key so device-side mpb
# verification still passes and the rejection must come from the loader
# (patches p1/p2) or the harness program-header admission check.
function(poca_negative_corpus)
    cmake_parse_arguments(CORPUS "" "BASELINE_ELF" "" ${ARGN})
    if(NOT CORPUS_BASELINE_ELF)
        message(FATAL_ERROR "poca_negative_corpus: BASELINE_ELF is required")
    endif()

    set(out_dir "${CMAKE_BINARY_DIR}/plugins")
    set(gen_dir "${out_dir}/generated")
    set(corpus_names neg_r32 neg_s0op neg_phspan neg_phbe neg_ph64 neg_phmach)
    set(corpus_outputs "")
    foreach(name IN LISTS corpus_names)
        list(APPEND corpus_outputs "${out_dir}/${name}.mpb" "${gen_dir}/poca_${name}_meta.h")
    endforeach()
    set(stamp "${out_dir}/negative/corpus.stamp")

    set(signing_key "${POCA_REPO_ROOT}/tools/frame_tools/testdata/keys/poc_a_test_signing_key.pem")
    if(Python_EXECUTABLE)
        set(corpus_python "${Python_EXECUTABLE}")
    else()
        set(corpus_python python3)
    endif()

    add_custom_command(
        OUTPUT "${stamp}"
        BYPRODUCTS ${corpus_outputs}
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${out_dir}/negative"
        COMMAND "${corpus_python}" "${POCA_PLUGINS_DIR}/negative/build_negative_corpus.py"
            --baseline-elf "${CORPUS_BASELINE_ELF}"
            --key "${signing_key}"
            --out-dir "${out_dir}"
        COMMAND "${CMAKE_COMMAND}" -E touch "${stamp}"
        DEPENDS "${CORPUS_BASELINE_ELF}" "${POCA_PLUGINS_DIR}/negative/build_negative_corpus.py" "${signing_key}"
        COMMENT "[poca-corpus] handcrafted negative containers (gate-bypass by design)"
        VERBATIM)
    add_custom_target(poca_negative_corpus DEPENDS "${stamp}")
endfunction()

# Generate the embedded TEST public key header (65-byte uncompressed point
# 0x04||X||Y plus key_id) from the committed PEM. Runs at configure time with
# any stdlib python; fails the configure when the derived key_id does not
# match EXPECT_KEY_ID (guards against key material drift).
function(poca_pubkey_header pem out_header expect_key_id)
    set(gen_py "${POCA_PLUGINS_DIR}/gen_pubkey_header.py")
    if(Python_EXECUTABLE)
        set(plugin_python "${Python_EXECUTABLE}")
    else()
        set(plugin_python python3)
    endif()
    execute_process(
        COMMAND "${plugin_python}" "${gen_py}" "${pem}" "${out_header}"
                --expect-key-id "${expect_key_id}"
        RESULT_VARIABLE res
        OUTPUT_VARIABLE stdout
        ERROR_VARIABLE stderr)
    if(NOT res EQUAL 0)
        message(FATAL_ERROR "poca_pubkey_header: ${gen_py} failed (${res}): ${stdout} ${stderr}")
    endif()
    string(STRIP "${stdout}" stdout)
    message(STATUS "[poca-plugin] pubkey ${stdout}")
endfunction()

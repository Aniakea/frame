idf_component_register(
    SRCS
        "abi_smoke.cc"
        "poc_a_main.cc"
        "poca_console.cc"
    INCLUDE_DIRS "."
    PRIV_REQUIRES
        console
        elf_loader
        esp_hw_support
        frame_poc_runtime
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

idf_component_register(
    SRCS "poc_a_main.cc"
    PRIV_REQUIRES frame_poc_runtime unity)

target_compile_options(${COMPONENT_LIB} PRIVATE -std=c++26)

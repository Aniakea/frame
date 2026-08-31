idf_component_register(SRCS "main.cc"
                       INCLUDE_DIRS ".")

target_compile_options(${COMPONENT_LIB} PRIVATE -std=c++26)

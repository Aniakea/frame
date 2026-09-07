idf_component_register(
    SRCS
        "src/mpb_mbedtls_glue.c"
        "src/mpb_parser.c"
    INCLUDE_DIRS "include"
    REQUIRES frame_poc_runtime
    PRIV_REQUIRES mbedtls
)

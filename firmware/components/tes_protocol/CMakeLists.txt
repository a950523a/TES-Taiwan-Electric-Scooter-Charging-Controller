idf_component_register(
    SRCS
        "tes_codec.c"
        "tes_sm.c"
    INCLUDE_DIRS
        "include"
    # No REQUIRES: pure C99, zero platform/OS dependencies
)

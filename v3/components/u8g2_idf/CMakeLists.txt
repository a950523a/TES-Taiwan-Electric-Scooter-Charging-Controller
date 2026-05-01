# u8g2_idf — ESP-IDF wrapper for the olikraus/u8g2 git submodule.
# The u8g2 source lives in ../u8g2/csrc (git submodule, not committed to this repo).
# Run `git submodule update --init` to populate it before building.

set(U8G2_CSRC "${CMAKE_CURRENT_SOURCE_DIR}/../u8g2/csrc")

file(GLOB U8G2_SRCS "${U8G2_CSRC}/*.c")

idf_component_register(
    SRCS        ${U8G2_SRCS}
    INCLUDE_DIRS "${U8G2_CSRC}"
)

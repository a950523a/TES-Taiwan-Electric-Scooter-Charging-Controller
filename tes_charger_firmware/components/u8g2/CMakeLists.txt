# components/u8g2/CMakeLists.txt

# 收集 csrc 資料夾下的所有 C 原始檔
file(GLOB U8G2_SRCS "csrc/*.c")

idf_component_register(
    SRCS ${U8G2_SRCS}
    INCLUDE_DIRS "csrc"
    REQUIRES driver
)

# 關閉一些編譯警告，讓它安靜一點
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-unused-function -Wno-unused-variable)
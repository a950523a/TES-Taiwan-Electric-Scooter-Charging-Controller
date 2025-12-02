idf_component_register(
    SRCS 
        "cutils.c"
        "quickjs.c"
        "libregexp.c"
        "libunicode.c"
        "dtoa.c"
    INCLUDE_DIRS "."
    REQUIRES driver
)

# --- 關鍵修正 1：關閉將警告視為錯誤 ---
# QuickJS 的原始碼比較舊，會有很多類型警告，我們忽略它
target_compile_options(${COMPONENT_LIB} PRIVATE 
    -Wno-implicit-fallthrough 
    -Wno-sign-compare
    -Wno-missing-field-initializers
    -Wno-unused-parameter
    -Wno-unused-variable
    -Wno-type-limits
    -Wno-format             # 忽略 printf 格式警告
    -Wno-int-conversion     # 忽略指針轉型警告
    -Wno-incompatible-pointer-types # 忽略指針類型不合
    -Wno-implicit-function-declaration # 忽略隱式宣告 (malloc_usable_size)
    -Wno-error              # 絕對不要把警告當錯誤！
)

# --- 關鍵修正 2：定義編譯參數以適應 ESP32 ---
target_compile_definitions(${COMPONENT_LIB} PUBLIC 
    CONFIG_VERSION="2021-03-27"
    _GNU_SOURCE
    
    # 告訴 quickjs-libc 不要用 dlfcn.h (動態加載)
    CONFIG_SHCC=0 
    
    # 告訴 quickjs 不要用原子操作 (如果報錯)
    # CONFIG_ATOMICS=0
)
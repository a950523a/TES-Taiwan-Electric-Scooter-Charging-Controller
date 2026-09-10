cmake_minimum_required(VERSION 3.16)

list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/components")

# 版本號：優先用 git describe（含 commit hash 和 dirty 標記），供本機測試識別用
# GitHub Actions tag push 時 git describe 直接回傳 tag 名稱（如 v3.2.0）
#
# execute_process 只在 CMake configure 階段跑一次，之後單純 idf.py build 不會重跑 ——
# 版本字串會一直停在上次 configure 的值（實測曾落後 5 個 commit，裝置回報的
# firmware_version 因此不可信）。把 HEAD 與 index 列為 configure 的相依，
# commit 或 stage 之後就會自動重新設定。
foreach(_git_dep "${CMAKE_SOURCE_DIR}/../.git/HEAD" "${CMAKE_SOURCE_DIR}/../.git/index")
    if(EXISTS "${_git_dep}")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_git_dep}")
    endif()
endforeach()

execute_process(
    COMMAND git describe --always --tags --dirty
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE GIT_VERSION
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(NOT GIT_VERSION)
    set(GIT_VERSION "dev")
endif()
set(PROJECT_VER "${GIT_VERSION}")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(tes_charger)

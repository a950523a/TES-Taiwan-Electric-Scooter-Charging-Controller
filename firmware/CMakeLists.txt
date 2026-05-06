cmake_minimum_required(VERSION 3.16)

list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/components")

# 版本號：優先用 git describe（含 commit hash 和 dirty 標記），供本機測試識別用
# GitHub Actions tag push 時 git describe 直接回傳 tag 名稱（如 v3.2.0）
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

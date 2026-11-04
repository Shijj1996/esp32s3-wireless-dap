cmake_minimum_required(VERSION 3.16)

# Colored Compiler Output with ninja
add_compile_options(-fdiagnostics-color=always)

# The upstream sources use project-root-relative includes such as
# "main/tcp_server.h" and "components/DAP/include/DAP.h".
# Keeping the project root on the include path lets every component keep
# those include statements unchanged.
include_directories(${CMAKE_CURRENT_SOURCE_DIR})

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(wireless_esp_dap)

# This file was automatically generated for projects
# without default 'CMakeLists.txt' file.

FILE(GLOB_RECURSE app_sources ${CMAKE_SOURCE_DIR}/src/*.*)
# Exclude DAS_v0.1 files for ESP32 builds
if(CMAKE_SYSTEM_PROCESSOR MATCHES "xtensa")
    list(FILTER app_sources EXCLUDE REGEX .*src/Devices/DAS_v0\.1.*)
endif()

idf_component_register(SRCS ${app_sources})

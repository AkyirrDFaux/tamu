# This file was automatically generated for projects
# without default 'CMakeLists.txt' file.

# The core factory bootloader is a separate app built from its own single source
# (Tamu_bootloader env passes -DTAMU_BOOTLOADER=ON).
if(TAMU_BOOTLOADER)
    idf_component_register(SRCS "Bootloader/CoreBootloader.cpp")
    return()
endif()

FILE(GLOB_RECURSE app_sources ${CMAKE_SOURCE_DIR}/src/*.*)
# The factory bootloader has its own app_main and is a separate image.
list(FILTER app_sources EXCLUDE REGEX .*src/Bootloader/.*)
# Exclude DAS_v0.1 files for ESP32 builds
if(CMAKE_SYSTEM_PROCESSOR MATCHES "xtensa")
    list(FILTER app_sources EXCLUDE REGEX .*src/Devices/DAS_v0\.1.*)
endif()

idf_component_register(SRCS ${app_sources})

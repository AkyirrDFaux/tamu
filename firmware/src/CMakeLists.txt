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
# Exclude the CH32V003 DAS sources from every ESP-IDF target. The DAS belongs to the ch32v
# `noneos-sdk` platform, not IDF. Key on the IDF target arch (xtensa *and* riscv) - the old
# `CMAKE_SYSTEM_PROCESSOR MATCHES "xtensa"` was dead on the RISC-V ESP32-C3.
idf_build_get_property(idf_target_arch IDF_TARGET_ARCH)
if(idf_target_arch MATCHES "^(xtensa|riscv)")
    list(FILTER app_sources EXCLUDE REGEX .*src/Devices/DAS_v0\.1.*)
endif()

idf_component_register(SRCS ${app_sources})

# This file was automatically generated for projects
# without default 'CMakeLists.txt' file.

# The core (ESP32-C3) factory image (Tamu_bootloader env passes -DTAMU_BOOTLOADER=ON): one
# source, both transports - the USB frames of the original image plus the pairing-free BLE
# updater - so it needs NimBLE and NVS.
if(TAMU_BOOTLOADER)
    idf_component_register(SRCS "Bootloader/CoreBootloader.cpp"
                           REQUIRES bt nvs_flash)
    return()
endif()

# The deliberately broken main-app image used to exercise the recovery path on the rig
# (Tamu_badimage env): it crashes in a static initialiser, i.e. before the main app can erase
# otadata to confirm itself - exactly the case the bootloader's rollback covers.
if(TAMU_BADIMAGE)
    idf_component_register(SRCS "Test/BadImage.cpp")
    return()
endif()

FILE(GLOB_RECURSE app_sources ${CMAKE_SOURCE_DIR}/src/*.*)
# The factory bootloader has its own app_main and is a separate image.
list(FILTER app_sources EXCLUDE REGEX .*src/Bootloader/.*)
# Rig fixtures (src/Test/) are separate images too: they define their own app_main.
list(FILTER app_sources EXCLUDE REGEX .*src/Test/.*)
# Exclude the CH32V003 DAS sources from every ESP-IDF target. The DAS belongs to the ch32v
# `noneos-sdk` platform, not IDF. Key on the IDF target arch (xtensa *and* riscv) - the old
# `CMAKE_SYSTEM_PROCESSOR MATCHES "xtensa"` was dead on the RISC-V ESP32-C3.
idf_build_get_property(idf_target_arch IDF_TARGET_ARCH)
if(idf_target_arch MATCHES "^(xtensa|riscv)")
    list(FILTER app_sources EXCLUDE REGEX .*src/Devices/DAS_v0\.1.*)
    # Same reasoning for the CH32V203 Valu sources: its custom startup .S files define `_start`
    # and would otherwise collide with each other inside an ESP-IDF image.
    list(FILTER app_sources EXCLUDE REGEX .*src/Devices/Valu_v2\.0.*)
endif()

idf_component_register(SRCS ${app_sources})

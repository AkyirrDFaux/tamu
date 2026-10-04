cmake_minimum_required(VERSION 3.16.0)
# The factory bootloader needs no managed components (the main app's src/idf_component.yml pulls
# in h2zero/esp-nimble-cpp). Disable the component manager for this build only, keyed on the
# bootloader env's -DTAMU_BOOTLOADER=ON. set(ENV{...}) affects just this CMake process, unlike
# mutating os.environ, so a `pio run -e Tamu_bootloader -e Tamu_v2_0A` build still resolves the
# app's managed component.
if(TAMU_BOOTLOADER)
    set(ENV{IDF_COMPONENT_MANAGER} "0")
endif()
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(tamu)

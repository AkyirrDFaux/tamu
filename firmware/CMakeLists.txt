cmake_minimum_required(VERSION 3.16.0)
# The factory image and the rig fixtures need no managed components (the main app's
# src/idf_component.yml pulls in h2zero/esp-nimble-cpp). Disable the component manager for these
# builds only, keyed on the env's -DTAMU_BOOTLOADER=ON / -DTAMU_BADIMAGE=ON. set(ENV{...}) affects
# just this CMake process, unlike mutating os.environ, so a `pio run -e Tamu_bootloader -e
# Tamu_v2_0A` build still resolves the app's managed component.
if(TAMU_BOOTLOADER OR TAMU_BADIMAGE)
    set(ENV{IDF_COMPONENT_MANAGER} "0")
endif()
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(tamu)

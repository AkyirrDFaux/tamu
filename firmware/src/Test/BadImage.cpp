// A deliberately broken main-app image for the recovery demonstration on the rig (kanban
// t_3d74f8d1). NOT shipped firmware - it is written to `ota_0` by the updater and then booted, so
// that the boot decision's recovery path can be observed on real hardware, not only in the host
// test.
//
// It crashes in a static initialiser, which runs before app_main - and the main app's proof of
// life IS the otadata erase at the top of app_main. That makes this the exact case the attempt
// budget cannot catch (the app never gets far enough to spend an attempt) and the one the
// second-stage bootloader's pending-verify/rollback exists for: the image is selected with state
// NEW, never confirms itself, and the next reset rolls the slot back to `factory`.
//
// The image is a real, valid ESP image (correct header and checksum), so the bootloader loads and
// jumps to it - unlike a corrupt body, which the loader would reject before it ever ran.
//
// Build: pio run -e Tamu_badimage   (bin: .pio/build/Tamu_badimage/firmware.bin)
// Flash/transfer it into ota_0 with the updater (USB frames or esptool at the ota_0 offset).

#include <cstdlib>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace
{
    // Runs from the C++ static initialiser, i.e. before app_main and therefore before the app can
    // erase otadata to confirm itself.
    struct CrashBeforeMain
    {
        CrashBeforeMain() { abort(); }
    };
    CrashBeforeMain Crash;
}

extern "C" void app_main(void)
{
    // Never reached: the initialiser above aborts first. Kept so the image has a normal entry
    // point if the crash is ever moved into the task.
    for (;;)
        vTaskDelay(pdMS_TO_TICKS(1000));
}

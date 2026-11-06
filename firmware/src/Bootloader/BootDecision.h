#pragma once

// Factory-image boot decision (Docs/Services/Bootloader.md): the "bootloader" is a factory
// app, so it runs on every reset and decides between "serve an update" and "start the main
// app". This header holds that decision and nothing else, so it can be executed on the host
// (firmware/test/native/updater_boot_test.cpp) instead of only being read.
//
// It exists because a BLE updater has to survive a bad image with no cable attached. The
// shipped image already refuses to boot an `ota_0` whose header magic is not 0xE9, but a
// VALID-looking image that crashes early is a reboot loop: the factory app runs again, sees
// the same good magic, and starts it again. The attempt budget below closes that: every boot
// of the main app spends one attempt, the app clears the budget once it has proved it is
// alive, and a device that keeps spending attempts stops booting the app and stays in update
// mode - which now offers BLE as well as USB, so it is recoverable without a cable.
//
// Deliberately device-free: no NVS, no flash, no GPIO. The caller reads the three inputs and
// stores the result; the numbers live here.

#include <cstdint>

namespace UpdaterBoot
{
    // Attempts the main app gets before the factory image stops booting it. Three is enough
    // for a link that comes up slowly or a once-off early crash, and short enough that a
    // walking owner sees the LED of update mode within a couple of resets.
    constexpr uint8_t MAX_APP_BOOTS = 3;

    enum class Action : uint8_t
    {
        UpdateMode, // stay here: serve frames over USB and (when its window is open) BLE
        BootApp     // select ota_0 and restart into the main app
    };

    struct Inputs
    {
        bool ButtonHeld;     // B1 (GPIO2, active low) is held at reset
        bool ImagePresent;   // ota_0's first byte is the ESP image magic 0xE9
        bool ImageRolledBack; // the bootloader rolled that slot back (otadata says ABORTED/INVALID)
        uint8_t FailedBoots; // attempts recorded since the app last proved healthy
    };

    // The button is an unconditional override: it is the physical-presence proof the update
    // window is built on, and it has to work even while the app is healthy.
    inline Action Decide(const Inputs &In)
    {
        if (In.ButtonHeld)
            return Action::UpdateMode;
        // Nothing bootable in ota_0 (a factory-fresh device, or an update that never landed
        // an image at all): booting it would only reboot through here.
        if (!In.ImagePresent)
            return Action::UpdateMode;
        // The second-stage bootloader rolled the slot back because the image never confirmed
        // itself (it crashed before its otadata erase). Booting it again would repeat exactly
        // that - the case the attempt budget cannot see, because the app never gets far enough
        // to spend an attempt through this decision. A new write pass clears the state.
        if (In.ImageRolledBack)
            return Action::UpdateMode;
        if (In.FailedBoots >= MAX_APP_BOOTS)
            return Action::UpdateMode;
        return Action::BootApp;
    }

    // The value to persist before acting on Decide()'s answer: booting the app spends one
    // attempt, staying in update mode spends none (a device parked in update mode must still
    // refuse to boot that image after the next reset - otherwise the budget is a 3-second
    // pause, not a recovery).
    inline uint8_t NextFailedBoots(Action Taken, uint8_t FailedBoots)
    {
        if (Taken != Action::BootApp)
            return FailedBoots;
        return (uint8_t)(FailedBoots < 0xFF ? FailedBoots + 1 : 0xFF);
    }

    // What the main app stores once it is demonstrably alive (it has run its main loop for a
    // while without faulting). Zero, not "attempts - 1": a device that ran fine for hours and
    // then died on the next reset deserves the full budget again, and a counter that only
    // decrements would leave a device that crashes once a week permanently one attempt short.
    constexpr uint8_t HealthyBoots() { return 0; }
}

// Host-side test for the pairing-free updater's two decisions (kanban t_79a194d5):
//
//   Bootloader/BootDecision.h   - does the factory image boot the app or stay in update mode?
//   Bootloader/UpdaterAdmission.h - may a frame written over BLE be accepted?
//
// Both are pure functions, so the property that matters most for a BLE updater - "a bad image
// is recoverable without a cable" - is exercised here as a sequence of resets, instead of only
// being read off the source. The simulation below is the device: four inputs (button, image
// magic, the bootloader's rollback state, stored attempt count), the decision, and the stored
// value the decision implies.
//
// What a run through this file proves:
//   * a device with no image never tries to boot one;
//   * a good app boots, and a healthy app refunds the budget it spent;
//   * an image that looks valid but never proves healthy gets MAX_APP_BOOTS tries and then the
//     device parks in update mode - and stays parked across further resets, because staying in
//     update mode spends no attempt;
//   * the button is an override that costs nothing (released, the next reset boots again);
//   * a new write pass restarts the budget, so a repaired image is not immediately written off.

#include <cstdint>
#include <cstdio>

#include "Bootloader/BootDecision.h"
#include "Bootloader/UpdaterAdmission.h"

static int checks = 0, failures = 0;

#define CHECK(cond)                                                                  \
    do {                                                                             \
        checks++;                                                                    \
        if (!(cond)) {                                                               \
            failures++;                                                              \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);              \
        }                                                                            \
    } while (0)

#define CHECK_EQ(got, want)                                                          \
    do {                                                                             \
        checks++;                                                                    \
        const long long g = (long long)(got), w = (long long)(want);                 \
        if (g != w) {                                                                \
            failures++;                                                              \
            std::printf("FAIL %s:%d: %s = %lld, want %lld\n", __FILE__, __LINE__,    \
                        #got, g, w);                                                 \
        }                                                                            \
    } while (0)

namespace
{
    // The device: what the factory image can see at reset, plus the NVS byte it owns.
    struct Device
    {
        bool ImagePresent;
        uint8_t FailedBoots;
        // The second-stage bootloader rolled the slot back (otadata says ABORTED). Cleared by the
        // bootloader selecting it again, which only happens after a new write pass.
        bool ImageRolledBack = false;
    };

    // One reset: decide, then store what the decision spends. Returns the action taken.
    UpdaterBoot::Action Reset(Device &Dev, bool ButtonHeld)
    {
        const UpdaterBoot::Action Taken = UpdaterBoot::Decide(UpdaterBoot::Inputs{
            ButtonHeld, Dev.ImagePresent, Dev.ImageRolledBack, Dev.FailedBoots});
        Dev.FailedBoots = UpdaterBoot::NextFailedBoots(Taken, Dev.FailedBoots);
        return Taken;
    }

    // The main app proving it is alive (it ran its loop; a healthy start refunds the budget).
    void AppBecomesHealthy(Device &Dev) { Dev.FailedBoots = UpdaterBoot::HealthyBoots(); }

    // A write pass starting at offset 0 - what the updater does before programming a new image.
    // It forgets the previous image's budget AND its rollback state (CoreBootloader.cpp erases
    // otadata on the same pass), so the image about to be written is bootable again.
    void UpdateStarts(Device &Dev)
    {
        Dev.FailedBoots = UpdaterBoot::HealthyBoots();
        Dev.ImageRolledBack = false;
    }

    constexpr bool kBoot = true;
    constexpr bool kUpdate = false;
}

int main()
{
    // ---- the budget is what the design says it is --------------------------------
    CHECK_EQ(UpdaterBoot::MAX_APP_BOOTS, 3);

    // ---- a device with nothing in ota_0 (fresh factory flash) --------------------
    {
        Device Dev{false, 0};
        CHECK_EQ(Reset(Dev, false), (int)UpdaterBoot::Action::UpdateMode);
        CHECK_EQ(Reset(Dev, false), (int)UpdaterBoot::Action::UpdateMode);
        // Staying in update mode never spends an attempt: the counter is still untouched.
        CHECK_EQ(Dev.FailedBoots, 0);
    }

    // ---- a good app: boots, spends one attempt, and a healthy run refunds it -----
    {
        Device Dev{true, 0};
        CHECK_EQ(Reset(Dev, false), (int)UpdaterBoot::Action::BootApp);
        CHECK_EQ(Dev.FailedBoots, 1);
        AppBecomesHealthy(Dev);
        CHECK_EQ(Dev.FailedBoots, 0);
        // ... and the next reset boots again, so a healthy device never drifts toward the limit.
        CHECK_EQ(Reset(Dev, false), (int)UpdaterBoot::Action::BootApp);
        CHECK_EQ(Dev.FailedBoots, 1);
    }

    // ---- the recovery path: an image whose magic is valid but which never gets healthy ----
    {
        Device Dev{true, 0};
        // Three attempts, then the factory image refuses to boot it again.
        for (int i = 0; i < UpdaterBoot::MAX_APP_BOOTS; i++)
        {
            CHECK_EQ(Reset(Dev, false), (int)UpdaterBoot::Action::BootApp);
            CHECK_EQ(Dev.FailedBoots, i + 1);
        }
        CHECK_EQ(Reset(Dev, false), (int)UpdaterBoot::Action::UpdateMode);
        // Parked: every further reset stays in update mode, so the device is still there for a
        // phone minutes or days later. This is the property the whole budget exists for - a
        // "stay in update mode for one reset" budget would re-arm the broken image.
        for (int i = 0; i < 5; i++)
            CHECK_EQ(Reset(Dev, false), (int)UpdaterBoot::Action::UpdateMode);
        CHECK_EQ(Dev.FailedBoots, UpdaterBoot::MAX_APP_BOOTS);
    }

    // ---- the repaired image: a new write pass restarts the budget ----------------
    {
        Device Dev{true, UpdaterBoot::MAX_APP_BOOTS};
        UpdateStarts(Dev);
        CHECK_EQ(Dev.FailedBoots, 0);
        // The freshly written image gets the full budget, not the remains of the old one.
        CHECK_EQ(Reset(Dev, false), (int)UpdaterBoot::Action::BootApp);
        CHECK_EQ(Dev.FailedBoots, 1);
    }

    // ---- an image that crashed before it could spend an attempt through us -------
    // This is the case the budget cannot see: the app died before its first statement, so it never
    // reached this image to spend an attempt. The second-stage bootloader rolled the slot back and
    // otadata says ABORTED; booting it again would be the same crash, so update mode it is.
    {
        Device Dev{true, 0, true};
        for (int i = 0; i < 3; i++)
            CHECK_EQ(Reset(Dev, false), (int)UpdaterBoot::Action::UpdateMode);
        // The budget is untouched - the decision does not spend an attempt for a slot it refuses.
        CHECK_EQ(Dev.FailedBoots, 0);
        // ... and the button still works (it is checked before anything else).
        CHECK_EQ(Reset(Dev, true), (int)UpdaterBoot::Action::UpdateMode);
    }

    // The rollback does not latch: a new write pass clears the state along with the budget, so the
    // image that replaced the bad one is booted with a full budget.
    {
        Device Dev{true, 0, true};
        CHECK_EQ(Reset(Dev, false), (int)UpdaterBoot::Action::UpdateMode);
        UpdateStarts(Dev);
        CHECK(!Dev.ImageRolledBack);
        CHECK_EQ(Reset(Dev, false), (int)UpdaterBoot::Action::BootApp);
        CHECK_EQ(Dev.FailedBoots, 1);
    }

    // A rolled-back slot that also has a spent budget keeps refusing on the *budget* path too:
    // neither input may resurrect the image.
    {
        Device Dev{true, UpdaterBoot::MAX_APP_BOOTS, true};
        CHECK_EQ(Reset(Dev, false), (int)UpdaterBoot::Action::UpdateMode);
        CHECK_EQ(Dev.FailedBoots, UpdaterBoot::MAX_APP_BOOTS);
    }


    // ---- the button: an override that costs nothing ------------------------------
    {
        Device Dev{true, 0};
        CHECK_EQ(Reset(Dev, true), (int)UpdaterBoot::Action::UpdateMode);
        CHECK_EQ(Dev.FailedBoots, 0); // a press must not eat into the image's budget
        CHECK_EQ(Reset(Dev, false), (int)UpdaterBoot::Action::BootApp);
    }

    // ---- the button also overrides a spent budget (that is the cable-free escape) --
    {
        Device Dev{true, UpdaterBoot::MAX_APP_BOOTS};
        CHECK_EQ(Reset(Dev, false), (int)UpdaterBoot::Action::UpdateMode);
        CHECK_EQ(Reset(Dev, true), (int)UpdaterBoot::Action::UpdateMode);
        CHECK_EQ(Dev.FailedBoots, UpdaterBoot::MAX_APP_BOOTS);
    }

    // ---- an image that vanishes under us (0xFF magic) is not booted --------------
    {
        Device Dev{true, 0};
        Dev.ImagePresent = false;
        CHECK_EQ(Reset(Dev, false), (int)UpdaterBoot::Action::UpdateMode);
        CHECK_EQ(Dev.FailedBoots, 0);
    }

    // ---- the counter cannot wrap into "bootable again" ---------------------------
    {
        Device Dev{true, 0xFF};
        CHECK_EQ(UpdaterBoot::NextFailedBoots(UpdaterBoot::Action::BootApp, 0xFF), 0xFF);
        CHECK_EQ(Reset(Dev, false), (int)UpdaterBoot::Action::UpdateMode);
    }

    // ---- the update window ------------------------------------------------------
    {
        const uint32_t T0 = 5000;
        const uint32_t End = UpdaterAdmission::WindowEnd(T0);
        CHECK_EQ(End, T0 + 300000);
        CHECK(UpdaterAdmission::WindowOpen(T0, End));             // the moment it opens
        CHECK(UpdaterAdmission::WindowOpen(T0 + 299999, End));    // the last millisecond
        CHECK(!UpdaterAdmission::WindowOpen(T0 + 300000, End));   // exactly at the deadline
        CHECK(!UpdaterAdmission::WindowOpen(T0 + 300001, End));   // and afterwards
        CHECK(!UpdaterAdmission::WindowOpen(T0, 0));              // no window at all
    }

    // The deadline arithmetic is a signed difference, so it survives the uptime counter wrapping
    // past 2^32 ms (49.7 days): a device that has been up that long must not read its window as
    // either permanently open or permanently closed.
    {
        const uint32_t T0 = 0xFFFFFF00u; // 256 ms before the counter wraps
        const uint32_t End = UpdaterAdmission::WindowEnd(T0);
        CHECK(End < T0); // it wrapped
        CHECK(UpdaterAdmission::WindowOpen(T0, End));
        CHECK(UpdaterAdmission::WindowOpen(0x00001000u, End));      // after the wrap, 3.5 s in
        CHECK(!UpdaterAdmission::WindowOpen(0x00050000u, End));     // well past it
    }

    // ---- write admission: the window AND a live link ------------------------------
    CHECK(UpdaterAdmission::WriteAllowed(true, true));
    CHECK(!UpdaterAdmission::WriteAllowed(true, false));  // no link: nothing to accept from
    CHECK(!UpdaterAdmission::WriteAllowed(false, true));  // window shut: a live link is refused
    CHECK(!UpdaterAdmission::WriteAllowed(false, false));

    std::printf("%s: %d checks, %d failures\n", failures == 0 ? "PASS" : "FAIL", checks, failures);
    return failures == 0 ? 0 : 1;
}

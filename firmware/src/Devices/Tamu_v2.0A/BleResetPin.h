#pragma once

// The device half of the USB-less BLE reset (Blocks/BleResetHold.h): GPIO8 as the input, GPIO2 as the
// LED it lights, and AppBLEStackReset() as the action it performs. Included from Main.h after
// AppBLE.h (the action) and Button.h (the notification driver it must hand the pin back to).
//
// GPIO8 is otherwise unused on this board (0/3 = the LED strips, 2 = the notification LED and the
// button, 4/5 = I2C, 6/10 = fan PWM, 9 = RS485 enable, 18/19 = USB, 20/21 = RS485), and it is one of
// the ESP32-C3 strapping pins: it must be HIGH at reset, which is why Blocks/BleResetHold.h refuses to
// time a contact until the pin has been seen released once.
//
// The notification LED shares its pin with the button input (Docs/Devices.md: "LED-Button ... button
// active low, LED active low"). Two consequences this glue has to respect:
//
//   - during a recognised hold the pin is left as an INPUT with the pull-up: the closed contact pulls
//     it low, which lights the red LED by itself and keeps the level readable for the release. Driving
//     it low would light the same LED and blind the sequencer to the release;
//   - the five blinks after a clear are the only time the sequencer DRIVES the pin, and only ever to
//     LOW (lit) or back to INPUT with the pull-up (dark) - never HIGH, which with the contact still
//     closed would drive 3V3 into ground through the pin. While the contact is still down its own low
//     wins during an "off" half, so the gaps of the five blinks are only visible once it is released;
//     the sequence itself runs regardless, which is what the owner asked for.
//
// While the notification driver has the LED ON, the line is driven low by that driver and the reset
// input cannot be read at all (the same level a closed contact gives). The sequencer is told so
// (`Readable`) and holds its state instead of guessing a contact; the button's own read is already
// suppressed in that state by ButtonUpdate.

#include "Blocks/BleResetHold.h"

// The reset input. GPIO8 has no other job on this board.
static const gpio_num_t BLE_RESET_PIN = GPIO_NUM_8;

// The one instance of the sequencer (Blocks/BleResetHold.h).
static BleResetHold BleResetState;

// The two questions Button.h asks. Declared there (Button.h comes first in Main.h's include order)
// and defined here, where the sequencer lives.
bool BleResetOwnsPin();
bool BleResetButtonPressed();

// Boot: the pin as an input with the internal pull-up (released = HIGH). The first sample the
// sequencer sees IS the boot level, and a low one arms nothing until the pin is released once.
void BleResetInit()
{
    PinModeInputPullUp(BLE_RESET_PIN);
    BleResetStep(BleResetState, PinRead(BLE_RESET_PIN), TimeFromBoot());
    DeviceLog("BLERST", "reset input ready (GPIO%d, %s)", (int)BLE_RESET_PIN,
              BleResetState.ReleasedSeen ? "released" : "held low at boot: ignored until released");
}

// Drives the shared pin for the sequencer: `Lit` = output LOW (the LED is active low), otherwise
// INPUT with the pull-up (dark, and readable again).
static void BleResetApplyPin(bool Lit)
{
    if (Lit)
    {
        PinModeOutput(LED_NOTIFICATION_PIN);
        PinLow(LED_NOTIFICATION_PIN);
    }
    else
    {
        PinModeInputPullUp(LED_NOTIFICATION_PIN);
    }
}

// Hands the pin back to the notification driver: re-applies the LED/Button block's own LEDState, so
// the LED goes back to whatever the app asked for and ButtonUpdate can read the contact again.
static void BleResetHandBack()
{
    OnLEDStateChange(StaticBlockRegistry[0], 3, (const void *)&StaticVol.LedButton.LEDState, sizeof(bool));
}

bool BleResetOwnsPin()
{
    return BleResetPinOwned(BleResetState.Phase);
}

bool BleResetButtonPressed()
{
    return BleResetButtonPressed(BleResetState, TimeFromBoot());
}

// One pass of the reset input, from the main loop: sample, apply the LED, perform the action.
//
// The action is the BLE stack restart the card asks for (AppBLEStackReset, the same forget-and-rebuild
// path the owner's USB commands use). The clear path reports its completion here, so the five blinks
// are timed from the moment the bonds are really gone instead of from the 10 s mark.
void BleResetService()
{
    // The line can be read unless the notification driver is holding it low with its LED on.
    const bool Readable = BleResetOwnsPin() || StaticVol.LedButton.LEDState == false;
    const uint32_t NowMs = TimeFromBoot();
    const BleResetOutput Out = BleResetStep(BleResetState, PinRead(BLE_RESET_PIN), NowMs, Readable);

    // 1. The pin: driven only while the sequencer owns it.
    if (Out.PinOwned)
        BleResetApplyPin(Out.LedLit);
    if (Out.HandBackPin)
        BleResetHandBack();

    // 2. The action. The restart takes a moment (the host task is stopped and restarted), and the
    //    blink sequence starts after it - not before - so it cannot be truncated by the restart.
    if (Out.Action != BleResetAction::None)
    {
        DeviceLog("BLERST", "%s", BleResetActionName(Out.Action));
        const bool Clear = Out.Action == BleResetAction::ClearBondsAndRestart;
        if (!AppBLEStackReset(Clear))
            DeviceLog("BLERST", "the reset action did not complete");
        if (Clear)
            BleResetClearComplete(BleResetState, TimeFromBoot());
    }
}

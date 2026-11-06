#pragma once

// The device half of the USB-less BLE reset (Blocks/BleResetHold.h): GPIO8 as the input, GPIO2 as the
// LED it drives, and AppBLEStackReset() as the action it performs. Included from Main.h after
// AppBLE.h (the action) and Button.h (the notification driver it must hand the pin back to).
//
// GPIO8 is otherwise unused on this board (0/3 = the LED strips, 2 = the notification LED and the
// button, 4/5 = I2C, 6/10 = fan PWM, 9 = RS485 enable, 18/19 = USB, 20/21 = RS485). It is one of the
// ESP32-C3 strapping pins - it must be HIGH at reset, which is why Blocks/BleResetHold.h refuses to
// time a contact until the pin has been seen released once - and it is a pad of its own: the GPIO2
// driver can neither drive it nor change what it reads, so the reset input is sampled on every pass
// whatever the notification LED is doing.
//
// The notification LED shares its pin with the button input (Docs/Devices.md: "LED-Button ... button
// active low, LED active low"), which is why this glue drives GPIO2 while the sequencer owns it:
//
//   - from the 2 s mark of a recognised hold, and through the five blinks after a clear, GPIO2 is
//     driven LOW (lit). That drive is what lights the owner's "solid red until lifting the contact" -
//     the contact on GPIO8 does not light anything by itself;
//   - the dark halves of the blinks are INPUT with the pull-up, so the five gaps are visible while the
//     GPIO8 contact is still down and the sequence runs regardless of it;
//   - GPIO2 is never driven HIGH: with the button held that is the pin shorting 3V3 to ground;
//   - ButtonUpdate suppresses the button's own report while the sequencer owns the pin, because a
//     line driven low by this sequencer reads as a pressed button.

#include "Blocks/BleResetHold.h"

// The reset input. GPIO8 has no other job on this board.
static const gpio_num_t BLE_RESET_PIN = GPIO_NUM_8;

// The one instance of the sequencer (Blocks/BleResetHold.h).
static BleResetHold BleResetState;

// The question Button.h asks (declared there, because Button.h comes first in Main.h's include
// order) and defined here, where the sequencer lives.
bool BleResetOwnsPin();

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
// INPUT with the pull-up (dark, and the button read is live again).
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

// One pass of the reset input, from the main loop: sample, apply the LED, perform the action.
//
// The action is the BLE stack restart the card asks for (AppBLEStackReset, the same forget-and-rebuild
// path the owner's USB commands use). The clear path reports its completion here, so the five blinks
// are timed from the moment the bonds are really gone instead of from the 10 s mark.
void BleResetService()
{
    const uint32_t NowMs = TimeFromBoot();
    const BleResetOutput Out = BleResetStep(BleResetState, PinRead(BLE_RESET_PIN), NowMs);

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

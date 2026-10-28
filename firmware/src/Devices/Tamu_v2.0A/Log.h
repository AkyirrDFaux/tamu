#pragma once

#include "Core/Services/LogHandler.h"
#include "Core/Functions/AppInterface.h"
#include "Core/Functions/AppConsole.h"
#include "esp_log.h"
#include <cstdarg>

// Free-text diagnostics go to the USB Serial/JTAG console, which is ALSO the app link's byte
// stream (Core/Functions/AppInterface.h, Docs/Services/App Interface.md). The copy is therefore
// suppressed while that stream is IN USE - not while a cable is attached (Issues.md E67, route (d),
// owner ruling 2026-10-28, "D is sufficient"):
//   - `AppUSBActive()` alone is `usb_serial_jtag_is_connected()`, which reads true ~0.4 s after a
//     reset whenever a host merely HOLDS the port, so gating on it kept every runtime line off a rig
//     capture (only the boot banner got out, before the first Pump());
//   - the rule is `AppConsoleBusy(...)` (Core/Functions/AppConsole.h): a host that is TALKING on
//     the link suppresses the line, because a reply is in flight or expected; a line written into a
//     silent link is harmless, and that is what makes a rig session readable;
//   - `AppUsbSilent()` is the ~500 ms idle test the TX pump already uses, and a frame still queued
//     in the TX ring counts as "in use" too (the pump hands out 60 B at a time, so a line between
//     two chunks would split it).
//
// The wire format, the LOG_ codes and the block layouts are untouched by this: it is one guard.
//
// Do not re-open this as a second sink: the log database behind `GetLogs` stores a source/code PAIR
// per entry and folds repeats into a count, so a formatted line has nowhere to go there; the console
// IS the app link's byte stream, so a line written mid-frame corrupts the frame it lands in. A line
// kept in RAM and streamed by a new CID was withdrawn by the owner on 2026-10-28.
void DeviceLog(const char *Tag, const char *Fmt, ...)
{
    if (AppConsoleBusy(AppUSBActive(), AppUsbSilent(), AppInterface.TxHead, AppInterface.TxTail))
        return;
    char Buffer[128];
    va_list Args;
    va_start(Args, Fmt);
    vsnprintf(Buffer, sizeof(Buffer), Fmt, Args);
    va_end(Args);
    ESP_LOGI(Tag, "%s", Buffer);
}

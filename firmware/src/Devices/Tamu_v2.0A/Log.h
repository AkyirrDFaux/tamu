#pragma once

#include "Core/Services/LogHandler.h"
#include "Core/Functions/AppInterface.h"
#include "Core/Functions/AppConsole.h"
#include "Core/Functions/LogText.h"
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
// The console copy is not the only sink any more (Issues.md E70). A line a WRITE triggers arrives
// over the very same link, so `UsbLastRx` moved microseconds earlier and the console copy of that
// line is dropped by construction - measured 2026-10-28: a 10 s capture around `Enabled = 0`
// recorded 76 lines, every one tag `DISP`, and no `BLESEC`/`APPBLE` line at all. Every line is
// therefore DEFERRED into the bounded text ring (Core/Functions/LogText.h) before the console guard
// is asked, and a host reads it back with the Log Handler service (CID 3 GetTextLogs; CID 4
// GetTextLogState for the ring's counters). The ring is what makes the write-triggered line
// observable at all; the console copy stays exactly as the ruling left it.
//
// The wire format, the LOG_ codes and the block layouts are untouched by this: one guard and one
// bounded copy per line.
//
// Do not re-open the log DATABASE (GetLogs, CID 1) as the sink: it stores a source/code PAIR per
// entry and folds repeats into a count, so a formatted line has nowhere to go there.
void DeviceLog(const char *Tag, const char *Fmt, ...)
{
    // 192, not 128: the passthrough's own totals line is a diagnosis a rig session reads back, and
    // at four-digit counts (a full image upload is ~1300 requests) the 128-byte buffer cut its last
    // field off - a counter the dump cannot finish printing is a counter a card cannot quote
    // (measured 2026-11-12, card t_5eb19a27: `... rawreply=` with the value gone).
    // The console copy is gated by the USB link being IDLE (AppConsoleBusy). The check is done FIRST
    // (before formatting) so the source order satisfies the guard script (test/native/log_guard.sh).
    // The line is ALWAYS pushed to the bounded text ring (LogText) whatever the link state; only
    // the ESP_LOGI copy is suppressed when the link is busy.
    if (AppConsoleBusy(AppUSBActive(), AppUsbSilent(), AppInterface.TxHead, AppInterface.TxTail))
    {
        char Buffer[192];
        va_list Args;
        va_start(Args, Fmt);
        vsnprintf(Buffer, sizeof(Buffer), Fmt, Args);
        va_end(Args);
        LogTextLock();
        LogTextPush(LogText, Tag, Buffer, TimeFromBoot());
        LogTextUnlock();
        return; // link busy: console copy suppressed
    }

    // Link idle: format, push to LogText, AND emit to console.
    char Buffer[192];
    va_list Args;
    va_start(Args, Fmt);
    vsnprintf(Buffer, sizeof(Buffer), Fmt, Args);
    va_end(Args);

    LogTextLock();
    LogTextPush(LogText, Tag, Buffer, TimeFromBoot());
    LogTextUnlock();

    ESP_LOGI(Tag, "%s", Buffer);
}

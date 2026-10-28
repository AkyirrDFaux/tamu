#!/bin/bash
# Source guard for Issues.md E67 route (d) - the console copy belongs to the USB link being IDLE,
# not to a cable being attached.
#
# Devices/Tamu_v2.0A/Log.h cannot be linked on the host (it includes ESP-IDF's esp_log.h and the
# device USB glue), so what is asserted here is the SHAPE of the guard. The decision it delegates to
# is host-tested: test/native/appconsole_test.cpp drives AppConsoleBusy() (Core/Functions/
# AppInterface.h) through its truth table. The defect this pins:
#
#   DeviceLog() used to return as soon as `AppInterface.Connected` was true, and AppUSBActive() is
#   usb_serial_jtag_is_connected() - true ~0.4 s after a reset whenever a host merely HOLDS the
#   port. So a rig session with the cable attached read the boot banner and nothing else, which is
#   why E65/E66/E69 had to be pinned by timing and register reads. The ruling (owner, 2026-10-28,
#   "D is sufficient") keeps the console copy while the link is silent: silence means nothing is in
#   flight to interleave a line into. The cost, stated in the entry: nothing is shown while an app
#   is actually talking.
#
# What must hold:
#   1. the old presence guard (`AppInterface.Connected`) is gone - presence is not traffic;
#   2. the guard asks AppConsoleBusy(...) with the three live inputs: the link being up, the idle
#      test, and the TX ring's head/tail (a queued frame means one is being written out now);
#   3. the idle test is AppUsbSilent() (~500 ms without a frame) and it treats "nothing has ever
#      arrived since boot" as silent, or the boot lines vanish (uptime 0 is less than 500 ms);
#   4. the suppression happens BEFORE the format/emit, so a line cannot be written into a live
#      stream by reordering, and there is still exactly one sink (ESP_LOGI).
#
# Usage: bash test/native/log_guard.sh <Log.h> <AppUSB.h> [AppInterface.h]
# Exit 0 = the rule holds. Any FAIL line names what broke.
set -u

LOG_SRC="${1:-}"
USB_SRC="${2:-}"
CORE_SRC="${3:-}"
if [ -z "$LOG_SRC" ] || [ ! -f "$LOG_SRC" ]; then
    echo "FAIL: no Log.h given (usage: bash $0 <Log.h> <AppUSB.h> [AppInterface.h])" >&2
    exit 1
fi
if [ -z "$USB_SRC" ] || [ ! -f "$USB_SRC" ]; then
    echo "FAIL: no AppUSB.h given (usage: bash $0 <Log.h> <AppUSB.h> [AppInterface.h])" >&2
    exit 1
fi

fail=0
note_log() { echo "FAIL $LOG_SRC: $1"; fail=1; }
note_usb() { echo "FAIL $USB_SRC: $1"; fail=1; }
note_core() { echo "FAIL $CORE_SRC: $1"; fail=1; }

body=$(sed -n '/^void DeviceLog(/,/^}/p' "$LOG_SRC")
[ -n "$body" ] || note_log "DeviceLog() not found - is this the Tamu log header?"

silent=$(sed -n '/^bool AppUsbSilent()/,/^}/p' "$USB_SRC")
[ -n "$silent" ] || note_usb "AppUsbSilent() not found - is this the Tamu USB glue?"

# 1. Presence is not traffic. The old guard dropped every line while a cable was attached.
if printf '%s\n' "$body" | grep -q 'AppInterface\.Connected'; then
    note_log "the console copy is gated on the app being CONNECTED again - that is cable presence, not a busy link (E67)"
fi

# 2. The guard is the named decision, fed with the live link state.
guard_line=$(printf '%s\n' "$body" | grep -n '^    if (' | head -1 | cut -d: -f1)
guard_text=$(printf '%s\n' "$body" | sed -n "${guard_line}p")
if [ -z "$guard_line" ]; then
    note_log "no guard line at the top of DeviceLog - what suppresses the console copy?"
else
    printf '%s\n' "$guard_text" | grep -q 'AppConsoleBusy(' \
        || note_log "the guard does not ask AppConsoleBusy() - the console rule must be the host-tested one"
    printf '%s\n' "$guard_text" | grep -q 'AppUSBActive()' \
        || note_log "the guard does not pass whether the USB link is up (AppUSBActive)"
    printf '%s\n' "$guard_text" | grep -q 'AppUsbSilent()' \
        || note_log "the guard does not pass the idle test (AppUsbSilent) - a talking host must suppress the copy"
    printf '%s\n' "$guard_text" | grep -Eq 'AppInterface\.TxHead.*TxTail' \
        || note_log "the guard does not pass the TX ring's head/tail - a queued frame means a line would split it"
fi

# 3. "Nothing has ever arrived" is idle, not busy: otherwise the first 500 ms of uptime (and the
#    boot lines in it) read as an active session.
printf '%s\n' "$silent" | grep -q 'UsbLastRx == 0' \
    || note_usb "AppUsbSilent() must treat 'no frame yet' as silent, or boot lines get suppressed"
printf '%s\n' "$silent" | grep -q 'USB_TX_SILENCE_MS' \
    || note_usb "AppUsbSilent() must keep the ~500 ms silence window (USB_TX_SILENCE_MS)"

# 4. Order: suppress, then format, then emit - and still exactly one sink.
fmt_line=$(printf '%s\n' "$body" | grep -n 'vsnprintf(' | head -1 | cut -d: -f1)
emit_line=$(printf '%s\n' "$body" | grep -n 'ESP_LOGI(' | head -1 | cut -d: -f1)
if [ -z "$guard_line" ] || [ -z "$fmt_line" ] || [ "$guard_line" -ge "$fmt_line" ]; then
    note_log "the guard must come before the format/emit (a line formatted first is a line written later)"
fi
if [ -z "$emit_line" ] || [ "$guard_line" -ge "$emit_line" ]; then
    note_log "the guard must come before ESP_LOGI"
fi
if [ "$(printf '%s\n' "$body" | grep -c 'ESP_LOG[IWE](')" != 1 ]; then
    note_log "DeviceLog must still have exactly one sink (one ESP_LOGI) - no second output path"
fi

# 5. The named decision has to exist where the device glue can reach it, and carry both halves of
#    "in use" (the talking host and the queued frame). Its truth table is appconsole_test.cpp.
if [ -n "$CORE_SRC" ]; then
    if [ ! -f "$CORE_SRC" ]; then
        note_core "not a file - the AppInterface.h holding AppConsoleBusy()"
    else
        busy=$(sed -n '/^inline bool AppConsoleBusy(/,/^}/p' "$CORE_SRC")
        if [ -z "$busy" ]; then
            note_core "AppConsoleBusy() is not defined here (Log.h calls it)"
        else
            printf '%s\n' "$busy" | grep -q '!UsbSilent' \
                || note_core "AppConsoleBusy() must suppress while the host is talking (the !UsbSilent half)"
            printf '%s\n' "$busy" | grep -Eq 'TxHead.*!=.*TxTail' \
                || note_core "AppConsoleBusy() must count a queued TX frame as in use (TxHead != TxTail)"
        fi
    fi
fi

if [ "$fail" = 0 ]; then
    echo "PASS: the console copy is suppressed by a busy USB link (a talking host or a queued frame), never by cable presence"
fi
exit "$fail"

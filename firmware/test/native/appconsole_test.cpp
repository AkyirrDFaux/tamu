// Host-side test for the console copy's guard (Core/Functions/AppConsole.h: AppConsoleBusy).
//
// Issues.md E67 is the reason this file exists. Devices/Tamu_v2.0A/Log.h used to return as soon as
// `AppInterface.Connected` was true, and `AppUSBActive()` is `usb_serial_jtag_is_connected()` -
// true ~0.4 s after a reset whenever a host merely HOLDS the port, not only while a process speaks
// the protocol. A rig session therefore read the boot banner and nothing after it, which is why
// E65/E66/E69 had to be pinned by timing and register reads instead of by the device's own lines.
//
// The owner ruled route (d) on 2026-10-28 ("D is sufficient"): keep the console copy while the USB
// link is IDLE, and suppress it only while the stream is in use, because that stream is also the
// app link's byte stream and a line written mid-frame corrupts the frame it lands in.
//
// "In use" is the decision driven here, in three parts:
//   1. no host  -> the stream is nobody's; nothing can be corrupted (this is the boot banner's case,
//      and the first 500 ms of uptime, where the idle test says "silent" because nothing has
//      arrived yet);
//   2. a host that TALKS (a frame arrived inside the ~500 ms idle window) -> suppressed: its reply
//      is in flight or expected;
//   3. a host that is SILENT but still OWED bytes (a frame sits in the TX ring, which the pump hands
//      out 60 B at a time) -> suppressed too: the line would land between two chunks of a frame that
//      is being written right now. That is not only a reply - an unsolicited push (a subscription
//      update replayed to the app) needs no request to be in flight either.
// Everything else - cable attached, host silent, nothing queued - writes the line. That is the fix.
//
// The wire format, the LOG_ codes and the block layouts are not involved: this is one guard. The
// caller (Log.h) cannot be linked here (esp_log.h, the USJ driver), so run.sh asserts its shape on
// the source (test/native/log_guard.sh) - it must ask this predicate, and it must still have exactly
// one sink.

#include <cstdint>
#include <cstdio>

#include "Core/Functions/AppConsole.h"

static int checks = 0, failures = 0;

#define CHECK(cond)                                                                  \
    do {                                                                             \
        checks++;                                                                    \
        if (!(cond)) {                                                               \
            failures++;                                                              \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                   \
        }                                                                            \
    } while (0)

// The three inputs spelled out as the states the rig actually produces.
static const bool UsbUp = true;
static const bool UsbGone = false;
static const bool Silent = true;    // no app frame for ~500 ms (or none ever)
static const bool Talking = false;  // a frame arrived within the window
static const uint16_t RingEmptyHead = 0x1234;
static const uint16_t RingEmptyTail = RingEmptyHead;
static const uint16_t RingQueuedHead = 0x1238;  // 4 bytes queued: head walked past tail
static const uint16_t RingQueuedTail = 0x1234;

// The pre-fix expression, kept to show what the ruling changed - not what the code does.
static bool PrevPresenceOnlyGuard(bool UsbActive) { return UsbActive; }

static void TestNoHostIsNobodysStream()
{
    // Before the port is open (and while nothing has ever arrived) the line is written, as it was
    // before: the boot banner's path, and the reason AppUsbSilent() calls "never received" idle.
    CHECK(!AppConsoleBusy(UsbGone, Silent, RingEmptyHead, RingEmptyTail));
    CHECK(!AppConsoleBusy(UsbGone, Talking, RingEmptyHead, RingEmptyTail));
    CHECK(!AppConsoleBusy(UsbGone, Silent, RingQueuedHead, RingQueuedTail));
    // Nothing goes out to an absent host, so a queued frame is not "in use" either.
    CHECK(!AppConsoleBusy(UsbGone, Talking, RingQueuedHead, RingQueuedTail));
}

static void TestSilentHostWritesTheLine()
{
    // THE E67 CASE: cable attached, host connected but idle -> the console copy is emitted while
    // nothing is in flight. This is the line a rig capture needs, and the pre-fix guard dropped it.
    CHECK(!AppConsoleBusy(UsbUp, Silent, RingEmptyHead, RingEmptyTail));
    // The old guard suppressed exactly this state, which is the regression this pins.
    CHECK(PrevPresenceOnlyGuard(UsbUp));
}

static void TestTalkingHostSuppresses()
{
    // A frame arrived inside the idle window: its reply is in flight or about to be written, and a
    // line landing between two chunks of it would split it. Suppressed, as before and as required.
    CHECK(AppConsoleBusy(UsbUp, Talking, RingEmptyHead, RingEmptyTail));
    CHECK(AppConsoleBusy(UsbUp, Talking, RingQueuedHead, RingQueuedTail));
}

static void TestQueuedFrameSuppressesEvenWhenSilent()
{
    // The host has been quiet for 500 ms but bytes it is owed are still in the ring, so a frame is
    // being written out right now - an unsolicited push, or the tail of a reply. A line here splits
    // the frame, so silence is not enough on its own.
    CHECK(AppConsoleBusy(UsbUp, Silent, RingQueuedHead, RingQueuedTail));
    CHECK(AppConsoleBusy(UsbUp, Silent, (uint16_t)0x0002, (uint16_t)0xFFFE));  // wrapped counter
}

static void TestRingEmptinessIsEqualityNotOrder()
{
    // Head/tail are free-running uint16 counters, so only equality means empty: after a wrap the
    // head can be numerically SMALLER than the tail and the ring still holds bytes.
    CHECK(!AppConsoleBusy(UsbUp, Silent, (uint16_t)0x0003, (uint16_t)0x0003));
    CHECK(AppConsoleBusy(UsbUp, Silent, (uint16_t)0x0003, (uint16_t)0xFFFE));
    CHECK(AppConsoleBusy(UsbUp, Silent, (uint16_t)0xFFFE, (uint16_t)0x0003));
}

static void TestTheWholeTable()
{
    // Exhaustive over the three inputs (the ring reduced to empty/queued): the decision is a
    // predicate of the link state only, never of a cached "an app is connected" flag.
    const bool States[] = {UsbGone, UsbUp};
    const bool Silences[] = {Silent, Talking};
    const bool Queued[] = {false, true};
    int Writing = 0, Suppressed = 0;
    for (bool Usb : States)
        for (bool Silence : Silences)
            for (bool Q : Queued)
            {
                uint16_t Head = Q ? RingQueuedHead : RingEmptyHead;
                uint16_t Tail = Q ? RingQueuedTail : RingEmptyTail;
                bool Busy = AppConsoleBusy(Usb, Silence, Head, Tail);
                // The line is written exactly when there is a host AND nothing is in flight on it.
                CHECK(Busy == (Usb && (!Silence || Q)));
                Writing += Busy ? 0 : 1;
                Suppressed += Busy ? 1 : 0;
            }
    // 5 of the 8 states write the line (the four with no host, plus the ruled case: cable attached,
    // host silent, nothing queued), 3 suppress it (host up and busy).
    CHECK(Writing == 5);
    CHECK(Suppressed == 3);
    // ... and the ruled case is one of the writing ones.
    CHECK(!AppConsoleBusy(UsbUp, Silent, RingEmptyHead, RingEmptyTail));
}

int main()
{
    TestNoHostIsNobodysStream();
    TestSilentHostWritesTheLine();
    TestTalkingHostSuppresses();
    TestQueuedFrameSuppressesEvenWhenSilent();
    TestRingEmptinessIsEqualityNotOrder();
    TestTheWholeTable();

    printf("app console guard: %d checks, %d failure(s)\n", checks, failures);
    return failures == 0 ? 0 : 1;
}

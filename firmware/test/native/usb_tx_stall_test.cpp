// Host-side test for the USB TX stall detector (Blocks/UsbTxStall.h).
//
// The defect: `usb_serial_jtag_is_connected()` is a SOF test, so a host that holds the port open and
// reads nothing keeps it true. The pump then offers the port bytes that sit in the driver's 2 KB ring
// for ever, and every pass waited out its write timeout. The tracker answers the only question that
// distinguishes "idle host" from "port not draining": did a pass go by in which the port took
// NOTHING? After USB_TX_STALL_MS of that, the pump purges the ring once, logs once, and skips USB
// until the host sends a frame (proof it is reading).
//
// The purge-once property is the one that must not regress: purging every pass would drop a reply
// that arrived while the port was slow, and logging every pass would flood the console.
//
// No device dependency: built like align/stride (see run.sh).

#include <cstdio>

#include "Blocks/UsbTxStall.h"

#include "check.h"

// A port that takes nothing: the clock starts on the first empty pass and the episode is reported
// exactly once, USB_TX_STALL_MS later.
static void TestSilentPortStallsOncePerEpisode()
{
    UsbTxStall T;
    T.Note(0, 1000);
    CHECK(!T.Stalled(1000));
    CHECK(!T.TakeReport(1000));

    // Every pass in between: not yet.
    CHECK(!T.Stalled(1000 + USB_TX_STALL_MS - 1));
    CHECK(!T.TakeReport(1000 + USB_TX_STALL_MS - 1));

    // The threshold: stalled, and the report is taken once.
    CHECK(T.Stalled(1000 + USB_TX_STALL_MS));
    CHECK(T.TakeReport(1000 + USB_TX_STALL_MS));
    CHECK(T.Episodes == 1);
    CHECK(T.Stalled(2000 + USB_TX_STALL_MS));       // still stalled
    CHECK(!T.TakeReport(2000 + USB_TX_STALL_MS));   // ... but not reported again
    CHECK(T.Episodes == 1);

    // More empty passes do not restart the clock: the episode continues.
    T.Note(0, 5000);
    CHECK(!T.TakeReport(5000));
    CHECK(T.Episodes == 1);
}

// A port that takes bytes: nothing stalls, and a progress pass in the middle of a stall clears it -
// a reading host must never be mistaken for a dead one.
static void TestProgressClears()
{
    UsbTxStall T;
    T.Note(60, 1000);
    CHECK(!T.Stalled(1000));
    CHECK(!T.TakeReport(1000));
    CHECK(T.Episodes == 0);

    // Stall starts, then one frame goes through.
    T.Note(0, 1000);
    CHECK(T.Stalled(1000 + USB_TX_STALL_MS));
    T.Note(60, 1000 + USB_TX_STALL_MS);
    CHECK(!T.Stalled(1000 + USB_TX_STALL_MS));
    CHECK(!T.TakeReport(1000 + USB_TX_STALL_MS));

    // A partial write is still progress (the ring is draining, just slowly).
    T.Note(12, 2000);
    CHECK(!T.Stalled(2000 + 10 * USB_TX_STALL_MS));
}

// The host speaking again (AppRxStream calls Clear) restarts the whole thing: a later silence is a
// new episode and is reported again. The clock also has to work when it starts at uptime 0, which a
// "0 means not running" sentinel would have broken.
static void TestClearStartsANewEpisode()
{
    UsbTxStall T;
    T.Note(0, 0); // an empty pass at t = 0
    CHECK(T.Stalled(USB_TX_STALL_MS));
    CHECK(T.TakeReport(USB_TX_STALL_MS));
    CHECK(T.Episodes == 1);

    T.Clear(); // the host sent a frame: it is reading again
    CHECK(!T.Stalled(2000));
    CHECK(!T.TakeReport(2000));

    // A later silence is a new episode, and is reported again.
    T.Note(0, 3000);
    CHECK(T.TakeReport(3000 + USB_TX_STALL_MS));
    CHECK(T.Episodes == 2);
}

int main()
{
    TestSilentPortStallsOncePerEpisode();
    TestProgressClears();
    TestClearStartsANewEpisode();

    printf("usb tx stall: %d checks passed", checks - failures);
    if (failures != 0)
        printf(", %d FAILED", failures);
    printf("\n");
    return failures == 0 ? 0 : 1;
}

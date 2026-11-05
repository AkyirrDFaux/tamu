// Host-side test for the app-link TX ring's geometry (Core/Functions/TxRing.h).
//
// Issues.md E75 is the reason this file exists. Growing the ring from 8 KiB to 24 KiB made
// `TxPeek` read ring bytes 16384 positions away from where `Send` had written them: it indexed
// with `(TxTail + Count) % APP_TX_RING_SIZE`, an `int` sum that does not wrap when the 16-bit
// counter does, while the writer used `TxHead % APP_TX_RING_SIZE` on the uint16 counter. 65536 is
// not a multiple of 24576 (65536 % 24576 == 16384), so every ~3 whole-file replies - a `.TABLE`
// reply is 22,528 B, so the counter crosses 65536 about every third one - a peek that straddled
// the wrap handed out stale ring content: whole frames of EARLIER fragments where later ones
// belonged, with the byte count and the STOP frame intact. Measured on the rig as 4 of 10 reads.
//
// The 8 KiB ring divides 65536 exactly, which is why the defect only appeared after the growth.
//
// This test links the real header and models the ring the way AppInterface.h drives it: one
// writer (`Send`, 88-byte fragment frames into the ring when the room check allows), one reader
// (`TxPeek`/`TxCommit` in 480-byte notifications or 60-byte USB chunks), free-running uint16
// counters. It asserts the delivered byte stream equals what was written, across the counter
// wrap - and it runs the same model with the pre-fix expression to pin what went wrong.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "Core/Functions/TxRing.h"

static int checks = 0, failures = 0;

#define CHECK(cond)                                                                  \
    do {                                                                             \
        checks++;                                                                    \
        if (!(cond)) {                                                               \
            failures++;                                                              \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                   \
        }                                                                            \
    } while (0)

// The reply the rig serves: 256 fragments of 88 B on the wire.
static const size_t ReplyBytes = 22528;
static const size_t FrameBytes = 88;

// The ring's reader/writer pair, with the position rule as a parameter so the same model can be
// run with the pre-fix expression (E75) and with AppTxRingPos (the fix).
typedef uint16_t (*PosFn)(uint32_t Counter);

static uint16_t PreFixPos(uint32_t Counter)
{
    return (uint16_t)(Counter % APP_TX_RING_SIZE); // `(TxTail + Count) % SIZE`: int sum, no wrap
}

static uint16_t FixedPos(uint32_t Counter) { return AppTxRingPos((uint16_t)Counter); }

// Runs `replies` whole replies back to back through one ring, starting with the counters at
// `Start`. Returns true when every reply arrived byte for byte. `FirstDiff` reports the offset of
// the first differing byte in the first broken reply (-1 when intact).
static bool PumpReplies(uint16_t Start, PosFn Pos, size_t Chunk, int Replies, size_t &FirstDiff)
{
    std::vector<uint8_t> ring(APP_TX_RING_SIZE, 0);
    uint16_t Head = Start, Tail = Start;
    FirstDiff = (size_t)-1;
    for (int r = 0; r < Replies; r++)
    {
        // A distinct payload per reply, so a stale value from the previous one is visible.
        std::vector<uint8_t> Src(ReplyBytes);
        for (size_t i = 0; i < ReplyBytes; i++)
            Src[i] = (uint8_t)(i * 7 + r * 13 + 3);
        std::vector<uint8_t> Out;
        size_t Sent = 0;
        for (;;)
        {
            uint16_t Used = (uint16_t)(Head - Tail);
            if (Sent < ReplyBytes && ReplyBytes - Sent >= FrameBytes &&
                FrameBytes <= (size_t)(APP_TX_RING_SIZE - 1 - Used))
            {
                for (size_t i = 0; i < FrameBytes; i++)
                {
                    ring[AppTxRingPos(Head)] = Src[Sent];
                    Head = (uint16_t)(Head + 1);
                    Sent++;
                }
            }
            Used = (uint16_t)(Head - Tail);
            if (Used == 0)
            {
                if (Sent == ReplyBytes)
                    break;
                continue;
            }
            size_t Take = Used > Chunk ? Chunk : Used;
            for (size_t c = 0; c < Take; c++)
                Out.push_back(ring[Pos((uint32_t)Tail + c)]);
            Tail = (uint16_t)(Tail + Take);
        }
        if (Out.size() != ReplyBytes || std::memcmp(Out.data(), Src.data(), ReplyBytes) != 0)
        {
            if (FirstDiff == (size_t)-1 && Out.size() == ReplyBytes)
                for (size_t i = 0; i < ReplyBytes; i++)
                    if (Out[i] != Src[i])
                    {
                        FirstDiff = i;
                        break;
                    }
            return false;
        }
    }
    return true;
}

// A whole reply survives the counters wrapping at 65536, for both link chunk sizes and for the
// counter positions a rig session actually passes through (the crossing happens every ~3 reads).
static void TestReplySurvivesTheCounterWrap()
{
    const uint16_t Starts[] = {0, 60000, 63000, 65000, 65500, 0xFFFF - 87};
    const size_t Chunks[] = {480, 60}; // BLE notification, USB stream chunk
    for (uint16_t Start : Starts)
        for (size_t Chunk : Chunks)
        {
            size_t Diff = 0;
            CHECK(PumpReplies(Start, FixedPos, Chunk, 12, Diff));
        }
}

// The pre-fix expression is what the rig measured: same byte count, wrong bytes. Pinned here so
// the helper is not "simplified" back to an int sum later.
static void TestTheWiderSumWasTheDefect()
{
    size_t Diff = 0;
    const bool Ok = PumpReplies(0xFFFF - 200, PreFixPos, 480, 4, Diff);
    CHECK(!Ok);
    CHECK(Diff != (size_t)-1);
    printf("   pre-fix expression: a reply first differs %zu B in (whole file is %zu B)\n", Diff,
           ReplyBytes);
}

// The geometry fact behind it: a size that divides 65536 (the old 8 KiB) cannot skip, the 24 KiB
// one does skip 8192 positions whenever the counter wraps. Both reader and writer must therefore
// use the same 16-bit rule - which is all AppTxRingPos is.
static void TestGeometry()
{
    static_assert(65536u % 8192u == 0, "the ring this replaced was seamless at the wrap");
    CHECK(65536u % APP_TX_RING_SIZE != 0);
    CHECK((uint16_t)(65536u % APP_TX_RING_SIZE) == 16384);
    // Positions around the wrap: the writer's counter wraps with it, so the two agree.
    const uint16_t Before = AppTxRingPos((uint16_t)(0xFFFF - 1));
    const uint16_t At = AppTxRingPos(0xFFFF);
    const uint16_t After = AppTxRingPos(0);
    CHECK(Before == 16382);
    CHECK(At == 16383);
    CHECK(After == 0);
    // The pre-fix rule on the same counters: it keeps counting past the wrap instead of wrapping.
    CHECK(PreFixPos((uint32_t)0xFFFF + 1) == 16384);
    CHECK(PreFixPos((uint32_t)0xFFFF + 1) != AppTxRingPos(0));
}

int main()
{
    TestGeometry();
    TestReplySurvivesTheCounterWrap();
    TestTheWiderSumWasTheDefect();
    printf("== TX ring geometry tests (E75): %d checks, %d failures ==\n", checks, failures);
    return failures == 0 ? 0 : 1;
}

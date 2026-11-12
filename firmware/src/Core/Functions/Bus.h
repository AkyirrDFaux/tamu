#pragma once

#include "Core/Functions/Packet.h"
#include "Core/Functions/BusDiag.h" // rejected-frame records, shared with the BUSGATE dump
#include "Core/Types/Number.h" // RawRand() for CSMA backoff

// Shared RS-485 bus receive assembler (Core/Functions/Bus.h).
//
// Both the Tamu and the DAS previously kept byte-identical copies of this state
// machine in their own RSBus.h. It only differs in HOW a byte is pulled from the
// UART, so the machine lives here and each device supplies a tiny byte-source
// callback. The assembly state is static per template instantiation, so every
// device gets its own independent machine.

// CSMA/CD timings at 460800 baud (1 byte = ~21.7us) and the shared retry/silence
// constants (Docs/RSBus and Packets.md: silence = 8 + priority/8 + random bytes).
#define RS485_RETRIES 3
#define RS485_BYTE_TIME_US     22
#define RS485_SILENCE_BYTES    8
#define RS485_SILENCE_TIMEOUT_MS 100

// One whole frame on the line: sync byte (1) + header (12) + the longest payload (116) = 129 bytes
// x 10 bit times / 460800 baud = 2.80 ms. This is the unit the retry backoff is built on.
#define RS485_FRAME_US 2800
// The same frame in whole milliseconds, rounded up: the backoff is expressed in ms because BOTH
// targets sleep in milliseconds (the core's scheduler sleeps a tick, the node's `Sleep()` is a
// SysTick ms wait), so a sub-millisecond figure could never be honoured by either.
#define RS485_FRAME_MS ((RS485_FRAME_US + 999) / 1000)

// The retry backoff, in milliseconds (cards t_763b4cde / t_d7802076).
//
// The original expression was `(RawRand() % 16) + 1` ms, and it was a lockstep generator rather
// than jitter:
//   * RANGE - 1..16 ms against a 2.80 ms frame: a 1 ms draw is under half a frame, which the
//     silence window afterwards (174-264 us) absorbs, so the retry could re-enter the collision it
//     was backing off from.
//   * PHASE - RawRand() is the LCG s' = 13s + 9 (mod 16) in its low nibble, a cycle of length
//     exactly 16 (14,15,12,5,10,11,8,1,6,7,4,13,2,3,0,9 repeating). Two nodes drawing at the same
//     call index re-drew the SAME delay on attempts 2 and 3, so a collision repeated in lockstep.
//
// The first replacement (t_763b4cde) grouped the delay as `(Addr * 5 + Attempt) & 7` and was still
// a lockstep generator, only with a longer period: the group read the address only through
// `Addr mod 8` and the sub term through `Addr mod 4`, so two nodes whose addresses differ by a
// multiple of 8 - 0.1 and 0.9 on one net, any two nets' node 1 - drew an identical group AND an
// identical sub term, and stayed in phase for ever. (Measured: the degenerate set was exactly the
// multiples of 8, the smallest 8.)
//
// The delay is now a 5-bit ADDRESS PHASE XORed with a draw:
//     phase = (Addr & 31) ^ ((Addr >> 5) & 31) ^ ((Addr >> 10) & 31)   -- all 16 address bits
//     draw  = ((Rand >> 16) & 31) ^ Attempt
//     delay = 2 + (phase ^ draw) ms                                    = 2 .. 33 ms
//
// * Why the pair separates within ONE attempt. XOR is a bijection, so two nodes that are still in
//   phase (same PRNG state and attempt, hence the same draw) keep exactly the difference their
//   ADDRESSES give them: the delays are equal only when phase(A) == phase(B). The phase is linear
//   over GF(2), so phase(A) ^ phase(B) == phase(A ^ B) and the fold's KERNEL is the whole story.
//   Over the 16-bit address space the kernel holds 2047 of the 65535 differences and its smallest
//   member is 33, so any two addresses whose XOR difference is 33 or less always draw different
//   delays - for every draw and every attempt. Inside one net (device IDs 0..1023) the degenerate
//   differences are exactly the multiples of 33, the smallest pair being node 1 and node 32
//   (1 ^ 32 = 33). A bus whose node numbers sit in one 32-address block - every real one; SNDB
//   reserves 1 and allocates from 2 upwards - cannot produce the same delay from two nodes at all.
//   Two nodes that are NOT in phase (the core seeds from esp_random, the DAS from SysTick at boot)
//   land on the same slot with probability 1/32 per attempt - 1/1024 over both retries.
// * The draw reads the TOP half of the PRNG state, whose period is the full 2^32, so the 16-step
//   low nibble never reaches the delay; XORing it over the phase moves the value uniformly over all
//   32 slots (every slot is reachable from every address) without disturbing the difference above.
// * Numbers. One frame is 129 bytes x 10 bit times / 460800 = 2.800 ms. The window is 2..33 ms =
//   0.71 .. 11.8 frames, chosen in whole milliseconds because both targets sleep in ms (the core's
//   scheduler tick, the node's `Sleep`). A separated pair is at least 1 ms apart = 0.36 frame =
//   3.8x the longest silence window (8 + 15/8 + 3 = 12 byte times = 264 us), so the earlier node is
//   still driving the line when the later one's silence wait ends; two independent draws differ by
//   10.7 ms on average, ((32^2 - 1) / (3 * 32)), i.e. 3.8 frames.
// * Budget. The window is UNCHANGED (2..33 ms), so RS485_TX_STINT_MS' worst case does not move: a
//   send draws at most 3 x 33 = 99 ms of backoff, and the 2 ms floor cannot starve a busy bus -
//   waiting for the line is the silence wait's job, not the backoff's.
//
// Deliberately NOT used: a real timer or a per-node seed. Either would also decorrelate two nodes,
// but a per-node term inside the draw lets it CANCEL the 1 ms address separation (equality becomes
// 1-in-32 for every pair instead of impossible), and the DAS cannot seed from its own address at
// boot anyway - `ShortAddress` stays 0 until the discover loop answers (Devices/DAS_v0.1/Main.h),
// so nothing device-unique exists before the first backoff is drawn.
inline uint8_t RS485BackoffMs(uint16_t Addr, uint8_t Attempt, uint32_t Rand)
{
    const uint32_t Phase = ((uint32_t)Addr & 0x1Fu) ^ (((uint32_t)Addr >> 5) & 0x1Fu)
                         ^ (((uint32_t)Addr >> 10) & 0x1Fu);
    const uint32_t Draw = ((Rand >> 16) & 0x1Fu) ^ (uint32_t)Attempt;
    return (uint8_t)(2u + ((Phase ^ Draw) & 0x1Fu));
}

// The transmit stint (cards t_4c9c5fa8 / t_45490cbe).
//
// One send is 1..RS485_RETRIES attempts on the device's MAIN task - on the core the same task that
// runs AppInterface.Pump, the display, the scripts and the radios - so every ms it blocks is a ms
// the app link is not served. Before this it could hold that task for ~770 ms: 3 attempts x
// (100 ms silence + 20 ms sync echo + 20 ms first-chunk echo + 100 ms tx-done + up to 16 ms
// backoff), and the app was mid-transaction. Three things bound it:
//
//   * ONE deadline for the whole send (RS485_TX_STINT_MS): an attempt that starts after it never
//     runs, and each attempt's silence window is clipped to what is left of it, so a continuously
//     busy or shorted bus costs the task the stint and not three full attempts on top of it;
//   * the echo and tx-done windows sized from the LINE: a 16-byte chunk at 460800 is 0.35 ms and
//     the whole longest frame is 2.80 ms (RS485_FRAME_US), so 10 ms and 20 ms are 3.5x and 7x a
//     frame - a missing echo is still detected, just not after 20 ms;
//   * every wait inside the send serves the caller's link work instead of stopping it
//     (RS485AppLinkTick on the core; the DAS has no link to serve), in slices: the silence wait
//     yields every 1 ms, the tx-done poll is RS485_TX_DONE_SLICE_MS, the backoff sleeps in
//     RS485_BACKOFF_SLICE_MS.
//
// Worst case: RS485_TX_STINT_MS + the last attempt's non-wait work = 300 + (2 echo windows +
// tx-done + the largest backoff) = 373 ms. The longest stretch the app link goes unserved is one
// attempt's two echo windows plus the first tx-done poll (2 x RS485_ECHO_TIMEOUT_MS +
// RS485_TX_DONE_SLICE_MS = 22 ms): the echoes are the only blocking calls left in a send, they are
// back to back, and a tick between them would put a gap INSIDE the frame for the receiver.
// test/native/tx_stint_test.cpp measures both figures off the helpers below; the device reports the
// measured stint on the BUSGATE totals line as txstint=.
#define RS485_TX_STINT_MS 300
#define RS485_ECHO_TIMEOUT_MS 10
#define RS485_TX_DONE_TIMEOUT_MS 20
#define RS485_TX_DONE_SLICE_MS 2 // one uart_wait_tx_done() poll inside the tx-done wait
#define RS485_BACKOFF_SLICE_MS 5 // one sleep inside the retry backoff
// RS485BackoffMs draws 2..33 ms (2 + the low 5 bits). Named so the stint's budget is computed from
// the same figure the draw obeys instead of restating it in a comment.
#define RS485_BACKOFF_MAX_MS 33

// No blocking call inside a send may stop the app link for longer than the card allows (50 ms).
static_assert(2 * RS485_ECHO_TIMEOUT_MS + RS485_TX_DONE_SLICE_MS <= 50,
              "the back-to-back echo windows plus one tx-done poll are the longest unserved stretch");
// ... and the send as a whole is the deadline plus one attempt's non-wait work.
static_assert(RS485_TX_STINT_MS + 2 * RS485_ECHO_TIMEOUT_MS + RS485_TX_DONE_TIMEOUT_MS +
                      RS485_BACKOFF_MAX_MS <=
                  400,
              "the transmit stint's worst case outgrew the budget (see Issues E78/E81 neighbours)");

// Waits for the line to be silent for 8 bytes + Priority/8 + random 0-3 bytes, bounded by `CapMs`
// (RS485_SILENCE_TIMEOUT_MS unless the caller is inside a stint that has already spent part of its
// budget) so a continuously-busy or shorted bus cannot wedge the caller.
// Shared by both devices - they only differ in how the line is observed, how time is read and
// how the loop yields (DAS busy-waits, Tamu sleeps 1 ms):
//   active() - takes whatever the UART holds and returns true when the bus was active. A caller
//              that can keep the bytes (the Tamu's keep ring) MUST: a reply that is still arriving
//              has to survive the wait and reach the assembler. The old contract here was
//              "drain", and draining is what ate the node's answer and made a restore report a
//              silent node (card t_4c9c5fa8). The DAS still drains: its 2048 B of RAM has no room
//              for a keep ring and its echo check reads the same ring, so that half is not done.
//   now_us() - a microsecond clock (may wrap; only short spans are compared).
//   yield()  - a short cooperative pause for the tight retry loop.
template <typename ActiveFn, typename NowUs, typename YieldFn>
static void RS485WaitForSilence(uint8_t Priority, ActiveFn Active, NowUs NowUsCb, YieldFn YieldCb,
                               uint32_t CapMs = RS485_SILENCE_TIMEOUT_MS)
{
    uint32_t SilenceUs = (RS485_SILENCE_BYTES + (Priority / 8) + (RawRand() % 4)) * RS485_BYTE_TIME_US;
    uint32_t IdleSince = NowUsCb();
    uint32_t WaitStart = IdleSince;

    for (;;)
    {
        if (Active())
            IdleSince = NowUsCb(); // bus activity: restart the silence window
        if ((NowUsCb() - IdleSince) >= SilenceUs)
            return;
        if ((NowUsCb() - WaitStart) >= CapMs * 1000u)
            return; // Give up: transmit into the best window we had
        YieldCb();
    }
}

// Waits that keep the caller's link alive (card t_45490cbe).
//
// The retry backoff was one vTaskDelay() and the tx-done wait one uart_wait_tx_done() for its whole
// timeout (100 ms before that window was sized from the line), and either one stopped the whole
// task - the app link is mid-transaction during a send. They are loops here, with the chip-specific
// calls injected - the same shape as RS485WaitForSilence above, which is what lets
// test/native/tx_stint_test.cpp drive the real code on the host with a virtual clock instead of
// checking a comment:
//   now_us()   - a microsecond clock (may wrap; only short spans are compared).
//   tick()     - the link work: the core passes RS485AppLinkTick, which moves queued reply bytes
//                and ticks the radios and deliberately dispatches NOTHING (the send that is running
//                owns the wire; a dispatch could start a second transmit).
//   sleep_ms() - a short pause in whole milliseconds.
//
// `SliceMs` 0 means the poll itself already blocked (uart_wait_tx_done with a 2 ms timeout), so the
// loop adds no sleep of its own and the pacing is exactly what it was before this card.

// Polls `Done` until it answers true or `TimeoutMs` elapsed, ticking the link after every poll.
template <typename DoneFn, typename NowUs, typename TickFn, typename SleepMsFn>
static bool RS485WaitTicking(DoneFn Done, NowUs NowUsCb, TickFn TickCb, SleepMsFn SleepMsCb,
                             uint32_t TimeoutMs, uint32_t SliceMs)
{
    const uint32_t Start = NowUsCb();
    for (;;)
    {
        if (Done())
            return true;
        TickCb();
        if ((uint32_t)(NowUsCb() - Start) >= TimeoutMs * 1000u)
            return false;
        if (SliceMs != 0)
            SleepMsCb(SliceMs);
    }
}

// Sleeps `Ms`, ticking the link before the sleep and after every slice of it.
template <typename NowUs, typename TickFn, typename SleepMsFn>
static void RS485SleepTicking(NowUs NowUsCb, TickFn TickCb, SleepMsFn SleepMsCb, uint32_t Ms)
{
    const uint32_t Start = NowUsCb();
    for (;;)
    {
        TickCb();
        const int32_t LeftUs = (int32_t)(Ms * 1000u) - (int32_t)(uint32_t)(NowUsCb() - Start);
        if (LeftUs <= 0)
            return;
        SleepMsCb(LeftUs > RS485_BACKOFF_SLICE_MS * 1000
                      ? (uint32_t)RS485_BACKOFF_SLICE_MS
                      : (uint32_t)((LeftUs + 999) / 1000));
    }
}

// Bytes taken off the UART while a transmitter is waiting for the line, kept for the assembler.
//
// A silence wait has to READ the UART: its job is to know whether bytes are still arriving, and
// the driver's ring (1024 B = 22 ms of line at 460800) is far too small to leave a whole reply in
// while the caller also waits out a silence window - a maximally long frame is 129 bytes, and a
// waiting sender that keeps reading is exactly what a busy bus produces. So the bytes are put here
// and ReceivePacket()'s byte source reads this ring FIRST: its bytes are older than anything the
// driver still holds, so frame order is preserved across the two sources.
//
// Host-clean (no UART, no RTOS) so the native tests drive the real type: order across the wrap,
// full detection, and - the point of the whole card - that a byte it cannot hold is REFUSED to
// its caller (which counts it) rather than silently dropped on the floor.
template <uint16_t Size>
struct BusRxKeepRing
{
    static_assert(Size > 0 && Size <= 4000, "a keep ring is a small byte FIFO");
    uint8_t Buf[Size];
    uint16_t Head = 0; // free-running; wrapped on access, so 16-bit subtraction detects full/empty
    uint16_t Tail = 0;

    uint16_t Count() const { return (uint16_t)(Head - Tail); }

    // Returns false when the ring is full: the caller must count that byte as destroyed. The
    // contents already kept are untouched (the older bytes are the older frame).
    bool Push(uint8_t Byte)
    {
        if (Count() >= Size)
            return false;
        Buf[Head % Size] = Byte;
        Head = (uint16_t)(Head + 1);
        return true;
    }

    bool Pop(uint8_t &Byte)
    {
        if (Head == Tail)
            return false;
        Byte = Buf[Tail % Size];
        Tail = (uint16_t)(Tail + 1);
        return true;
    }
};

// CRC + length validation for a fully-assembled frame (RX side).
// CRC covers everything from flags through the end of the payload (the payload_len field is
// the byte count, so the covered length is 11 + PayloadBytes()).
#ifdef TYPE_CORE
// Counted on the core only: it is the number that tells "the node's answer never reached this
// core" from "it reached it corrupted" when an app transaction times out (AppInterface.h's gate
// ring reports it). A node has no room for a diagnostic it cannot show (DAS RAM is 2048/2048 B).
uint32_t BusRxCrcRejects = 0;
// Every byte the RX path had to DROP since boot (card t_4c9c5fa8). Zero here means no received
// byte was ever thrown away, so "the node never answered" cannot be this core eating its answer:
// before this counter existed the silence wait drained replies into a 32-byte scratch array and
// nothing anywhere recorded it. It is reported in the BUSGATE totals line beside rxrejects.
uint32_t BusRxDestroyed = 0;
#endif

inline int RxValidateFrame(PacketFrame *Data)
{
    if (Crc8(&Data->Flags, (uint16_t)(11 + PayloadBytes(*Data))) != Data->Crc8)
    {
#ifdef TYPE_CORE
        BusRxCrcRejects = BusRxCrcRejects + 1;
        // Name the frame the CRC refused: a window that expired with `rej=1` can then be matched
        // by TRID to the request it was answering (Core/Functions/BusDiag.h).
        BusRxRejectNote(Data->IdSrc, Data->Trid, Data->Cmd, (uint16_t)PayloadBytes(*Data));
#else
        (void)Data;
#endif
        return 0; // corrupted: keep scanning for the next 0xAA
    }
    return (int)(1 + 12 + PayloadBytes(*Data));
}

// @param Data Pointer to the struct where the packet will be stored. Assembly writes
//             directly into it, so the caller must pass the same buffer every call
//             (ProcessBus uses a single static frame).
// @param read_byte Callback returning one bus byte; must return false when no byte
//                  is currently available (device-specific UART pull).
// @return Total bytes read if successful (including start byte), 0 while incomplete.
//
// The assembly state persists across calls: a frame split across two ProcessBus()
// polls continues where it left off instead of being consumed and discarded. Bytes
// are only "spent" once; garbage before a 0xAA sync byte is skipped. If a sender
// aborts mid-frame, the stale stage consumes following bytes until length/CRC checks
// fail and the machine falls back to RX_SYNC.
template <typename ReadByte>
int ReceivePacketFrame(PacketFrame *Data, ReadByte ReadByteCb)
{
    if (!Data) return 0;

    enum RxStage : uint8_t { RX_SYNC, RX_HEADER, RX_PAYLOAD };
    static uint8_t Stage = RX_SYNC;
    static uint16_t Got = 0; // bytes of the current stage stored so far

    for (;;)
    {
        uint8_t Byte = 0;
        if (!ReadByteCb(Byte))
            break; // no more bytes available right now

        switch (Stage)
        {
        case RX_SYNC:
            if (Byte == 0xAA)
            {
                Stage = RX_HEADER;
                Got = 0;
            }
            // else: inter-frame garbage, skip
            break;

        case RX_HEADER:
            ((uint8_t *)Data)[Got++] = Byte;
            if (Got < 12)
                break;

            // Header complete: payload_len is the payload byte count.
            // A corrupt length could claim up to 255 bytes, beyond the payload buffer,
            // so reject such frames instead of overflowing.
            if (PayloadBytes(*Data) > MAX_PAYLOAD_SIZE)
            {
                Stage = RX_SYNC;
                Got = 0;
                break;
            }

            Stage = RX_PAYLOAD;
            Got = 0;
            if (Data->PayloadLen == 0)
            {
                // Zero-payload frames finish here.
                Stage = RX_SYNC;
                int Total = RxValidateFrame(Data);
                if (Total > 0)
                {
#ifdef TYPE_CORE
                    BusRxAsmNote(0, 0, 0);
#endif
                    return Total;
                }
            }
            break;

        case RX_PAYLOAD:
            Data->Payload[Got++] = Byte;
            if (Got >= PayloadBytes(*Data))
            {
                Stage = RX_SYNC;
                Got = 0;
                int Total = RxValidateFrame(Data);
                if (Total > 0)
                {
#ifdef TYPE_CORE
                    BusRxAsmNote(0, 0, 0);
#endif
                    return Total;
                }
            }
            break;
        }
    }
#ifdef TYPE_CORE
    // Publish the assembler's state (BusDiag.h): a window that expires while this is holding a
    // frame's head is a truncated answer, not silence.
    BusRxAsmNote(Stage, Got, (Stage == RX_PAYLOAD) ? PayloadBytes(*Data) : 0);
#endif
    return 0; // incomplete: more bytes may arrive later
}
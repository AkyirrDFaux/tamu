#pragma once

// The BLE RX chunk assembler: bytes written to the RX characteristic -> the app packet stream.
//
// Every chunk is a uint16 LE length prefix followed by that many stream bytes
// (Docs/Services/App Interface.md, "BLE packet layout"). BlueZ may deliver one write fragmented
// or coalesced arbitrarily, so an onWrite is NOT a chunk and the assembler cannot assume framing.
//
// Two ways one bad write used to end the session, both silent:
//
//  1. The declared length was never bounded. `Need` took whatever two bytes said, up to 65535, so
//     a single corrupt or foreign prefix swallowed every later byte as "payload" for the rest of
//     the session: each frame then failed CRC and the log only ever said "Rejected malformed app
//     frame (CRC)" - a desync that looks exactly like a broken host.
//  2. A SHORT chunk - the sender declared N and delivered fewer (an app that died mid-write, a
//     write the stack truncated). The next chunk's own 2-byte prefix was then taken as the tail of
//     the previous payload, so the stream was off by one header from then on, for ever.
//
// `Reset()` on connect/disconnect does not help either: the desync happens INSIDE one session, and
// resetting the packet-stream parser (WireStreamParser) does not realign the assembler - the two
// are different layers, which is why resetting the parser looked like it should fix this and did
// not.
//
// The answer is a bound and a clock. A declared length the link cannot carry (more than the
// negotiated MTU minus the ATT header and our prefix, never more than the absolute ceiling) is
// refused, and a chunk that stops arriving is abandoned after BLE_RX_CHUNK_TIMEOUT_MS. Both drop
// the partial bytes, COUNT the resync (Resyncs -> the caller's diagnostics line) and let the byte
// that triggered it start a fresh prefix - which is what makes "the next frame parses" true.
//
// The clock is only consulted when a byte arrives, from the caller's TimeFromBoot(): no timer, and
// no work at all on an idle link.
//
// Host-clean (no NimBLE, no FreeRTOS, no logging): AppBLE.h owns the instance and the log lines,
// test/native/ble_rx_test.cpp drives this table.

#include <cstdint>

// The absolute ceiling on one chunk's declared stream length: 512 is the largest ATT MTU the
// protocol allows, minus the 3-byte ATT header, minus our 2-byte length prefix. Nothing that
// arrives over ATT can declare more, whatever the negotiated MTU - so this bound cannot be stale.
#define BLE_STREAM_CHUNK_ABS_MAX 507

// A chunk that stops arriving is dead after this long. The app writes its chunks back-to-back
// (microseconds apart) inside one request, so a gap of a full second in the middle of a chunk is
// not a slow host - it is a chunk that will never be finished.
#define BLE_RX_CHUNK_TIMEOUT_MS 1000

// The largest chunk a negotiated MTU can carry. `Mtu <= 5` means the MTU is not known yet
// (ATT default 23, a server that has not negotiated), in which case only the absolute ceiling
// applies: a tighter bound derived from a stale low reading would refuse legitimate writes.
inline uint16_t BleMaxChunkForMtu(uint16_t Mtu)
{
    if (Mtu <= 5)
        return BLE_STREAM_CHUNK_ABS_MAX;
    const uint16_t Fit = (uint16_t)(Mtu - 3 - 2);
    return Fit < BLE_STREAM_CHUNK_ABS_MAX ? Fit : BLE_STREAM_CHUNK_ABS_MAX;
}

// What one fed byte produced.
struct BleFeedResult
{
    bool Stream;   // `Out` holds a stream byte for the parser
    bool Resynced; // the in-flight chunk was abandoned; the caller must reset the parser with it
};

struct BleRxAssembler
{
    uint16_t Need      = 0;  // stream bytes of the current chunk
    uint16_t Got       = 0;  // stream bytes accumulated so far (payload), or prefix bytes read
    uint16_t LenBuf[2] = {0, 0};
    bool     HaveLen   = false;       // the prefix is complete, `Need` is known
    bool     InChunk    = false;      // a chunk has started and not finished: its clock is running
    uint32_t StartedMs  = 0;
    uint16_t MaxNeed    = BLE_STREAM_CHUNK_ABS_MAX; // the negotiated link's bound
    uint32_t Resyncs    = 0;          // chunks abandoned, of either kind
    uint32_t Overruns   = 0;          // ... refused because the declared length cannot be carried
    uint32_t Truncations = 0;         // ... abandoned because they stopped arriving

    // Clears the in-flight chunk state. Called on connect/disconnect and by the radio reset: a
    // session that ends mid-chunk would otherwise leave the state set for the next one.
    inline void Reset()
    {
        Need    = 0;
        Got     = 0;
        HaveLen = false;
        InChunk = false;
    }

    // Abandons the in-flight chunk. `Kind` names which counter to move.
    inline void Abandon(BleFeedResult &R, uint32_t &Kind)
    {
        Reset();
        Resyncs = Resyncs + 1;
        Kind    = Kind + 1;
        R.Resynced = true;
    }

    // Feeds one raw byte. Returns whether a stream byte should go into the RX parser, and whether
    // the chunk in flight had to be abandoned (the caller then resets the parser too).
    inline BleFeedResult Feed(uint8_t Byte, uint32_t NowMs, uint8_t *Out)
    {
        BleFeedResult R{false, false};

        // The clock first: a stale chunk must not be allowed to eat this byte as its payload.
        if (InChunk && (uint32_t)(NowMs - StartedMs) > BLE_RX_CHUNK_TIMEOUT_MS)
            Abandon(R, Truncations);

        if (!HaveLen)
        {
            if (!InChunk)
            {
                InChunk   = true;
                StartedMs = NowMs;
            }
            LenBuf[Got++] = Byte;
            if (Got < 2)
                return R; // prefix pending: never stream data

            const uint16_t Declared = (uint16_t)(LenBuf[0] | (LenBuf[1] << 8));
            Got = 0;
            if (Declared == 0)
            {
                // Nothing to wait for: the next byte is a prefix again.
                InChunk = false;
                return R;
            }
            if (Declared > MaxNeed)
            {
                // This link cannot carry it, so it is not a length - it is a desync.
                Abandon(R, Overruns);
                return R;
            }
            Need    = Declared;
            HaveLen = true;
            return R;
        }

        *Out       = Byte;
        R.Stream   = true;
        if (++Got >= Need)
        {
            Got     = 0;
            Need    = 0;
            HaveLen = false;
            InChunk = false;
        }
        return R;
    }
};

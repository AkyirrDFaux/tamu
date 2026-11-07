#pragma once

#include "Core/Functions/Packet.h"
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
                if (Total > 0) return Total;
            }
            break;

        case RX_PAYLOAD:
            Data->Payload[Got++] = Byte;
            if (Got >= PayloadBytes(*Data))
            {
                Stage = RX_SYNC;
                Got = 0;
                int Total = RxValidateFrame(Data);
                if (Total > 0) return Total;
            }
            break;
        }
    }
    return 0; // incomplete: more bytes may arrive later
}
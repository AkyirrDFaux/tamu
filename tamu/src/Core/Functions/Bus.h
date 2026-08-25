#pragma once

#include "Core/Functions/Packet.h"

// Shared RS-485 bus receive assembler (Core/Functions/Bus.h).
//
// Both the Tamu and the DAS previously kept byte-identical copies of this state
// machine in their own RSBus.h. It only differs in HOW a byte is pulled from the
// UART, so the machine lives here and each device supplies a tiny byte-source
// callback. The assembly state is static per template instantiation, so every
// device gets its own independent machine.

// CRC + length validation for a fully-assembled frame (RX side).
// CRC covers everything from flags through the end of the payload.
inline int RxValidateFrame(PacketFrame *Data)
{
    if (Crc8(&Data->flags, (uint16_t)(11 + Data->payload_len)) != Data->crc8)
        return 0; // corrupted: keep scanning for the next 0xAA
    return (int)(1 + 12 + Data->payload_len);
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
int ReceivePacketFrame(PacketFrame *Data, ReadByte read_byte)
{
    if (!Data) return 0;

    enum RxStage : uint8_t { RX_SYNC, RX_HEADER, RX_PAYLOAD };
    static uint8_t stage = RX_SYNC;
    static uint16_t got = 0; // bytes of the current stage stored so far

    for (;;)
    {
        uint8_t b = 0;
        if (!read_byte(b))
            break; // no more bytes available right now

        switch (stage)
        {
        case RX_SYNC:
            if (b == 0xAA)
            {
                stage = RX_HEADER;
                got = 0;
            }
            // else: inter-frame garbage, skip
            break;

        case RX_HEADER:
            ((uint8_t *)Data)[got++] = b;
            if (got < 12)
                break;

            // Header complete: payload_len is a single byte (<=255) and the payload
            // buffer holds 256, so no length guard is needed - proceed to the payload
            // stage (CRC validates the frame on completion).
            stage = RX_PAYLOAD;
            got = 0;
            if (Data->payload_len == 0)
            {
                // Zero-payload frames finish here.
                stage = RX_SYNC;
                int total = RxValidateFrame(Data);
                if (total > 0) return total;
            }
            break;

        case RX_PAYLOAD:
            Data->payload[got++] = b;
            if (got >= Data->payload_len)
            {
                stage = RX_SYNC;
                got = 0;
                int total = RxValidateFrame(Data);
                if (total > 0) return total;
            }
            break;
        }
    }
    return 0; // incomplete: more bytes may arrive later
}
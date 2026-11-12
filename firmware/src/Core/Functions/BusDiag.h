#pragma once

// Diagnostic records owned by the bus RX path and printed by the BUSGATE dump.
//
// It exists as its own header for one mechanical reason: the RX path lives in Bus.h, the dump
// lives in AppInterface.h, and AppInterface.h is parsed BEFORE Bus.h in every TU (the device
// header includes it first), so the two cannot see each other's types. This header is
// self-contained and is included by both sides.
//
// WHY IT IS NEEDED (card t_82c3dc59): a `Register` write that the app reports as "no reply" is
// not the same event as a node that never spoke. The gate ring's `rx=` says the line carried
// bytes; only the frame's own header says whether it was the answer, arriving corrupted. The
// reject ring keeps (source, TRID, CMD, length) of the frames the CRC refused - so a window that
// expired with `rej=1` can be matched, by TRID, to the request it was answering.
//
// Core only: a node has no room for a diagnostic it cannot show (DAS RAM is 2048/2048 B).
#ifdef TYPE_CORE
#define BUS_RX_REJECT_SLOTS 8

struct BusRxRejectRecord
{
    uint16_t Src;  // the frame's source address
    uint16_t Trid; // and its transaction ID - this is what names the request it answered
    uint16_t Cmd;  // service|CID
    uint16_t Len;  // payload bytes the header claimed
};

BusRxRejectRecord BusRxRejectRing[BUS_RX_REJECT_SLOTS];
uint8_t BusRxRejectNext = 0;
uint32_t BusRxRejectTotal = 0;

// One frame the CRC refused.
inline void BusRxRejectNote(uint16_t Src, uint16_t Trid, uint16_t Cmd, uint16_t Len)
{
    BusRxRejectRecord &Record = BusRxRejectRing[BusRxRejectNext];
    Record.Src = Src;
    Record.Trid = Trid;
    Record.Cmd = Cmd;
    Record.Len = Len;
    BusRxRejectNext = (uint8_t)((BusRxRejectNext + 1) % BUS_RX_REJECT_SLOTS);
    BusRxRejectTotal++;
}

// The assembler's state after its last pass (RX_SYNC 0, RX_HEADER 1, RX_PAYLOAD 2), and how many
// bytes of the frame it is holding plus how many it is waiting for.
//
// A window that expires with bytes on the wire and NO reject is a frame that never completed: the
// assembler is still holding its head. These three numbers, sampled when the window closes, say
// exactly that - e.g. `asm=2 got=29 need=37` is a 37-byte reply cut after 29 bytes, while
// `asm=0 got=0` means the line was clean between frames. Without them "the node did not answer"
// and "the node's answer was cut in half" are the same record.
uint8_t BusRxAsmStage = 0;
uint16_t BusRxAsmGot = 0;
uint16_t BusRxAsmNeed = 0;

inline void BusRxAsmNote(uint8_t Stage, uint16_t Got, uint16_t Need)
{
    BusRxAsmStage = Stage;
    BusRxAsmGot = Got;
    BusRxAsmNeed = Need;
}
#endif

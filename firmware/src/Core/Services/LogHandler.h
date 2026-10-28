#pragma once

#include "Core/Functions/Packet.h"
#include "Core/Functions/Log.h"
#include "Core/Functions/LogText.h"
#ifdef TYPE_CORE
// The runtime text ring is written from every task that logs and read by the app task, so its
// appends are serialised with the RTOS primitive the ESP32 core build already links. LogText.h
// itself stays free of RTOS types so the native tests can drive it.
#include "freertos/FreeRTOS.h"
#include "esp_log.h"
#endif

// Log Handler service (Docs/Services/Log Handler.md):
//   CID 0: Error reporter (outbound on nodes, inbound DB input on the core).
//   CID 1: GetLogs - stream of LogRecord entries (core only).
//   CID 2: ClearReadLogs - clear N logs from the end, confirm (core only).
//   CID 3: GetTextLogs - the runtime TEXT log ring, oldest line first (core only, Issues.md E67).
class LogHandlerService
{
public:
    bool Handle(const PacketFrame &Frame); // CID dispatch; true if consumed

#ifdef TYPE_CORE
    // --- The readable runtime log (Issues.md E67) -------------------------------------------
    // A RAM copy of the free-text lines `DeviceLog` emits. The console copy is dropped while an
    // app holds the USB port; this one is not, and CID 3 streams it out over the protocol.
    // Allocated once at boot on the heap (the same policy as the log database and the app TX
    // ring), so the static RAM cost is the ring's bookkeeping only.
    void InitText();
    void TextLog(uint32_t UptimeMs, const char *Tag, const char *Text);
    LogTextRing TextRing; // 0 B static: the bytes live in the heap block InitText() takes
#endif

private:
#ifdef TYPE_CORE
    // qsort comparator: orders used-slot indices by their LogSeq (ascending = oldest first).
    // Reads the shared LogSeq table, so it is only valid on the single task that runs the
    // handler (no concurrent log append).
    static int LogSeqSlotCompare(const void *Left, const void *Right);

    // Fills `order` (at least LOG_MAX_CAPACITY entries) with the used slot indices sorted
    // oldest-first by LogSeq and returns how many were written. One ordered pass feeds both
    // GetLogs and ClearReadLogs.
    static uint32_t LogBuildOrder(uint16_t *Order);

    uint32_t Clock = 0;               // monotonic sequence source for "oldest" tracking
    uint16_t Order[LOG_MAX_CAPACITY];
    portMUX_TYPE TextMux = portMUX_INITIALIZER_UNLOCKED; // serialises the text ring's appends
#endif
};

// One instance per binary, in Core/Services/.
LogHandlerService LogHandler;

#ifdef TYPE_CORE
// Storage for the runtime text ring. 4 KB is a WINDOW, not a transcript: it holds at least 25
// worst-case (160 B) lines and typically 50-80 real ones, which is the "last N lines" a rig
// session reads back.
#define LOG_TEXT_RING_BYTES 4096

// Takes the ring's storage. Called once from the boot sequence, before anything logs, so the
// allocation happens on one task and every later append (app task or a radio callback) has a
// ring to write to.
inline void LogHandlerService::InitText()
{
    if (TextRing.Buf)
        return;
    uint8_t *Storage = (uint8_t *)malloc(LOG_TEXT_RING_BYTES);
    if (!Storage || !TextRing.Init(Storage, LOG_TEXT_RING_BYTES))
    {
        free(Storage);
        TextRing.Init(nullptr, 0);
        ESP_LOGE("LOGH", "text log ring allocation failed (%u B): runtime lines stay console-only",
                 (unsigned)LOG_TEXT_RING_BYTES);
    }
}

inline void LogHandlerService::TextLog(uint32_t UptimeMs, const char *Tag, const char *Text)
{
    // A short critical section: the line is at most LOG_TEXT_LINE_MAX bytes and this runs from
    // the app task and the radio callbacks alike.
    portENTER_CRITICAL(&TextMux);
    TextRing.Append(UptimeMs, Tag, Text);
    portEXIT_CRITICAL(&TextMux);
}
#endif

#ifdef TYPE_CORE
inline int LogHandlerService::LogSeqSlotCompare(const void *Left, const void *Right)
{
    uint16_t Ia = *reinterpret_cast<const uint16_t *>(Left);
    uint16_t Ib = *reinterpret_cast<const uint16_t *>(Right);
    uint32_t Sa = LogSeq[Ia];
    uint32_t Sb = LogSeq[Ib];
    return (Sa > Sb) - (Sa < Sb);
}

inline uint32_t LogHandlerService::LogBuildOrder(uint16_t *Order)
{
    uint32_t Used = 0;
    for (uint32_t i = 0; i < LogCount; i++)
        if (LogUsed[i])
            Order[Used++] = (uint16_t)i;
    if (Used > 1)
        qsort(Order, Used, sizeof(uint16_t), LogSeqSlotCompare);
    return Used;
}
#endif

inline bool LogHandlerService::Handle(const PacketFrame &Frame)
{
    uint8_t Cid = GetServiceCID(Frame.Cmd);
    if (Frame.Flags & FLAG_TYPE) return true; // Ignore responses

#ifndef TYPE_CORE
    // Nodes are outbound-only log reporters; they do not process incoming reports.
    (void)Cid;
    return true;
#else
    // GetTextLogs (CID 3, Issues.md E67): the runtime TEXT log ring - the lines DeviceLog emits,
    // readable while an app holds the USB port. It needs no log database, so it is answered
    // before the DB allocation guard: a failed DB allocation must not also blind the text log.
    // Request: an optional uint32 byte count (0/absent = everything the ring holds).
    // Response: a FRAG stream of raw text bytes, oldest line first, each line '\n'-terminated.
    // The Log wire format is untouched - this stream is not a Log Struct and no Log Struct
    // changed; the ring is the readable copy, not a different log record.
    if (Cid == 3)
    {
        uint32_t Max = 0;
        if (PayloadBytes(Frame) >= 4)
            memcpy(&Max, Frame.Payload, sizeof(Max));
        uint32_t End = TextRing.End(); // snapshot: lines appended after this belong to the next read
        uint32_t From = TextRing.Begin(End, Max);
        if (From >= End)
        {
            SendResponse(Frame, nullptr, 0); // nothing logged yet (or the ring allocation failed)
            return true;
        }
        uint16_t Frags = (uint16_t)((End - From + MAX_FRAG_CONTENT_SIZE - 1) / MAX_FRAG_CONTENT_SIZE);
        uint32_t Pos = From;
        for (uint16_t Frag = 0; Frag < Frags; Frag++)
        {
            uint32_t N = TextRing.Copy(Pos, End, TxFrame.Payload + 4, MAX_FRAG_CONTENT_SIZE);
            Pos += N;
            SendFragFragment(Frame, Frag, Frags, (uint16_t)N, PRIORITY_LOG);
        }
        return true;
    }

    EnsureLogStorage();
    if (!LogBuffer || !LogUsed || !LogSeq)
    {
        DeviceLog("LOGH", "log DB allocation failed"); // Heap allocation failed
        return true;
    }

    if (Cid == 0)
    {
        // Inbound log report -> keep in RAM, deduplicated per device + source + code.
        if (PayloadBytes(Frame) < sizeof(LogMessage))
            return true;
        const LogMessage *Msg = reinterpret_cast<const LogMessage *>(Frame.Payload);
        uint32_t Seq = ++Clock;

        int FreeSlot = -1;
        int OldestSlot = -1;
        uint32_t OldestSeq = 0xFFFFFFFF;
        for (uint32_t i = 0; i < LogCapacity; i++)
        {
            if (i < LogCount && LogUsed[i])
            {
                if (LogBuffer[i].DeviceId == Frame.IdSrc &&
                    LogBuffer[i].Msg.Source == Msg->Source &&
                    LogCode(LogBuffer[i].Msg) == LogCode(*Msg))
                {
                    // Dedup hit: refresh in place, newest wins the sequence number.
                    LogBuffer[i].Count++;
                    LogBuffer[i].Msg.Timestamp = Msg->Timestamp;
                    LogSeq[i] = Seq;
                    return true;
                }
                if (LogSeq[i] < OldestSeq)
                {
                    OldestSeq = LogSeq[i];
                    OldestSlot = (int)i;
                }
            }
            else if (FreeSlot == -1)
                FreeSlot = (int)i; // any unused slot: tail or a cleared-by-ClearReadLogs hole
        }

        uint32_t Slot;
        if (FreeSlot >= 0)
        {
            Slot = (uint32_t)FreeSlot;
        }
        else
        {
            // Database full: try to GROW it on the heap first; only when the heap cannot
            // provide more room, drop the OLDEST record to make space for this one.
            if (LogCapacity < LOG_MAX_CAPACITY && GrowLogStorage(LogCapacity + LOG_GROW_STEP))
                Slot = LogCount++;
            else
                Slot = (uint32_t)OldestSlot; // growth failed / hard cap: drop the oldest record
        }

        LogBuffer[Slot].DeviceId = Frame.IdSrc;
        LogBuffer[Slot].Count = 1;
        LogBuffer[Slot].Msg = *Msg;
        LogUsed[Slot] = true;
        LogSeq[Slot] = Seq;
        if ((uint32_t)Slot >= LogCount) LogCount = Slot + 1;
        return true;
    }

    if (Cid == 1 || Cid == 2)
    {
        // One ordered view of the database (oldest first by LogSeq) feeds both CIDs:
        // GetLogs must emit oldest-first, and ClearReadLogs must drop the lowest sequences.
        uint32_t Active = LogBuildOrder(Order);

        if (Cid == 1) // GetLogs: stream every stored LogRecord entry, oldest first
        {
            if (Active == 0)
            {
                SendResponse(Frame, nullptr, 0);
                return true;
            }

            // Stream every record as a FRAG stream (Docs/Services/Log Handler.md:
            // "Fragmentation, entries"). Each fragment carries up to 112 bytes of
            // LogRecords; SendFragFragment writes the 4-byte frag info.
            uint32_t Total = Active * sizeof(LogRecord);
            uint16_t TotalFrags = (uint16_t)((Total + MAX_FRAG_CONTENT_SIZE - 1) / MAX_FRAG_CONTENT_SIZE);
            uint32_t Sent = 0;
            for (uint16_t Frag = 0; Frag < TotalFrags; Frag++)
            {
                uint16_t Off = 4;
                while (Off - 4 + sizeof(LogRecord) <= MAX_FRAG_CONTENT_SIZE && Sent < Active)
                {
                    memcpy(TxFrame.Payload + Off, &LogBuffer[Order[Sent]], sizeof(LogRecord));
                    Off += sizeof(LogRecord);
                    Sent++;
                }
                SendFragFragment(Frame, Frag, TotalFrags, (uint16_t)(Off - 4), PRIORITY_LOG);
            }
            return true;
        }

        // ClearReadLogs: clear the `n` OLDEST entries (lowest seq), per Log Handler.md
        uint32_t Count = 0;
        if (PayloadBytes(Frame) >= 4)
            memcpy(&Count, Frame.Payload, sizeof(Count));
        uint32_t Clear = (Count < Active) ? Count : Active;
        for (uint32_t k = 0; k < Clear; k++)
            LogUsed[Order[k]] = false;
        uint8_t Status = 0;
        SendResponse(Frame, &Status, 1);
    }
    return true;
#endif
}

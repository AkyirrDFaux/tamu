#pragma once

// Bounded text ring for the device's own log (Issues.md E67 / E70).
//
// WHY IT EXISTS. The console copy of `DeviceLog` cannot be the only channel, because the console
// IS the app link's byte stream: a line written between two frame writes corrupts the frame it
// lands in, so the copy is suppressed while that stream is in use
// (Core/Functions/AppConsole.h, owner ruling 2026-10-28 route (d)). A line a WRITE triggers is
// therefore lost BY CONSTRUCTION (Issues.md E70): the write arrives over the same USB link, so
// `UsbLastRx` moved microseconds before the line is produced (`radio disabled by the BLE Settings
// block`, `radio up: ...`, `advertised name -> '...'`, `passkey written: ...`), while the same
// image prints those tags at boot, when nothing has been received yet.
//
// So there is a second channel: every line `DeviceLog` formats goes in here whatever the link is
// doing (defer, never drop), and a host reads it back through the Log Handler service
// (CID 3 GetTextLogs / CID 4 GetTextLogState, Core/Services/LogHandler.h). It needs no console, is
// unaffected by the cable being attached, and survives the link being busy the whole time.
//
// GEOMETRY AND ITS COST (bounded by construction - a flood can neither grow RAM nor stall a pass):
//   LOG_TEXT_SLOTS lines, LOG_TEXT_LINE text bytes each; sizeof(LogTextSlot) = 112 B,
//   so the ring is 32 * 112 = 3,584 B of static RAM plus 24 B of counters, core only.
// A full ring overwrites the OLDEST line and moves `Overwrites`; a push is a bounded memcpy and a
// counter bump, so it is O(1) and cannot block the loop for longer than the copy.
//
// EVERY LINE CARRIES ITS OWN ACCOUNTING: `Seq` (1-based, monotonic - the sequence number a reader
// resumes from) and `Overwrites` (the ring's count at the moment the line was written). A reader
// that races a flood can therefore say exactly how many lines were dropped before the ones it
// holds - a stated "drop, counted", never a silent gap and never a truncated line.
//
// TRUNCATION IS AT THE SLOT, NOT AT A READ. A line longer than LOG_TEXT_LINE is stored with its
// first LOG_TEXT_LINE bytes and FLAG_TRUNCATED; `Truncated` counts them. A record always fits one
// fragment (LOG_TEXT_RECORD_MAX <= MAX_FRAG_CONTENT_SIZE, pinned in LogHandler.h), so a fragment
// carries whole lines and a lost fragment loses whole lines.
//
// CONCURRENCY. LogTextLock()/LogTextUnlock() guard every push and every copy a read makes, so a
// line is never copied while it is being rewritten (the ESP core has two producers: the app task
// and the NimBLE host task). The ring itself knows nothing about the lock - it is a plain
// structure over caller-owned storage with pure arithmetic, which is what lets
// test/native/logtext_test.cpp drive the real wrap behaviour on the host.
//
// AN ALL-ZERO RING IS AN EMPTY RING. .bss zeroes the storage, and the only field whose empty value
// is not zero is the next sequence number, set on the first push - so no boot-order init call is
// needed for the boot banner to be captured.

#include <cstdint>

#define LOG_TEXT_SLOTS 32 // lines held (the oldest is overwritten first)
#define LOG_TEXT_LINE 88  // text bytes kept per line (see the truncation rule above)
#define LOG_TEXT_TAG 8    // tag bytes kept, NUL padded (every call site's tag is a literal)

#define LOG_TEXT_FLAG_TRUNCATED 0x01 // the line was longer than LOG_TEXT_LINE

// One line. `Tag` is COPIED, not pointed at: a read can happen long after the call site's frame
// is gone, and a dangling tag would be read as a line with a garbled name.
struct LogTextSlot
{
    uint32_t Seq;        // 1-based sequence number; the reader resumes from it
    uint32_t At;         // TimeFromBoot() when the line was produced (ms)
    uint32_t Overwrites; // the ring's overwrite count when this line was written
    uint16_t Len;        // bytes of Text held (<= LOG_TEXT_LINE)
    uint8_t Flags;       // LOG_TEXT_FLAG_*
    char Tag[LOG_TEXT_TAG];
    char Text[LOG_TEXT_LINE];
};

struct LogTextRing
{
    LogTextSlot Slots[LOG_TEXT_SLOTS];
    uint32_t Next;       // the slot the next line takes (free-running, modulo SLOTS)
    uint32_t Count;      // lines held (<= LOG_TEXT_SLOTS)
    uint32_t NextSeq;    // the sequence number the next line gets
    uint32_t Written;    // lines offered since boot
    uint32_t Overwrites; // lines dropped because the ring was full
    uint32_t Truncated;  // lines stored shortened (LOG_TEXT_FLAG_TRUNCATED)
};

// ---- the device lock (no-op on the host: the native test is single-threaded) -----------------
#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
static portMUX_TYPE LogTextMux = portMUX_INITIALIZER_UNLOCKED;
inline void LogTextLock() { portENTER_CRITICAL(&LogTextMux); }
inline void LogTextUnlock() { portEXIT_CRITICAL(&LogTextMux); }
#else
inline void LogTextLock() {}
inline void LogTextUnlock() {}
#endif

// A read's view of the ring, taken in ONE lock (the counters alone are not a consistent picture
// once a writer can move between them).
struct LogTextView
{
    uint32_t OldestSeq;  // sequence number of the oldest line still held (== NextSeq when empty)
    uint32_t Count;      // lines held
    uint32_t NextSeq;    // the sequence number the next line will get
    uint32_t Written;    // lines offered since boot
    uint32_t Overwrites; // lines dropped so far
    uint32_t Truncated;  // lines stored shortened
};

inline LogTextView LogTextViewOf(const LogTextRing &R)
{
    LogTextView V;
    V.Count = R.Count;
    V.OldestSeq = R.Count ? R.NextSeq - R.Count : (R.NextSeq ? R.NextSeq : 1);
    V.NextSeq = R.NextSeq ? R.NextSeq : 1; // an untouched ring's next line is sequence 1
    V.Written = R.Written;
    V.Overwrites = R.Overwrites;
    V.Truncated = R.Truncated;
    return V;
}

// The line at ring order `k` (0 = oldest) of the view `V`. Valid while `V` is: a writer that
// overwrites a slot mid-walk moves the whole window, which is why the reader re-checks Seq (a line
// it has already passed is skipped, never mixed) and why every copy is made under the lock.
inline const LogTextSlot &LogTextSlotAt(const LogTextRing &R, const LogTextView &V, uint32_t k)
{
    // OldestSeq is the sequence number; its ring index is (OldestSeq - 1) % LOG_TEXT_SLOTS.
    uint32_t OldestIdx = (V.OldestSeq - 1) % LOG_TEXT_SLOTS;
    return R.Slots[(OldestIdx + k) % LOG_TEXT_SLOTS];
}

// Appends one line. Called for EVERY DeviceLog line, whether or not the console copy can be
// written - that is the whole point (E70): defer, do not drop.
inline void LogTextPush(LogTextRing &R, const char *Tag, const char *Text, uint32_t At)
{
    if (R.NextSeq == 0)
        R.NextSeq = 1; // an all-zero ring is an empty ring (see the header)

    uint16_t Len = 0;
    while (Len < LOG_TEXT_LINE && Text[Len] != '\0')
        Len++;
    uint8_t Flags = 0;
    if (Text[Len] != '\0')
    {
        Flags = LOG_TEXT_FLAG_TRUNCATED;
        R.Truncated = R.Truncated + 1;
    }

    LogTextSlot &S = R.Slots[R.Next % LOG_TEXT_SLOTS];
    if (R.Count == LOG_TEXT_SLOTS)
        R.Overwrites = R.Overwrites + 1; // this slot held the oldest line: it is being overwritten
    S.Seq = R.NextSeq;
    S.At = At;
    S.Overwrites = R.Overwrites;
    S.Len = Len;
    S.Flags = Flags;
    for (int i = 0; i < LOG_TEXT_TAG; i++)
        S.Tag[i] = (Tag && Tag[i]) ? Tag[i] : '\0';
    for (uint16_t i = 0; i < Len; i++)
        S.Text[i] = Text[i];

    R.Next = (R.Next + 1) % LOG_TEXT_SLOTS;
    if (R.Count < LOG_TEXT_SLOTS)
        R.Count = R.Count + 1;
    R.NextSeq = R.NextSeq + 1;
    R.Written = R.Written + 1;
}

// ---- wire records -----------------------------------------------------------------------------
// One line, as CID 3 emits it: one fragment content, one line.
//   Seq u32 | At u32 | Overwrites u32 | Len u8 | Flags u8 | Tag[8] | Text[Len]
// The tag is on the wire (a text line without its tag names nothing) and the accounting travels
// with the line, so a reader needs no second request to place it.
#define LOG_TEXT_RECORD_HEADER 22
#define LOG_TEXT_RECORD_MAX (LOG_TEXT_RECORD_HEADER + LOG_TEXT_LINE) // 110 <= MAX_FRAG_CONTENT_SIZE

// Encodes one line into `Dst`. Returns the bytes written, or 0 when it cannot fit - a record is
// emitted whole or not at all, which is what keeps a read from ever producing a half line.
inline uint16_t LogTextEncodeLine(const LogTextSlot &S, uint8_t *Dst, uint16_t Cap)
{
    if (Cap < LOG_TEXT_RECORD_HEADER || (uint32_t)Cap < (uint32_t)LOG_TEXT_RECORD_HEADER + S.Len)
        return 0;
    uint16_t Off = 0;
    const uint8_t *Fields[3] = {(const uint8_t *)&S.Seq, (const uint8_t *)&S.At,
                                (const uint8_t *)&S.Overwrites};
    for (int i = 0; i < 3; i++)
        for (int b = 0; b < 4; b++)
            Dst[Off++] = Fields[i][b];
    Dst[Off++] = (uint8_t)(S.Len & 0xFF);
    Dst[Off++] = S.Flags;
    for (int i = 0; i < LOG_TEXT_TAG; i++)
        Dst[Off++] = (uint8_t)S.Tag[i];
    for (uint16_t i = 0; i < S.Len; i++)
        Dst[Off++] = (uint8_t)S.Text[i];
    return Off;
}

// The ring's state, as CID 4 emits it (16 B, no fragmentation):
//   OldestSeq u32 | NextSeq u32 | Overwrites u32 | Written u32
// `OldestSeq` is the oldest line still held (== NextSeq when the ring is empty), so a reader can
// see both what it may still ask for and how many lines it will never get.
#define LOG_TEXT_STATE_SIZE 16
inline void LogTextEncodeState(const LogTextView &V, uint8_t *Dst)
{
    uint32_t Fields[4] = {V.OldestSeq, V.NextSeq, V.Overwrites, V.Written};
    for (int i = 0; i < 4; i++)
        for (int b = 0; b < 4; b++)
            Dst[i * 4 + b] = (uint8_t)((Fields[i] >> (8 * b)) & 0xFF);
}

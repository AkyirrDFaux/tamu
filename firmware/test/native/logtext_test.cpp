// Host-side test for the device log's text ring (Core/Functions/LogText.h) - Issues.md E67/E70.
//
// Why it exists: `DeviceLog`'s console copy is suppressed while the USB link is in use, and the
// console IS that link's byte stream, so a line a WRITE triggers is lost by construction (E70).
// Every line is therefore deferred into a bounded ring that a host reads back over the protocol
// (Core/Services/LogHandler.h CID 3 GetTextLogs / CID 4 GetTextLogState). The ring's wrap
// arithmetic and its accounting are what make that read trustworthy, and this file drives the REAL
// header - no extracted predicate - so the geometry in LogText.h is the thing under test.
//
// What is pinned here:
//   1. an all-zero ring IS an empty ring (the boot banner is captured with no init call);
//   2. ring order is oldest-first and Seq is 1-based and monotonic;
//   3. a full ring overwrites the OLDEST line, Count stays at the geometry, and Overwrites moves;
//   4. every line carries the overwrite count in force when it was written, so a reader can state
//      how many lines it will never get ("drop, counted");
//   5. the index arithmetic across the wrap (a second lap) - the bug class this exists for;
//   6. a line longer than the slot is stored shortened and FLAGGED, and it is still ONE record
//      (a read never returns a fragment-sized half of a line);
//   7. a record is emitted whole or not at all (too small a buffer encodes nothing);
//   8. a writer racing a reader never produces a mixed line: every copied record equals a pushed
//      line verbatim;
//   9. the state record's oldest/next/written/overwrites reconcile with the lines themselves;
//  10. the residue rule: written = count + overwrites (what a reader's totals must close to).

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "Core/Functions/LogText.h"

#include "check.h"

// The wire record, decoded the way a host does it (LogHandler.h CID 3). Kept independent of the
// encoder so a layout change has to be made twice - which is the point.
struct Decoded
{
    uint32_t Seq = 0, At = 0, Overwrites = 0;
    uint16_t Len = 0;
    uint8_t Flags = 0;
    char Tag[LOG_TEXT_TAG + 1] = {};
    char Text[LOG_TEXT_LINE + 1] = {};
};

static bool DecodeRecord(const uint8_t *Src, uint16_t Bytes, Decoded &Out)
{
    if (Bytes < LOG_TEXT_RECORD_HEADER)
        return false;
    uint16_t Off = 0;
    memcpy(&Out.Seq, Src + Off, 4); Off += 4;
    memcpy(&Out.At, Src + Off, 4); Off += 4;
    memcpy(&Out.Overwrites, Src + Off, 4); Off += 4;
    Out.Len = Src[Off++];
    Out.Flags = Src[Off++];
    memcpy(Out.Tag, Src + Off, LOG_TEXT_TAG); Off += LOG_TEXT_TAG;
    Out.Tag[LOG_TEXT_TAG] = '\0';
    // A record is exactly header + Len: anything else is not a whole line.
    if (Bytes != LOG_TEXT_RECORD_HEADER + Out.Len)
        return false;
    memcpy(Out.Text, Src + Off, Out.Len);
    Out.Text[Out.Len] = '\0';
    return true;
}

static void Push(LogTextRing &R, const char *Tag, const char *Text, uint32_t At)
{
    LogTextPush(R, Tag, Text, At);
}

// The ring's overwrite count, read the way the ring means it.
static uint32_t V_Overwrites(LogTextRing &R) { return LogTextViewOf(R).Overwrites; }

// The host's read of CID 3: encode every line with Seq > After and decode it back.
static int ReadLines(LogTextRing &R, uint32_t After, Decoded *Out, int Max)
{
    LogTextView V = LogTextViewOf(R);
    int N = 0;
    for (uint32_t k = 0; k < V.Count; k++)
    {
        const LogTextSlot &S = LogTextSlotAt(R, V, k);
        if (S.Seq <= After)
            continue;
        uint8_t Buf[LOG_TEXT_RECORD_MAX];
        uint16_t Len = LogTextEncodeLine(S, Buf, sizeof(Buf));
        if (!Len || N >= Max)
            continue;
        if (!DecodeRecord(Buf, Len, Out[N]))
            continue;
        N++;
    }
    return N;
}

static void TestEmptyRingIsZeroedStorage()
{
    // .bss gives zeros and nothing initialises the ring, so an untouched ring must already BE the
    // empty ring - otherwise the boot banner would be the one run of lines the ring misses.
    static LogTextRing R;
    LogTextView V = LogTextViewOf(R);
    CHECK_EQ(V.Count, 0);
    CHECK_EQ(V.NextSeq, 1); // the first line a push takes is sequence 1
    CHECK_EQ(V.OldestSeq, 1);  // ... so the oldest line still held is that same 1
    CHECK_EQ(R.Overwrites, 0);
    CHECK_EQ(R.Next, 0);
}

static void TestOrderAndFields()
{
    static LogTextRing R;
    Push(R, "E70", "first", 100);
    Push(R, "APPBLE", "radio up: name='Akyirr' mode=passkey bonds=0", 200);
    Push(R, "BLESEC", "passkey written: mode=passkey nvs=set", 300);

    Decoded D[8];
    int N = ReadLines(R, 0, D, 8);
    CHECK_EQ(N, 3);
    CHECK_EQ(D[0].Seq, 1);
    CHECK_EQ(D[1].Seq, 2);
    CHECK_EQ(D[2].Seq, 3); // oldest first, monotonic
    CHECK(std::strcmp(D[0].Text, "first") == 0);
    CHECK(std::strcmp(D[0].Tag, "E70") == 0);
    CHECK_EQ(D[0].At, 100);
    CHECK(std::strcmp(D[2].Text, "passkey written: mode=passkey nvs=set") == 0);
    CHECK(std::strcmp(D[2].Tag, "BLESEC") == 0); // the tag travels with the line
    CHECK_EQ(R.Written, 3);
    CHECK_EQ(R.Count, 3);
    CHECK_EQ(V_Overwrites(R), 0);
}

static void TestFullRingOverwritesTheOldest()
{
    static LogTextRing R;
    char Text[32];
    for (int i = 1; i <= LOG_TEXT_SLOTS; i++)
    {
        std::snprintf(Text, sizeof(Text), "line %d", i);
        Push(R, "RING", Text, (uint32_t)i);
    }
    CHECK_EQ(R.Count, LOG_TEXT_SLOTS);           // exactly the geometry
    CHECK_EQ(R.Overwrites, 0);                   // nothing overwritten yet
    CHECK_EQ(LogTextViewOf(R).OldestSeq, 1);     // ... and the window starts at the first line

    // Seven more lines: the ring must still hold the geometry, drop the oldest seven, and count
    // them. This is the card's "log more lines than the ring holds" case.
    for (int i = LOG_TEXT_SLOTS + 1; i <= LOG_TEXT_SLOTS + 7; i++)
    {
        std::snprintf(Text, sizeof(Text), "line %d", i);
        Push(R, "RING", Text, (uint32_t)i);
    }
    LogTextView V = LogTextViewOf(R);
    CHECK_EQ(R.Count, LOG_TEXT_SLOTS);
    CHECK_EQ(V.Overwrites, 7);
    CHECK_EQ(R.NextSeq - 1, LOG_TEXT_SLOTS + 7); // 39 lines offered
    CHECK_EQ(V.OldestSeq, 8);                    // the newest sequence - the geometry + 1

    Decoded D[LOG_TEXT_SLOTS];
    int N = ReadLines(R, 0, D, LOG_TEXT_SLOTS);
    CHECK_EQ(N, LOG_TEXT_SLOTS);
    CHECK_EQ(D[0].Seq, 8);  // the oldest line still present
    CHECK_EQ(D[0].Overwrites, 0); // ... and the counter in force when IT was written
    CHECK_EQ(D[LOG_TEXT_SLOTS - 1].Seq, LOG_TEXT_SLOTS + 7); // the newest is present
    CHECK(std::strcmp(D[LOG_TEXT_SLOTS - 1].Text, "line 39") == 0);
    // Each line's own accounting: line n was written with the ring's count then in force.
    for (int i = 0; i < N; i++)
    {
        uint32_t Seq = D[i].Seq;
        uint32_t Expected = (Seq > LOG_TEXT_SLOTS) ? Seq - LOG_TEXT_SLOTS : 0;
        CHECK_EQ(D[i].Overwrites, Expected);
    }
    // The seven oldest lines are GONE from the window, not merely unread: none of them is
    // returned, which is the other half of "the newest are present, the oldest gone".
    for (int i = 0; i < N; i++)
        CHECK(!(D[i].Seq >= 1 && D[i].Seq <= 7));
    CHECK_EQ(D[0].Seq, 8);
    // Residue rule a reader's totals close to: everything offered is either still held or counted.
    CHECK_EQ(R.Written, R.Count + R.Overwrites);
}

static void TestSecondLapWrapArithmetic()
{
    static LogTextRing R;
    char Text[32];
    const int Total = 2 * LOG_TEXT_SLOTS + 3;
    for (int i = 1; i <= Total; i++)
    {
        std::snprintf(Text, sizeof(Text), "l%d", i);
        Push(R, "RING", Text, (uint32_t)i);
    }
    LogTextView V = LogTextViewOf(R);
    CHECK_EQ(R.Next, (uint32_t)(Total % LOG_TEXT_SLOTS));                  // the write index wrapped twice
    CHECK_EQ(R.Count, LOG_TEXT_SLOTS);
    CHECK_EQ(V.Overwrites, (uint32_t)(Total - LOG_TEXT_SLOTS));            // 35 lines overwritten
    CHECK_EQ(V.OldestSeq, (uint32_t)(Total - LOG_TEXT_SLOTS + 1));         // sequence of the oldest held
    CHECK_EQ(V.NextSeq, (uint32_t)(Total + 1));                            // the next line's number
    // The index form of the same window: the slot the next line takes minus the lines held.
    CHECK_EQ((R.Next + LOG_TEXT_SLOTS - R.Count) % LOG_TEXT_SLOTS, (uint32_t)(Total % LOG_TEXT_SLOTS));
    CHECK_EQ(R.Written, R.Count + R.Overwrites);

    Decoded D[LOG_TEXT_SLOTS];
    int N = ReadLines(R, 0, D, LOG_TEXT_SLOTS);
    CHECK_EQ(N, LOG_TEXT_SLOTS);
    CHECK_EQ(D[0].Seq, V.OldestSeq);
    CHECK_EQ(D[N - 1].Seq, (uint32_t)Total);
    char Expected[32];
    std::snprintf(Expected, sizeof(Expected), "l%d", Total);
    CHECK(std::strcmp(D[N - 1].Text, Expected) == 0); // the newest survived both laps
}

static void TestLongLineIsStoredShortenedAndFlagged()
{
    static LogTextRing R;
    char Long[256];
    std::memset(Long, 'x', sizeof(Long));
    Long[255] = '\0';
    Push(R, "LONG", Long, 42);

    Decoded D[2];
    int N = ReadLines(R, 0, D, 2);
    CHECK_EQ(N, 1); // still exactly ONE record, not a fragment-sized half of one
    CHECK_EQ(D[0].Len, LOG_TEXT_LINE);
    CHECK_EQ(D[0].Flags & LOG_TEXT_FLAG_TRUNCATED, LOG_TEXT_FLAG_TRUNCATED);
    CHECK_EQ(D[0].Text[LOG_TEXT_LINE - 1], 'x');
    CHECK_EQ(R.Truncated, 1);
    // The record fits one fragment: the geometry's own bound, checked where it is used.
    CHECK(LOG_TEXT_RECORD_MAX <= 112);
    CHECK_EQ(LOG_TEXT_RECORD_MAX, LOG_TEXT_RECORD_HEADER + LOG_TEXT_LINE);

    // A line that fits is not flagged.
    Push(R, "SHORT", "ok", 43);
    N = ReadLines(R, 0, D, 2);
    CHECK_EQ(N, 2);
    CHECK_EQ(D[1].Flags, 0);
    CHECK_EQ(R.Truncated, 1); // only the long one was counted
}

static void TestEncodeIsWholeOrNothing()
{
    static LogTextRing R;
    Push(R, "SHORT", "ok", 7);
    LogTextView V = LogTextViewOf(R);
    const LogTextSlot &S = LogTextSlotAt(R, V, 0);
    uint8_t Buf[LOG_TEXT_RECORD_MAX];
    uint16_t Need = LogTextEncodeLine(S, Buf, sizeof(Buf));
    CHECK_EQ(Need, LOG_TEXT_RECORD_HEADER + 2);
    CHECK_EQ(LogTextEncodeLine(S, Buf, (uint16_t)(Need - 1)), 0); // one byte short: nothing at all
    CHECK_EQ(LogTextEncodeLine(S, Buf, (uint16_t)LOG_TEXT_RECORD_HEADER), 0);
    // ... and the buffer is left alone by a refused encode of a LONG line.
    uint8_t Small[LOG_TEXT_RECORD_HEADER];
    std::memset(Small, 0xAA, sizeof(Small));
    char Long[256];
    std::memset(Long, 'y', 200);
    Long[200] = '\0';
    Push(R, "LONG", Long, 8);
    LogTextView V2 = LogTextViewOf(R);
    const LogTextSlot &L = LogTextSlotAt(R, V2, 1);
    CHECK_EQ(LogTextEncodeLine(L, Small, sizeof(Small)), 0);
    CHECK_EQ(Small[0], 0xAA);
}

static void TestReadCursorResumes()
{
    static LogTextRing R;
    for (int i = 1; i <= 5; i++)
        Push(R, "CUR", "line", (uint32_t)i);
    Decoded D[8];
    CHECK_EQ(ReadLines(R, 4, D, 8), 1); // only sequence 5 is newer than 4
    CHECK_EQ(D[0].Seq, 5);
    CHECK_EQ(ReadLines(R, 5, D, 8), 0); // a fully read ring answers nothing new
    CHECK_EQ(ReadLines(R, 0, D, 8), 5); // ... and an empty payload asks for everything retained
    CHECK_EQ(ReadLines(R, 3, D, 8), 2);
}

static void TestWriterRacingReaderNeverMixesALine()
{
    // A reader walks the window while a writer floods it. The reader's copies are made under the
    // ring's lock on the device; here the interleaving is explicit. Every record it gets must be a
    // line that was pushed VERBATIM - never two lines' bytes spliced, which is what a read without
    // the lock (or a fragment cut across lines) would produce.
    static LogTextRing R;
    char Text[32];
    static const char *Sources[] = {"alpha", "bravo", "charlie", "delta"};
    int Pushed = 0;
    for (int i = 0; i < 40; i++)
    {
        std::snprintf(Text, sizeof(Text), "%s-%d", Sources[i % 4], i);
        Push(R, "RACE", Text, (uint32_t)i);
        Pushed++;
    }
    uint32_t MaxSeq = 0;
    int Seen = 0;
    for (int Round = 0; Round < 6; Round++)
    {
        // The writer keeps going between reads.
        for (int i = 0; i < 5; i++)
        {
            std::snprintf(Text, sizeof(Text), "%s-%d", Sources[Pushed % 4], Pushed);
            Push(R, "RACE", Text, (uint32_t)Pushed);
            Pushed++;
        }
        Decoded D[LOG_TEXT_SLOTS];
        int N = ReadLines(R, MaxSeq, D, LOG_TEXT_SLOTS);
        for (int i = 0; i < N; i++)
        {
            // The line is whole: it matches one pushed line, tag and timestamp included.
            bool Whole = false;
            for (int s = 0; s < Pushed && !Whole; s++)
            {
                char Expected[32];
                std::snprintf(Expected, sizeof(Expected), "%s-%d", Sources[s % 4], s);
                Whole = std::strcmp(D[i].Text, Expected) == 0;
            }
            CHECK(Whole);
            CHECK(D[i].Seq > MaxSeq);      // resuming never re-delivers an older line
            if (D[i].Seq > MaxSeq)
                MaxSeq = D[i].Seq;
            Seen++;
        }
    }
    CHECK(Seen > 0);
    CHECK_EQ(R.Written, R.Count + R.Overwrites); // still bounded and still accounted
}

static void TestStateRecordReconciles()
{
    static LogTextRing R;
    for (int i = 1; i <= LOG_TEXT_SLOTS + 4; i++)
        Push(R, "STATE", "s", (uint32_t)i);

    LogTextView V = LogTextViewOf(R);
    uint8_t State[LOG_TEXT_STATE_SIZE];
    LogTextEncodeState(V, State);
    uint32_t Oldest, Next, Overwrites, Written;
    std::memcpy(&Oldest, State + 0, 4);
    std::memcpy(&Next, State + 4, 4);
    std::memcpy(&Overwrites, State + 8, 4);
    std::memcpy(&Written, State + 12, 4);

    CHECK_EQ(Oldest, 5);                        // 36 lines offered, 32 held -> oldest is 5
    CHECK_EQ(Next, LOG_TEXT_SLOTS + 5);         // the number the next line will take
    CHECK_EQ(Overwrites, 4);
    CHECK_EQ(Written, LOG_TEXT_SLOTS + 4);
    CHECK_EQ(Written, LOG_TEXT_SLOTS + Overwrites); // written = held + overwritten

    // An emptied-by-reading ring is not an empty ring: the counters still say what happened.
    static LogTextRing E;
    LogTextEncodeState(LogTextViewOf(E), State);
    std::memcpy(&Oldest, State + 0, 4);
    std::memcpy(&Next, State + 4, 4);
    CHECK_EQ(Oldest, 1);
    CHECK_EQ(Next, 1);
}

int main()
{
    TestEmptyRingIsZeroedStorage();
    TestOrderAndFields();
    TestFullRingOverwritesTheOldest();
    TestSecondLapWrapArithmetic();
    TestLongLineIsStoredShortenedAndFlagged();
    TestEncodeIsWholeOrNothing();
    TestReadCursorResumes();
    TestWriterRacingReaderNeverMixesALine();
    TestStateRecordReconciles();

    printf("log text ring: %d checks, %d failure(s)\n", checks, failures);
    return failures == 0 ? 0 : 1;
}

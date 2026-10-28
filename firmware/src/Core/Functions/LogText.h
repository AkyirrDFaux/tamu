#pragma once

#include <cstdint>

// The runtime text log ring (Issues.md E67): a RAM copy of the lines `DeviceLog` emits, readable
// over the protocol while an app holds the USB port.
//
// Why it exists: `AppUSBActive()` reads true as soon as the cable is attached, so the USB byte
// stream belongs to the app link and free-text diagnostics are dropped exactly when a rig session
// needs them. Free text has no place in the Log Struct (Core/Types/Log.h: source + 32-bit code +
// timestamp, decoded into text by the app), so this ring is deliberately NOT part of the log wire
// format: it is a byte stream of its own, served by the Log Handler service (CID 3 `GetTextLogs`).
//
// One flat byte ring, one '\n'-terminated line per entry:
//     '[' uptime_ms ']' ' ' TAG ':' ' ' text '\n'
// The uptime stamp is what makes a line's place in time provable instead of inferred from when
// the host happened to read it.
//
// Offsets are ABSOLUTE (bytes ever written, so they stay monotonic for ~49 days at any plausible
// log rate) while `Head` is the ring index; a readable range [Oldest, Total) is at most Cap bytes.
// Host-clean on purpose - the storage is the caller's, so the native tests drive this exact code
// with a small buffer (test/native/log_text_test.cpp).

#define LOG_TEXT_LINE_MAX 160 // one formatted line incl. stamp, tag and terminator
#define LOG_TEXT_TAG_MAX  16  // a tag longer than this is truncated (they are short literals)

struct LogTextRing
{
    uint8_t *Buf = nullptr; // caller's storage; nullptr = uninitialised (every call is a no-op)
    uint32_t Cap = 0;       // storage size in bytes
    uint32_t Head = 0;      // next write index
    uint32_t Total = 0;     // bytes ever written (absolute)
    uint32_t Dropped = 0;   // lines refused because one alone does not fit in Cap

    bool Init(uint8_t *Storage, uint32_t Capacity)
    {
        Buf = Storage;
        Cap = Storage ? Capacity : 0;
        Head = 0;
        Total = 0;
        Dropped = 0;
        return Buf != nullptr && Cap >= 2;
    }

    // Appends one stamped line. A line that does not fit the WHOLE ring is refused (Dropped++):
    // a wrapped line would read as a complete event that never happened. Text longer than the
    // line buffer is truncated, which is what a console line does too.
    void Append(uint32_t UptimeMs, const char *Tag, const char *Text)
    {
        if (!Buf || Cap < 2)
            return;

        char Line[LOG_TEXT_LINE_MAX];
        uint32_t N = 0;
        Line[N++] = '[';
        N += Decimal(UptimeMs, &Line[N]);
        Line[N++] = ']';
        Line[N++] = ' ';
        N += AppendTag(&Line[N], Tag);
        Line[N++] = ':';
        Line[N++] = ' ';
        // Room left for the text: the terminator needs one byte. vsnprintf caps the text at 127,
        // so the worst case (a 10-digit stamp and a 16-char tag) lands exactly in the buffer.
        N += AppendText(&Line[N], Text, (uint32_t)(LOG_TEXT_LINE_MAX - N - 1));
        Line[N++] = '\n';

        if (N > Cap)
        {
            Dropped++;
            return;
        }
        for (uint32_t i = 0; i < N; i++)
        {
            Buf[Head] = (uint8_t)Line[i];
            Head++;
            if (Head == Cap)
                Head = 0;
        }
        Total += N;
    }

    // Absolute offset just past the newest byte: the snapshot point of a read. Lines appended
    // after it belong to the NEXT read, so one reply is a consistent window.
    uint32_t End() const { return Total; }

    // Absolute offset of the oldest byte still stored for `End`.
    uint32_t Oldest(uint32_t End) const { return End > Cap ? End - Cap : 0; }

    // Start offset for a read of at most `MaxBytes` (0 = everything stored), moved forward to the
    // first line boundary so the stream never begins with a partial line. Returns End when there
    // is no complete line in range.
    uint32_t Begin(uint32_t End, uint32_t MaxBytes) const
    {
        if (!Buf)
            return 0;
        uint32_t Old = Oldest(End);
        uint32_t From = Old;
        if (MaxBytes && End - From > MaxBytes)
            From = End - MaxBytes;
        // A start is only a line boundary if the byte logically before it is stored AND is the
        // terminator. At Old the predecessor may be long overwritten (the wrap lands mid-line),
        // and a stale '\n' there is not the terminator of THIS byte - so the partial line is
        // dropped instead of being streamed as a line that never existed.
        bool WholeStart = (From == 0) || (From > Old && At(From - 1) == '\n');
        if (!WholeStart)
        {
            uint32_t Scan = From;
            while (Scan < End && At(Scan) != '\n')
                Scan++;
            From = (Scan < End) ? Scan + 1 : End; // +1 skips the terminator itself
        }
        return From;
    }

    // Copies the absolute range [From, To) into `Dst` (at most `Max` bytes); returns the count.
    // A stream is exactly this called in MAX_FRAG_CONTENT_SIZE pieces.
    uint32_t Copy(uint32_t From, uint32_t To, uint8_t *Dst, uint32_t Max) const
    {
        if (!Buf || !Dst || To <= From)
            return 0;
        uint32_t N = To - From;
        if (N > Max)
            N = Max;
        if (N > Cap)
            N = Cap;
        for (uint32_t i = 0; i < N; i++)
            Dst[i] = At(From + i);
        return N;
    }

    // The byte at an absolute offset (only meaningful for a stored offset).
    char At(uint32_t Offset) const { return (char)Buf[Offset % Cap]; }

private:
    static uint32_t Decimal(uint32_t Value, char *Dst)
    {
        char Digits[10];
        uint32_t Count = 0;
        do
        {
            Digits[Count++] = (char)('0' + (Value % 10));
            Value /= 10;
        } while (Value);
        for (uint32_t i = 0; i < Count; i++)
            Dst[i] = Digits[Count - 1 - i];
        return Count;
    }

    static uint32_t AppendTag(char *Dst, const char *Tag)
    {
        uint32_t N = 0;
        while (Tag && Tag[N] && N < LOG_TEXT_TAG_MAX)
        {
            Dst[N] = Flat(Tag[N]);
            N++;
        }
        return N;
    }

    // One entry is one line: a newline or carriage return inside the text would split it into a
    // line the reader counts (and stamps) as an event of its own.
    static uint32_t AppendText(char *Dst, const char *Text, uint32_t Max)
    {
        uint32_t N = 0;
        while (Text && Text[N] && N < Max)
        {
            Dst[N] = Flat(Text[N]);
            N++;
        }
        return N;
    }

    static char Flat(char C) { return (C == '\n' || C == '\r') ? ' ' : C; }
};

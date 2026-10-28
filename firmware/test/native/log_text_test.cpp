// Host-side test for the runtime text log ring (Core/Functions/LogText.h, Issues.md E67).
//
// The ring is what makes the device's runtime log readable while an app holds the USB port: the
// console copy of `DeviceLog` is dropped as soon as the cable is attached, so the ring is the
// only copy left and the Log Handler service streams it out (CID 3 `GetTextLogs`). This drives
// the real implementation - the device's append path and the reader's snapshot/segment calls are
// the same functions - over a small buffer, so the line format, the wrap, the line-boundary rule
// and the fragment-shaped copying are all pinned without hardware.
//
// The device streams the window in MAX_FRAG_CONTENT_SIZE pieces (Core/Functions/Packet.h). That
// header needs the packet value model, so the size is repeated here and run.sh guards the two
// against drifting apart.
#define TEST_FRAG_CONTENT_SIZE 112

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "Core/Functions/LogText.h"

static int checks = 0, failures = 0;

#define CHECK(cond)                                                                  \
    do {                                                                             \
        checks++;                                                                    \
        if (!(cond)) {                                                               \
            failures++;                                                              \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);              \
        }                                                                            \
    } while (0)

#define CHECK_EQ(got, want)                                                          \
    do {                                                                             \
        checks++;                                                                    \
        const long long g = (long long)(got), w = (long long)(want);                 \
        if (g != w) {                                                                \
            failures++;                                                              \
            std::printf("FAIL %s:%d: %s = %lld, want %lld\n", __FILE__, __LINE__,    \
                        #got, g, w);                                                 \
        }                                                                            \
    } while (0)

#define CHECK_STREQ(got, want)                                                       \
    do {                                                                             \
        checks++;                                                                    \
        const std::string g = (got), w = (want);                                     \
        if (g != w) {                                                                \
            failures++;                                                              \
            std::printf("FAIL %s:%d: got \"%s\", want \"%s\"\n", __FILE__, __LINE__, \
                        g.c_str(), w.c_str());                                       \
        }                                                                            \
    } while (0)

// The whole readable window in one copy - what a reader that had room would see.
static std::string Snapshot(const LogTextRing &Ring, uint32_t MaxBytes = 0) {
    const uint32_t End = Ring.End();
    const uint32_t From = Ring.Begin(End, MaxBytes);
    std::string Out(End > From ? End - From : 0, '\0');
    if (!Out.empty()) {
        const uint32_t N = Ring.Copy(From, End, reinterpret_cast<uint8_t *>(&Out[0]),
                                     static_cast<uint32_t>(Out.size()));
        Out.resize(N);
    }
    return Out;
}

// The same window as the device sends it: TEST_FRAG_CONTENT_SIZE-byte pieces, one per fragment.
static std::string Streamed(const LogTextRing &Ring, uint32_t MaxBytes = 0) {
    const uint32_t End = Ring.End();
    const uint32_t From = Ring.Begin(End, MaxBytes);
    std::string Out;
    uint32_t Pos = From;
    uint8_t Chunk[TEST_FRAG_CONTENT_SIZE];
    while (Pos < End) {
        const uint32_t N = Ring.Copy(Pos, End, Chunk, sizeof(Chunk));
        if (N == 0)
            break; // a zero-byte fragment would be an endless stream
        Out.append(reinterpret_cast<const char *>(Chunk), N);
        Pos += N;
    }
    return Out;
}

static int CountLines(const std::string &Text) {
    int Lines = 0;
    for (char C : Text)
        if (C == '\n')
            Lines++;
    return Lines;
}

static std::string LastLine(const std::string &Text) {
    const size_t End = Text.rfind('\n');
    if (End == std::string::npos)
        return "";
    size_t From = 0;
    for (size_t i = End; i > 0; i--)
        if (Text[i - 1] == '\n') {
            From = i;
            break;
        }
    return Text.substr(From, End - From + 1);
}

int main() {
    // --- the line format ------------------------------------------------------------------
    {
        uint8_t Mem[256];
        LogTextRing Ring;
        CHECK(Ring.Init(Mem, sizeof(Mem)));
        CHECK_EQ(Ring.End(), 0);
        CHECK_EQ(Ring.Begin(0, 0), 0); // nothing logged: the stream is empty
        CHECK_STREQ(Snapshot(Ring), "");

        Ring.Append(12, "A", "one");
        Ring.Append(345, "BB", "two two");
        Ring.Append(6789, "C", "three");
        const std::string All = "[12] A: one\n[345] BB: two two\n[6789] C: three\n";
        CHECK_STREQ(Snapshot(Ring), All);
        CHECK_EQ(Ring.End(), All.size());
        CHECK_EQ(Ring.Oldest(Ring.End()), 0); // no wrap yet: everything is still stored
        CHECK_EQ(CountLines(Snapshot(Ring)), 3);
        CHECK_EQ(Ring.Dropped, 0);
    }

    // --- the tail is a window of whole lines (the request's byte count) --------------------
    {
        uint8_t Mem[256];
        LogTextRing Ring;
        Ring.Init(Mem, sizeof(Mem));
        Ring.Append(12, "A", "one");
        Ring.Append(345, "BB", "two two");
        Ring.Append(6789, "C", "three");
        Ring.Append(7000, "D", "four");
        const std::string Two = "[6789] C: three\n[7000] D: four\n";
        CHECK_STREQ(Snapshot(Ring, static_cast<uint32_t>(Two.size())), Two);
        // One byte less and the start falls INSIDE the older line: that line is dropped rather
        // than streamed as a line that never existed.
        CHECK_STREQ(Snapshot(Ring, static_cast<uint32_t>(Two.size() - 1)), "[7000] D: four\n");
        // A count that covers everything returns everything.
        CHECK_EQ(CountLines(Snapshot(Ring, 1u << 20)), 4);
    }

    // --- wrap: the newest lines survive, whole, in order -----------------------------------
    {
        uint8_t Mem[64];
        LogTextRing Ring;
        Ring.Init(Mem, sizeof(Mem));
        std::string All;
        for (uint32_t i = 0; i < 10; i++)
            All += "[" + std::to_string(i) + "] T: xxxx\n"; // 12 B each
        for (uint32_t i = 0; i < 10; i++)
            Ring.Append(i, "T", "xxxx");
        CHECK_EQ(Ring.End(), All.size());
        const uint32_t Oldest = Ring.Oldest(Ring.End());
        CHECK_EQ(Oldest, All.size() - 64u); // a 64 B storage keeps the last 64 B
        const std::string Window = Snapshot(Ring);
        CHECK(Window.size() <= 64);
        CHECK(Window.size() >= 52); // at most one 12 B line is lost to the boundary
        CHECK_EQ(Window[0], '[');                  // starts on a line boundary
        CHECK_EQ(Window[Window.size() - 1], '\n'); // ends on one
        CHECK_STREQ(LastLine(Window), "[9] T: xxxx\n");
        // The window is a SUFFIX of everything logged: nothing reordered, nothing invented.
        size_t Start = Oldest;
        if (Start % 12 != 0)
            Start += 12 - (Start % 12);
        CHECK_STREQ(Window, All.substr(Start));
        for (size_t i = 0; i + 1 < Window.size(); i++)
            if (Window[i] == '\n')
                CHECK_EQ(Window[i + 1], '['); // never a partial line after a terminator
    }

    // --- the fragment-shaped reader returns the same bytes as one big copy ------------------
    {
        uint8_t Mem[128];
        LogTextRing Ring;
        Ring.Init(Mem, sizeof(Mem));
        for (uint32_t i = 0; i < 20; i++)
            Ring.Append(1000 + i, "APPBLE", "line with some length in it");
        CHECK(Streamed(Ring) == Snapshot(Ring));
        CHECK(Streamed(Ring, 40) == Snapshot(Ring, 40));
        CHECK(!Streamed(Ring).empty()); // the ring keeps the newest lines, not nothing
        CHECK_EQ(Streamed(Ring)[0], '[');
        CHECK(CountLines(Streamed(Ring)) >= 2); // 30 B lines: two or more fit in 128 B
        CHECK(Streamed(Ring).size() <= 128);
    }

    // --- a line that does not fit the whole ring is refused, not wrapped --------------------
    {
        uint8_t Mem[32];
        LogTextRing Ring;
        Ring.Init(Mem, sizeof(Mem));
        Ring.Append(1, "T", "fits");
        const uint32_t Before = Ring.End();
        char Big[80];
        std::memset(Big, 'x', sizeof(Big) - 1);
        Big[sizeof(Big) - 1] = '\0';
        Ring.Append(2, "T", Big); // ~85 B into a 32 B ring
        CHECK_EQ(Ring.Dropped, 1);
        CHECK_EQ(Ring.End(), Before); // the stored window is untouched
        CHECK_STREQ(Snapshot(Ring), "[1] T: fits\n");
    }

    // --- one entry is one line: newlines inside the text are flattened ---------------------
    {
        uint8_t Mem[256];
        LogTextRing Ring;
        Ring.Init(Mem, sizeof(Mem));
        Ring.Append(5, "T", "a\nb\rc");
        CHECK_STREQ(Snapshot(Ring), "[5] T: a b c\n");
        CHECK_EQ(CountLines(Snapshot(Ring)), 1);

        // ... and a tag longer than the tag field is truncated, not allowed to shift the layout.
        char Tag[40];
        std::memset(Tag, 'T', sizeof(Tag) - 1);
        Tag[sizeof(Tag) - 1] = '\0';
        uint8_t Mem2[256];
        LogTextRing Ring2;
        Ring2.Init(Mem2, sizeof(Mem2));
        Ring2.Append(7, Tag, "x");
        const std::string Line = Snapshot(Ring2);
        CHECK_EQ(Line.size(), (size_t)(1 + 1 + 1 + 1 + LOG_TEXT_TAG_MAX + 1 + 1 + 1 + 1));
    }

    // --- a text longer than the line buffer is truncated, the line still terminates ---------
    {
        uint8_t Mem[1024];
        LogTextRing Ring;
        Ring.Init(Mem, sizeof(Mem));
        const std::string Long(300, 'y');
        Ring.Append(1, "T", Long.c_str());
        const std::string Line = Snapshot(Ring);
        CHECK_EQ(Ring.Dropped, 0);
        CHECK_EQ(Line.size(), (size_t)LOG_TEXT_LINE_MAX); // "[1] T: " + text + '\n' fills the line
        CHECK_EQ(Line[Line.size() - 1], '\n');
        CHECK_EQ(Line.compare(0, 7, "[1] T: "), 0);
        CHECK_EQ(CountLines(Line), 1);
    }

    // --- no storage (the heap allocation failed): every call is a no-op, the stream empty ---
    {
        LogTextRing Ring;
        CHECK(!Ring.Init(nullptr, 0));
        uint8_t Buffer[8];
        Ring.Append(1, "T", "lost");
        CHECK_EQ(Ring.End(), 0);
        CHECK_EQ(Ring.Begin(0, 0), 0);
        CHECK_EQ(Ring.Copy(0, 4, Buffer, sizeof(Buffer)), 0);
        CHECK_STREQ(Snapshot(Ring), "");

        // A one-byte ring cannot hold a line either, and says so instead of corrupting memory.
        uint8_t One[1];
        LogTextRing Tiny;
        CHECK(!Tiny.Init(One, sizeof(One)));
        Tiny.Append(1, "T", "x");
        CHECK_EQ(Tiny.End(), 0);
    }

    // --- a reply is a snapshot: lines appended afterwards are the next read -----------------
    {
        uint8_t Mem[256];
        LogTextRing Ring;
        Ring.Init(Mem, sizeof(Mem));
        Ring.Append(10, "T", "first");
        const std::string First = Snapshot(Ring);
        Ring.Append(11, "T", "second");
        CHECK_STREQ(First, "[10] T: first\n"); // unchanged by the later append
        const std::string Second = Snapshot(Ring);
        CHECK_STREQ(Second, "[10] T: first\n[11] T: second\n");
        // Which is exactly how a rig proves the buffer is live: read, let the device log again,
        // read - the second window ends with a line the first one did not have.
        CHECK(LastLine(Second) != LastLine(First));
    }

    // --- the stamp is a full uint32 (uptime wraps after 49 days) ----------------------------
    {
        uint8_t Mem[256];
        LogTextRing Ring;
        Ring.Init(Mem, sizeof(Mem));
        Ring.Append(0, "T", "boot");
        Ring.Append(4294967295u, "T", "late");
        CHECK_STREQ(Snapshot(Ring), "[0] T: boot\n[4294967295] T: late\n");
    }

    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}

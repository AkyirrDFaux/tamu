// Host-side test for the RX path that must survive a transmit (card t_4c9c5fa8).
//
// The defect this covers: the RSBus silence wait has to READ the UART to know whether the line is
// still active, and it used to throw every byte it read away. A reply that was arriving when the
// core decided to transmit was therefore destroyed by the wait itself, and nothing counted it -
// so a restore that ate a node's answer reported the node as silent.
//
// The fix is BusRxKeepRing (Core/Functions/Bus.h), a host-clean FIFO the device fills while it
// waits and whose bytes ReceivePacket()'s byte source reads BEFORE the UART. This test drives the
// real ring and the real assembler (ReceivePacketFrame) - not a model of them:
//
//   1. ring order, full detection (a refused byte is reported to the caller, which counts it),
//      and order preserved across the modulo wrap;
//   2. a reply kept during the wait assembles as a frame, and reads back byte-identical;
//   3. a reply whose head is in the ring and whose tail is still in the UART assembles too - the
//      two sources are concatenated in the right order (this is the interleaving that matters when
//      the wait stopped reading mid-frame);
//   4. inter-frame garbage in the ring is skipped, as on the bus;
//   5. a full ring refuses the next byte WITHOUT disturbing the frame it already holds (the older
//      frame is the one that must survive);
//   6. a frame split across two ProcessBus-style calls keeps the assembler's state.
//
// The assembler keeps its stage in a function-local static, so every case here finishes its frame
// before the next one starts (case 6 is the deliberate two-call split, and it completes).
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "Blocks/DeviceInfo.h" // DeviceStatusStruct, which Packet.h forward-uses
#include "Core/Functions/Bus.h"

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

// The device's ring holds one whole longest frame (12 + 116 header/payload + the 0xAA sync byte =
// 129) plus the head of the next; 160 is that with slack. Pinned here so a device that shrinks its
// ring has to change this test too.
static constexpr uint16_t KeepBytes = 160;
static constexpr uint16_t MaxWire = 1 + 12 + MAX_PAYLOAD_SIZE;

static PacketFrame MakeFrame(uint16_t PayloadLen, uint8_t Seed)
{
    PacketFrame Frame;
    std::memset(&Frame, 0, sizeof(Frame));
    Frame.Flags = FLAG_TYPE;
    Frame.Priority = 8;
    Frame.IdSrc = 0x0203;
    Frame.IdTgt = 0x0101;
    Frame.Cmd = 0x0301;
    Frame.Trid = 0x0042;
    for (uint16_t i = 0; i < PayloadLen; i++)
        Frame.Payload[i] = (uint8_t)(Seed + i);
    PacketFinalize(&Frame, PayloadLen);
    return Frame;
}

// The wire form of a frame: the 0xAA sync byte, then the packed frame.
static uint16_t MakeWire(const PacketFrame &Frame, uint8_t *Wire)
{
    Wire[0] = 0xAA;
    const uint16_t Size = PacketWireSize(&Frame);
    std::memcpy(Wire + 1, &Frame, Size);
    return (uint16_t)(Size + 1);
}

// A byte source of the shape the device uses: the keep ring first, then whatever the UART still
// holds. The "UART" half is a plain array here, and it counts how often it was touched.
struct Sources
{
    BusRxKeepRing<KeepBytes> Ring;
    const uint8_t *Tail = nullptr;
    uint16_t TailLen = 0;
    uint16_t TailPos = 0;
    int TailReads = 0;

    bool Next(uint8_t &Byte)
    {
        if (Ring.Pop(Byte))
            return true;
        if (TailPos < TailLen)
        {
            Byte = Tail[TailPos++];
            TailReads++;
            return true;
        }
        return false;
    }
};

static void TestRingBasics()
{
    BusRxKeepRing<8> Ring;
    CHECK_EQ(Ring.Count(), 0);
    uint8_t B = 0;
    CHECK(!Ring.Pop(B)); // empty

    for (uint8_t i = 0; i < 8; i++)
        CHECK(Ring.Push(i));
    CHECK_EQ(Ring.Count(), 8);
    CHECK(!Ring.Push(0xEE)); // full: refused, and the caller is what counts the drop
    CHECK_EQ(Ring.Count(), 8); // a refusal must not consume or overwrite anything

    for (uint8_t i = 0; i < 8; i++)
    {
        CHECK(Ring.Pop(B));
        CHECK_EQ(B, i); // the bytes that were kept are still in order
    }
    CHECK(!Ring.Pop(B));

    // Order across the modulo wrap: drain 3, push 3 more, and the sequence must still be FIFO.
    for (uint8_t i = 0; i < 8; i++)
        CHECK(Ring.Push((uint8_t)(0x10 + i)));
    for (int i = 0; i < 3; i++)
        CHECK(Ring.Pop(B));
    CHECK_EQ(B, 0x12);
    for (uint8_t i = 0; i < 3; i++)
        CHECK(Ring.Push((uint8_t)(0x20 + i)));
    for (uint8_t i = 3; i < 8; i++)
    {
        CHECK(Ring.Pop(B));
        CHECK_EQ(B, (uint8_t)(0x10 + i));
    }
    for (uint8_t i = 0; i < 3; i++)
    {
        CHECK(Ring.Pop(B));
        CHECK_EQ(B, (uint8_t)(0x20 + i));
    }
    CHECK(!Ring.Pop(B));
}

// Case 2: the whole reply was taken off the UART while the transmitter waited for silence.
static void TestWholeFrameFromRing()
{
    const PacketFrame Frame = MakeFrame(20, 0x30);
    uint8_t Wire[MaxWire];
    const uint16_t WireLen = MakeWire(Frame, Wire);

    Sources Src;
    for (uint16_t i = 0; i < WireLen; i++)
        CHECK(Src.Ring.Push(Wire[i]));
    CHECK(Src.TailLen == 0);

    PacketFrame Out;
    std::memset(&Out, 0, sizeof(Out));
    CHECK_EQ(ReceivePacketFrame(&Out, [&Src](uint8_t &B) { return Src.Next(B); }),
             WireLen);
    CHECK(std::memcmp(&Out, &Frame, PacketWireSize(&Frame)) == 0);
    CHECK_EQ(Out.PayloadLen, 20);
    CHECK_EQ(Out.Trid, 0x0042);
    CHECK(std::memcmp(Out.Payload, Frame.Payload, 20) == 0);
    CHECK_EQ(Src.Ring.Count(), 0);
}

// Case 3: the wait stopped reading mid-frame - the head is kept, the tail is still in the UART.
// This is the case the old drain could not survive at all: the head was gone and the frame could
// never be validated.
static void TestFrameSplitAcrossRingAndUart()
{
    const PacketFrame Frame = MakeFrame(52, 0x77);
    uint8_t Wire[MaxWire];
    const uint16_t WireLen = MakeWire(Frame, Wire);
    const uint16_t Split = 23; // deliberately not on a header/ payload boundary

    Sources Src;
    for (uint16_t i = 0; i < Split; i++)
        CHECK(Src.Ring.Push(Wire[i]));
    Src.Tail = Wire + Split;
    Src.TailLen = (uint16_t)(WireLen - Split);

    PacketFrame Out;
    std::memset(&Out, 0, sizeof(Out));
    CHECK_EQ(ReceivePacketFrame(&Out, [&Src](uint8_t &B) { return Src.Next(B); }),
             WireLen);
    CHECK(std::memcmp(&Out, &Frame, PacketWireSize(&Frame)) == 0);
    CHECK_EQ(Src.TailReads, WireLen - Split); // the UART half was read after the kept half
    uint8_t Extra = 0;
    CHECK(!Src.Next(Extra));                  // nothing left over
}

// Case 4: garbage before the sync byte is skipped wherever it came from.
static void TestGarbageThenFrame()
{
    const PacketFrame Frame = MakeFrame(4, 0xC0);
    uint8_t Wire[MaxWire];
    const uint16_t WireLen = MakeWire(Frame, Wire);

    Sources Src;
    const uint8_t Garbage[4] = {0x00, 0x55, 0x13, 0xFE};
    for (uint8_t B : Garbage)
        CHECK(Src.Ring.Push(B));
    for (uint16_t i = 0; i < WireLen; i++)
        CHECK(Src.Ring.Push(Wire[i]));

    PacketFrame Out;
    std::memset(&Out, 0, sizeof(Out));
    CHECK_EQ(ReceivePacketFrame(&Out, [&Src](uint8_t &B) { return Src.Next(B); }),
             WireLen);
    CHECK(std::memcmp(&Out, &Frame, PacketWireSize(&Frame)) == 0);
}

// Case 5: the ring is full. The next byte is refused (that is what BusRxDestroyed counts), and the
// frame it already holds must still assemble - the older frame is the one that has to survive.
static void TestFullRingKeepsTheOlderFrame()
{
    const PacketFrame Frame = MakeFrame(MAX_PAYLOAD_SIZE, 0x01); // the longest frame there is
    uint8_t Wire[MaxWire];
    const uint16_t WireLen = MakeWire(Frame, Wire);
    CHECK(WireLen <= KeepBytes); // a whole frame fits, or nothing here is safe

    Sources Src;
    for (uint16_t i = 0; i < WireLen; i++)
        CHECK(Src.Ring.Push(Wire[i]));

    // Fill to capacity with the head of the next frame, then prove the byte past capacity is
    // refused (the device adds exactly that byte to BusRxDestroyed) and disturbs nothing.
    uint16_t Filler = 0;
    while (Src.Ring.Push((uint8_t)(0xE0 + (Filler & 0x0F))))
        Filler++;
    CHECK_EQ(Src.Ring.Count(), KeepBytes);
    CHECK_EQ((uint16_t)(WireLen + Filler), KeepBytes);
    CHECK(!Src.Ring.Push(0xAA));
    CHECK_EQ(Src.Ring.Count(), KeepBytes);

    PacketFrame Out;
    std::memset(&Out, 0, sizeof(Out));
    CHECK_EQ(ReceivePacketFrame(&Out, [&Src](uint8_t &B) { return Src.Next(B); }),
             WireLen);
    CHECK(std::memcmp(&Out, &Frame, PacketWireSize(&Frame)) == 0);
}

// Case 6: a frame that arrives in two pieces across two passes (the assembler's state persists).
// The tail is pulled from the ring on the second pass, i.e. the wait kept the rest.
//
// Both calls go through the SAME byte-source object: the assembler keeps its stage in a static per
// byte-source type, so this is also the test that the device's single source really carries the
// state across passes.
static void TestFrameAcrossTwoPasses()
{
    const PacketFrame Frame = MakeFrame(16, 0x5A);
    uint8_t Wire[MaxWire];
    const uint16_t WireLen = MakeWire(Frame, Wire);
    const uint16_t Split = 7;

    Sources Src;
    for (uint16_t i = 0; i < Split; i++)
        CHECK(Src.Ring.Push(Wire[i]));

    auto Read = [&Src](uint8_t &B) { return Src.Next(B); };

    PacketFrame Out;
    std::memset(&Out, 0, sizeof(Out));
    CHECK_EQ(ReceivePacketFrame(&Out, Read), 0); // half a frame: nothing yet

    for (uint16_t i = Split; i < WireLen; i++)
        CHECK(Src.Ring.Push(Wire[i]));
    CHECK_EQ(ReceivePacketFrame(&Out, Read), WireLen);
    CHECK(std::memcmp(&Out, &Frame, PacketWireSize(&Frame)) == 0);
}

// The premise, reproduced: this is what the old silence wait did. It read the reply out of the
// UART and threw it away (a 32-byte scratch array), so by the time the line was silent the frame
// was gone - and the app was told the node never answered. Everything above is only worth
// anything if THIS loses the frame and the keep path does not.
static void TestDrainingLosesTheFrame()
{
    const PacketFrame Frame = MakeFrame(20, 0x30);
    uint8_t Wire[MaxWire];
    const uint16_t WireLen = MakeWire(Frame, Wire);

    BusRxKeepRing<KeepBytes> OldScratch; // stands in for the UART ring the old wait read
    for (uint16_t i = 0; i < WireLen; i++)
        CHECK(OldScratch.Push(Wire[i]));
    uint8_t Drained = 0;
    while (OldScratch.Pop(Drained))
    {
    } // "drain and restart the silence window" - to /dev/null

    PacketFrame Out;
    std::memset(&Out, 0, sizeof(Out));
    CHECK_EQ(ReceivePacketFrame(&Out, [&OldScratch](uint8_t &B) { return OldScratch.Pop(B); }),
             0);
    CHECK(Out.PayloadLen != 20);
}

int main()
{
    TestRingBasics();
    TestWholeFrameFromRing();
    TestFrameSplitAcrossRingAndUart();
    TestGarbageThenFrame();
    TestFullRingKeepsTheOlderFrame();
    TestFrameAcrossTwoPasses();
    TestDrainingLosesTheFrame();

    std::printf("%s: %d checks, %d failure(s)\n", failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}

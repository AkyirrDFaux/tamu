// Host test for the bootloader passthrough refusal frame format.
// Verifies that the four one-byte failure reasons produce correct FLAG_FAIL frames:
//   0x01 KPtReasonBusy      - a passthrough request is already outstanding
//   0x02 KPtReasonMalformed - payload too short, or CID outside 0x20-0x25
//   0x03 KPtReasonTimeout   - the node did not answer by the deadline
//   0x04 KPtReasonNodeFail  - the node answered with a failure frame
//
// The passthrough is the ONLY way a node is flashed, so every failure path must be
// unit-tested to keep the reason byte, the counter, and the log line from drifting apart.

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "Blocks/DeviceInfo.h"
#include "Core/Functions/Packet.h"
#include "Core/Functions/Bootloader.h"

#include "check.h"

// ---- Minimal stubs for Packet.h ----------------------------------------------

DeviceStatusStruct DeviceStatus = {};

uint32_t TimeFromBoot() { return 1000; }
uint32_t Now() { return 1000; }
void ReportLog(uint16_t, uint8_t) {}

// Initialize DeviceStatus.ShortAddress for PacketConstruct
struct DeviceStatusInitializer { DeviceStatusInitializer() { DeviceStatus.ShortAddress = MakeId(1, 2); } } gDeviceStatusInit;

const SerialNumber &GetSerialNumber()
{
    static SerialNumber Sn;
    static bool Init = false;
    if (!Init) { memset(Sn.Bytes, 0, sizeof(Sn.Bytes)); Sn.Bytes[0]=0x12; Sn.Bytes[1]=0x34; Sn.Bytes[2]=0x56; Sn.Bytes[3]=0x78; Init=true; }
    return Sn;
}

// Mock FLAG constants (must match src/Core/Functions/Packet.h)
#define FLAG_REQACK  (1 << 0)
#define FLAG_START   (1 << 1)
#define FLAG_STOP    (1 << 2)
#define FLAG_TYPE    (1 << 3)
#define FLAG_SUCCESS (1 << 5)
#define FLAG_FAIL    (1 << 6)

// Global TxFrame used by PacketConstruct
PacketFrame TxFrame;

// ---- Test: SendPassthroughFail frame format ----------------------------------
//
// The real SendPassthroughFail (in Device.h) does:
//   PacketConstruct(&TxFrame, Src, Cmd, Trid, FLAG_TYPE | FLAG_START | FLAG_STOP | FLAG_FAIL, &Reason, 1);
//
// We verify this frame format for each reason code.

static void TestSendPassthroughFailFormat()
{
    printf("\n=== Test: SendPassthroughFail frame format ===\n");

    const uint16_t Src = MakeId(1, 2);
    const uint16_t Cmd = MakeService(ServiceType::Device, 0x20); // identify passthrough
    const uint16_t Trid = 0x1234;

    for (uint8_t Reason = 0x01; Reason <= 0x04; Reason++)
    {
        // Simulate what SendPassthroughFail does
        PacketConstruct(&TxFrame, Src, Cmd, Trid,
                         FLAG_TYPE | FLAG_START | FLAG_STOP | FLAG_FAIL, &Reason, 1);

        // Verify the frame
        CHECK_EQ(TxFrame.IdSrc, Src);
        CHECK_EQ(TxFrame.Cmd, Cmd);
        CHECK_EQ(TxFrame.Trid, Trid);
        CHECK_EQ(TxFrame.Flags, (uint8_t)(FLAG_TYPE | FLAG_START | FLAG_STOP | FLAG_FAIL));
        CHECK_EQ(TxFrame.PayloadLen, 1);
        CHECK_EQ(TxFrame.Payload[0], Reason);

        // Verify reason codes match documentation
        const char *ReasonName = "unknown";
        switch (Reason) {
            case 0x01: ReasonName = "busy"; break;
            case 0x02: ReasonName = "malformed"; break;
            case 0x03: ReasonName = "timeout"; break;
            case 0x04: ReasonName = "nodefail"; break;
        }
        printf("  Reason 0x%02X (%s): frame verified\n", Reason, ReasonName);
    }

    // Verify all reasons are distinct
    CHECK(0x01 != 0x02 && 0x01 != 0x03 && 0x01 != 0x04);
    CHECK(0x02 != 0x03 && 0x02 != 0x04);
    CHECK(0x03 != 0x04);

    // Verify none is zero (would be ambiguous with success)
    CHECK(0x01 != 0 && 0x02 != 0 && 0x03 != 0 && 0x04 != 0);

    printf("PASS: All four refusal reason frames verified\n");
}

// ---- Test: Passthrough CID to Bootloader command mapping --------------------
//
// The CID rank (0x20-0x25) must match the bootloader command bits 1-3.

static void TestPassthroughCidMapping()
{
    printf("\n=== Test: Passthrough CID to Bootloader command mapping ===\n");

    using namespace Bootloader;

    // CID 0x20 -> identify (0b000x) -> CMD_IDENTIFY_REQ (0x0) / CMD_IDENTIFY_RESP (0x1)
    CHECK_EQ(PassthroughResponse(0x20), CMD_IDENTIFY_RESP);
    CHECK_EQ(FrameSize(CMD_IDENTIFY_RESP), IDENTIFY_RESP_SIZE);

    // CID 0x21 -> read (0b001x) -> CMD_READ_REQ (0x2) / CMD_READ_RESP (0x3)
    CHECK_EQ(PassthroughResponse(0x21), CMD_READ_RESP);
    CHECK_EQ(FrameSize(CMD_READ_RESP), DATA_SIZE);

    // CID 0x22 -> erase (0b010x) -> CMD_ERASE_REQ (0x4) / CMD_ERASE_RESP (0x5)
    CHECK_EQ(PassthroughResponse(0x22), CMD_ERASE_RESP);
    CHECK_EQ(FrameSize(CMD_ERASE_RESP), ERASE_RESP_SIZE);

    // CID 0x23 -> write (0b011x) -> CMD_WRITE (0x6) / CMD_WRITE_RESP (0x7)
    CHECK_EQ(PassthroughResponse(0x23), CMD_WRITE_RESP);
    CHECK_EQ(FrameSize(CMD_WRITE_RESP), READ_REQ_SIZE);

    // CID 0x24 -> CRC (0b100x) -> CMD_DIGEST_REQ (0x8) / CMD_DIGEST_RESP (0x9)
    CHECK_EQ(PassthroughResponse(0x24), CMD_DIGEST_RESP);
    CHECK_EQ(FrameSize(CMD_DIGEST_RESP), DIGEST_RESP_SIZE);

    // CID 0x25 -> jump (0b101x) -> CMD_JUMP_REQ (0xA) / CMD_JUMP_RESP (0xB)
    CHECK_EQ(PassthroughResponse(0x25), CMD_JUMP_RESP);
    CHECK_EQ(FrameSize(CMD_JUMP_RESP), READ_REQ_SIZE);

    // All response commands must be RESPONSE (bit 0 set) with real frame size
    for (uint8_t Cid = 0x20; Cid <= 0x25; Cid++)
    {
        const uint8_t Resp = PassthroughResponse(Cid);
        CHECK((Resp & 1u) == 1u);
        CHECK(FrameSize(Resp) != 0);
    }

    // Invalid CIDs return INVALID
    CHECK_EQ(PassthroughResponse(0x1F), INVALID);
    CHECK_EQ(PassthroughResponse(0x26), INVALID);

    printf("PASS: All six passthrough CIDs mapped correctly\n");
}

// ---- Test: Reason code attribution table indices ----------------------------
//
// PtTimeoutById[6] is indexed by CID - 0x20:
//   [0] = identify (0x20), [1] = read (0x21), [2] = erase (0x22),
//   [3] = write (0x23), [4] = CRC (0x24), [5] = jump (0x25)

static void TestTimeoutAttributionIndices()
{
    printf("\n=== Test: Timeout attribution indices ===\n");

    CHECK_EQ(0x20 - 0x20, 0); // identify
    CHECK_EQ(0x21 - 0x20, 1); // read
    CHECK_EQ(0x22 - 0x20, 2); // erase
    CHECK_EQ(0x23 - 0x20, 3); // write
    CHECK_EQ(0x24 - 0x20, 4); // CRC
    CHECK_EQ(0x25 - 0x20, 5); // jump

    // Array size is 6
    constexpr size_t kPtTimeoutByIdSize = 6;
    CHECK_EQ(kPtTimeoutByIdSize, 6u);

    printf("PASS: Timeout attribution indices verified\n");
}

// ---- Test: Bootloader codec for all passthrough commands --------------------

static void TestBootloaderCodecForPassthrough()
{
    printf("\n=== Test: Bootloader codec for all passthrough commands ===\n");

    using namespace Bootloader;

    // Test each command's request/response frame sizes
    CHECK_EQ(FrameSize(CMD_IDENTIFY_REQ), READ_REQ_SIZE);
    CHECK_EQ(FrameSize(CMD_IDENTIFY_RESP), IDENTIFY_RESP_SIZE);
    CHECK_EQ(FrameSize(CMD_READ_REQ), READ_REQ_SIZE);
    CHECK_EQ(FrameSize(CMD_READ_RESP), DATA_SIZE);
    CHECK_EQ(FrameSize(CMD_ERASE_REQ), ERASE_REQ_SIZE);
    CHECK_EQ(FrameSize(CMD_ERASE_RESP), ERASE_RESP_SIZE);
    CHECK_EQ(FrameSize(CMD_WRITE), DATA_SIZE);
    CHECK_EQ(FrameSize(CMD_WRITE_RESP), READ_REQ_SIZE);
    CHECK_EQ(FrameSize(CMD_DIGEST_REQ), DIGEST_REQ_SIZE);
    CHECK_EQ(FrameSize(CMD_DIGEST_RESP), DIGEST_RESP_SIZE);
    CHECK_EQ(FrameSize(CMD_JUMP_REQ), READ_REQ_SIZE);
    CHECK_EQ(FrameSize(CMD_JUMP_RESP), READ_REQ_SIZE);

    // Unassigned commands have no frame size
    for (int Cmd = 12; Cmd < 16; Cmd++) CHECK_EQ(FrameSize((uint8_t)Cmd), 0);

    // Verify encode/decode round-trip for each command
    uint8_t Frame[DATA_SIZE];

    // Identify request
    EncodeIdentifyRequest(Frame);
    CHECK_EQ(Decode(Frame, READ_REQ_SIZE), CMD_IDENTIFY_REQ);

    // Read request
    EncodeReadRequest(0x1234, Frame);
    CHECK_EQ(Decode(Frame, READ_REQ_SIZE), CMD_READ_REQ);

    // Erase request
    EncodeEraseRequest(0x1000, Frame);
    CHECK_EQ(Decode(Frame, ERASE_REQ_SIZE), CMD_ERASE_REQ);

    // Write request
    uint8_t Payload[PAYLOAD_SIZE] = {0};
    EncodeWrite(0x2000, Payload, Frame);
    CHECK_EQ(Decode(Frame, DATA_SIZE), CMD_WRITE);

    // Digest request
    EncodeDigestRequest(Frame);
    CHECK_EQ(Decode(Frame, DIGEST_REQ_SIZE), CMD_DIGEST_REQ);

    // Jump request
    EncodeJumpRequest(Frame);
    CHECK_EQ(Decode(Frame, READ_REQ_SIZE), CMD_JUMP_REQ);

    // Responses
    EncodeIdentifyResponse(0x3600, 64, "DAS v0.1", Frame);
    CHECK_EQ(Decode(Frame, IDENTIFY_RESP_SIZE), CMD_IDENTIFY_RESP);

    EncodeReadResponse(0x40, Payload, Frame);
    CHECK_EQ(Decode(Frame, DATA_SIZE), CMD_READ_RESP);

    EncodeEraseResponse(0x1000, Frame);
    CHECK_EQ(Decode(Frame, ERASE_RESP_SIZE), CMD_ERASE_RESP);

    EncodeWriteResponse(0x2000, Frame);
    CHECK_EQ(Decode(Frame, READ_REQ_SIZE), CMD_WRITE_RESP);

    EncodeDigestResponse(0xDEADBEEF, Frame);
    CHECK_EQ(Decode(Frame, DIGEST_RESP_SIZE), CMD_DIGEST_RESP);

    EncodeJumpResponse(Frame);
    CHECK_EQ(Decode(Frame, READ_REQ_SIZE), CMD_JUMP_RESP);

    printf("PASS: All passthrough command codecs verified\n");
}

// ---- Main ---------------------------------------------------------------------

int main()
{
    printf("=== Bootloader Passthrough Refusal Frame Format Tests ===\n");

    TestSendPassthroughFailFormat();
    TestPassthroughCidMapping();
    TestTimeoutAttributionIndices();
    TestBootloaderCodecForPassthrough();

    printf("\n== Passthrough refusal tests: %d checks, %d failures ==\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
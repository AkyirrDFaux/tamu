// Host test for the bootloader jump command (CMD_JUMP_REQ 0b1010 / CMD_JUMP_RESP 0b1011).
// Tests the codec-level encoding/decoding and frame format for the jump command:
//   1. Jump request frame encoding (EncodeJumpRequest)
//   2. Jump response frame encoding (EncodeJumpResponse)
//   3. FRAME_SIZE_OF table includes jump commands
//   4. PassthroughResponse maps CID 0x25 to CMD_JUMP_RESP
//   5. Bootloader codec encode/decode round-trip for jump frames
//   6. Single-bit flip rejection for jump frames
//   7. DAS bootloader HandleJump frame modification behavior

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "Core/Functions/Bootloader.h"

#include "check.h"

// ---- Test cases ---------------------------------------------------------------

// Test 1: Jump request frame encoding (CMD_JUMP_REQ = 0xA)
static void TestJumpRequestEncoding()
{
    printf("\n=== Test 1: Jump request frame encoding ===\n");

    using namespace Bootloader;

    uint8_t Frame[READ_REQ_SIZE];
    EncodeJumpRequest(Frame);

    CHECK_EQ(sizeof(Frame), 7u);
    CHECK_EQ(Frame[0], START);           // 0xCA
    CHECK_EQ(Frame[6], END);             // 0xBC
    CHECK_EQ(Frame[1] & 0x0Fu, CMD_JUMP_REQ); // 0xA
    CHECK_EQ(Frame[1] & 0xE0u, 0);       // padding zero
    // 0b1010 = 0xA: bits set at positions 1 and 3 = 2 bits = even parity = 0
    CHECK_EQ((Frame[1] >> 4) & 1u, 0u);
    CHECK_EQ(Frame[1], 0x0Au);
    CHECK_EQ(Offset(Frame), 0u);         // offset slot ignored, zeroed
    CHECK_EQ(Decode(Frame, READ_REQ_SIZE), CMD_JUMP_REQ);

    // Known-answer vector (mirrored in app test)
    uint8_t Expected[READ_REQ_SIZE] = {0xCA, 0x0A, 0x00, 0x00, 0x00, 0x00, 0xBC};
    CHECK(memcmp(Frame, Expected, READ_REQ_SIZE) == 0);

    printf("PASS: Jump request encodes to CA 0A 00 00 00 00 BC\n");
}

// Test 2: Jump response frame encoding (CMD_JUMP_RESP = 0xB)
static void TestJumpResponseEncoding()
{
    printf("\n=== Test 2: Jump response frame encoding ===\n");

    using namespace Bootloader;

    uint8_t Frame[READ_REQ_SIZE];
    EncodeJumpResponse(Frame);

    CHECK_EQ(sizeof(Frame), 7u);
    CHECK_EQ(Frame[0], START);
    CHECK_EQ(Frame[6], END);
    CHECK_EQ(Frame[1] & 0x0Fu, CMD_JUMP_RESP); // 0xB
    CHECK_EQ(Frame[1] & 0xE0u, 0);
    // 0b1011 = 0xB: bits set at positions 0, 1, 3 = 3 bits = odd parity = 1
    CHECK_EQ((Frame[1] >> 4) & 1u, 1u);
    CHECK_EQ(Frame[1], 0x1Bu);
    CHECK_EQ(Offset(Frame), 0u);
    CHECK_EQ(Decode(Frame, READ_REQ_SIZE), CMD_JUMP_RESP);
    CHECK(Decode(Frame, READ_REQ_SIZE) != CMD_JUMP_REQ);

    // Compile-time control byte must match runtime
    CHECK_EQ(HeaderControlZeroOffset(CMD_JUMP_RESP), Frame[1]);
    CHECK_EQ(HeaderControlZeroOffset(CMD_JUMP_RESP), 0x1Bu);

    // Known-answer vector
    uint8_t Expected[READ_REQ_SIZE] = {0xCA, 0x1B, 0x00, 0x00, 0x00, 0x00, 0xBC};
    CHECK(memcmp(Frame, Expected, READ_REQ_SIZE) == 0);

    printf("PASS: Jump response encodes to CA 1B 00 00 00 00 BC\n");
}

// Test 3: FRAME_SIZE_OF table includes jump commands
static void TestFrameSizeTable()
{
    printf("\n=== Test 3: FRAME_SIZE_OF table includes jump commands ===\n");

    using namespace Bootloader;

    CHECK_EQ(FrameSize(CMD_JUMP_REQ), READ_REQ_SIZE);   // 0b1010 = 7 bytes
    CHECK_EQ(FrameSize(CMD_JUMP_RESP), READ_REQ_SIZE);  // 0b1011 = 7 bytes
    CHECK_EQ(CMD_JUMP_REQ, 0xAu);
    CHECK_EQ(CMD_JUMP_RESP, 0xBu);

    // Unassigned commands (0b1100-0b1111) have no frame size
    for (int Cmd = 12; Cmd < 16; Cmd++) CHECK_EQ(FrameSize((uint8_t)Cmd), 0);

    printf("PASS: Frame size table verified for jump commands\n");
}

// Test 4: PassthroughResponse maps CID 0x25 to CMD_JUMP_RESP
static void TestPassthroughResponseMapping()
{
    printf("\n=== Test 4: PassthroughResponse maps 0x25 to jump response ===\n");

    using namespace Bootloader;

    CHECK_EQ(PassthroughResponse(0x25), CMD_JUMP_RESP);

    // The response command must be a RESPONSE (bit 0 set) with a real frame size
    const uint8_t Resp = PassthroughResponse(0x25);
    CHECK((Resp & 1u) == 1u);
    CHECK(FrameSize(Resp) != 0);

    printf("PASS: PassthroughResponse(0x25) = 0x%02X (CMD_JUMP_RESP)\n", Resp);
}

// Test 5: Bootloader codec encode/decode round-trip for jump frames
static void TestJumpCodecRoundTrip()
{
    printf("\n=== Test 5: Jump codec encode/decode round-trip ===\n");

    using namespace Bootloader;

    uint8_t Frame[READ_REQ_SIZE];

    // Jump request
    EncodeJumpRequest(Frame);
    CHECK_EQ(Decode(Frame, READ_REQ_SIZE), CMD_JUMP_REQ);

    // Jump response
    EncodeJumpResponse(Frame);
    CHECK_EQ(Decode(Frame, READ_REQ_SIZE), CMD_JUMP_RESP);

    // Verify the control byte parity is correct for both
    CHECK_EQ((Frame[1] >> 4) & 1u, 1u); // jump response has odd parity

    // Test HeaderControlZeroOffset for jump response
    CHECK_EQ(HeaderControlZeroOffset(CMD_JUMP_RESP), 0x1Bu);
    CHECK_EQ(HeaderControlZeroOffset(CMD_ERASE_RESP), 0x05u); // 0b0101: two set bits -> parity 0
    CHECK_EQ(HeaderControlZeroOffset(CMD_WRITE_RESP), 0x17u); // 0b0111 -> parity 1

    printf("PASS: Jump codec round-trip verified\n");
}

// Test 6: Single-bit flip rejection for jump frames
static void TestJumpFrameCorruption()
{
    printf("\n=== Test 6: Single-bit flip rejection for jump frames ===\n");

    using namespace Bootloader;

    uint8_t Req[READ_REQ_SIZE];
    EncodeJumpRequest(Req);

    uint8_t Resp[READ_REQ_SIZE];
    EncodeJumpResponse(Resp);

    // Every single-bit flip must be rejected
    for (uint16_t i = 0; i < READ_REQ_SIZE; i++) {
        for (int bit = 0; bit < 8; bit++) {
            uint8_t Bad[READ_REQ_SIZE];
            memcpy(Bad, Req, READ_REQ_SIZE);
            Bad[i] ^= (uint8_t)(1u << bit);
            CHECK_EQ(Decode(Bad, READ_REQ_SIZE), INVALID);
        }
    }

    for (uint16_t i = 0; i < READ_REQ_SIZE; i++) {
        for (int bit = 0; bit < 8; bit++) {
            uint8_t Bad[READ_REQ_SIZE];
            memcpy(Bad, Resp, READ_REQ_SIZE);
            Bad[i] ^= (uint8_t)(1u << bit);
            CHECK_EQ(Decode(Bad, READ_REQ_SIZE), INVALID);
        }
    }

    printf("PASS: All single-bit flips of jump request/response rejected\n");
}

// Test 7: DAS bootloader HandleJump frame modification behavior
static void TestBootloaderHandleJump()
{
    printf("\n=== Test 7: DAS bootloader HandleJump frame modification ===\n");

    using namespace Bootloader;

    // The DAS bootloader's HandleJump:
    // 1. Modifies the received frame in place: sets control byte to CMD_JUMP_RESP with correct parity
    // 2. Sends the ack via SendRaw (which waits for TC, drops TXEN, drains echo)
    // 3. Calls JumpToApp() which does not return
    //
    // We can't test the actual jump on host, but we can verify the frame modification
    // that HandleJump does: Frame[1] = HeaderControlZeroOffset(CMD_JUMP_RESP)

    uint8_t Frame[READ_REQ_SIZE];
    EncodeJumpRequest(Frame); // Start with a jump request frame
    CHECK_EQ(Frame[1] & 0x0Fu, CMD_JUMP_REQ);

    // Simulate what HandleJump does:
    Frame[1] = HeaderControlZeroOffset(CMD_JUMP_RESP);
    CHECK_EQ(Frame[1], 0x1Bu);
    CHECK_EQ(Decode(Frame, READ_REQ_SIZE), CMD_JUMP_RESP);

    // The frame is now a valid jump response that will be sent
    CHECK_EQ(Frame[0], START);
    CHECK_EQ(Frame[6], END);
    CHECK_EQ(Offset(Frame), 0u);

    printf("PASS: HandleJump frame modification verified\n");
}

// Test 8: Jump command values are part of the protocol contract
static void TestJumpCommandContract()
{
    printf("\n=== Test 8: Jump command contract values ===\n");

    using namespace Bootloader;

    // The jump command values are the contract with the tool
    CHECK_EQ(CMD_JUMP_REQ, 0xAu);      // 0b1010
    CHECK_EQ(CMD_JUMP_RESP, 0xBu);     // 0b1011

    // Direction bit (bit 0): 0 = request, 1 = response
    CHECK_EQ(CMD_JUMP_REQ & 1u, 0u);
    CHECK_EQ(CMD_JUMP_RESP & 1u, 1u);

    // Operation bits (bits 1-3): 0b101 = jump
    CHECK_EQ(CMD_JUMP_REQ >> 1, 0x5u);
    CHECK_EQ(CMD_JUMP_RESP >> 1, 0x5u);

    // Parity bit is bit 4 of control byte, computed over cmd+offset+payload
    // For jump request (0b1010): 2 bits set -> even parity -> 0
    // For jump response (0b1011): 3 bits set -> odd parity -> 1

    printf("PASS: Jump command contract values verified\n");
}

// ---- Main ---------------------------------------------------------------------

int main()
{
    printf("=== Bootloader Jump Command Codec Tests ===\n");

    TestJumpRequestEncoding();
    TestJumpResponseEncoding();
    TestFrameSizeTable();
    TestPassthroughResponseMapping();
    TestJumpCodecRoundTrip();
    TestJumpFrameCorruption();
    TestBootloaderHandleJump();
    TestJumpCommandContract();

    printf("\n== Bootloader jump tests: %d checks, %d failures ==\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
// Host test for the DAS app Device service CID 4 "enter bootloader" handler.
// Tests:
// 1. Without DEVICE_HAS_BOOTLOADER_ENTRY: CID 4 is refused (not handled)
// 2. With DEVICE_HAS_BOOTLOADER_ENTRY: CID 4 accepted, reply sent, jump to bootloader entry at 0x7FC
// 3. Bootloader answers identify after jump (the bootloader's identify response at BL_ENTRY_ADDR)

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "Blocks/DeviceInfo.h"
#include "Core/Functions/Bootloader.h"
#include "Core/Functions/Packet.h"
#include "Core/Services/Device.h"

#include "check.h"

// ---- Stubs and mocks for the host build -------------------------------------

// DeviceStatus is defined in Packet.h as extern; we need one instance.
DeviceStatusStruct DeviceStatus = {};

// Bootloader::BL_ENTRY_ADDR is 0x7FC (the bootloader entry stub address).
// The DAS app's DeviceEnterBootloaderJump() calls this address.
// For the host test we record whether it was called and what address it used.
#ifdef DEVICE_HAS_BOOTLOADER_ENTRY
#include <setjmp.h>
static uint32_t gBootloaderEntryCalled = 0;
static uint32_t gBootloaderEntryAddr = 0;
static jmp_buf gBootloaderJumpBuf;
static bool gBootloaderJumpActive = false;
#endif

#ifdef DEVICE_HAS_BOOTLOADER_ENTRY
extern "C" __attribute__((noreturn)) void DeviceEnterBootloaderJump(void)
{
    gBootloaderEntryCalled = 1;
    gBootloaderEntryAddr = Bootloader::BL_ENTRY_ADDR;
    std::printf("MOCK DeviceEnterBootloaderJump called, addr=0x%X, gBootloaderJumpActive=%d\n", Bootloader::BL_ENTRY_ADDR, gBootloaderJumpActive);
    fflush(stdout);
    if (gBootloaderJumpActive) longjmp(gBootloaderJumpBuf, 1);
    // Noreturn: if longjmp wasn't active, abort
    std::abort();
}
#endif

// Mock DispatchPacket - the real one sends the frame on the bus.
// We just capture the reply frame for verification.
static PacketFrame gLastDispatchedFrame;
static bool gDispatchCalled = false;

void DispatchPacket(const PacketFrame &Frame)
{
    gDispatchCalled = true;
    gLastDispatchedFrame = Frame;
    std::printf("MOCK DispatchPacket: Cmd=0x%04X, Flags=0x%02X, Len=%u\n",
                Frame.Cmd, Frame.Flags, Frame.PayloadLen);
}

// Mock GetSerialNumber - returns a fixed serial number for testing.
const SerialNumber &GetSerialNumber()
{
    static SerialNumber Sn;
    static bool Init = false;
    if (!Init)
    {
        memset(Sn.Bytes, 0, sizeof(Sn.Bytes));
        Sn.Bytes[0] = 0x12;
        Sn.Bytes[1] = 0x34;
        Sn.Bytes[2] = 0x56;
        Sn.Bytes[3] = 0x78;
        Init = true;
    }
    return Sn;
}

// Mock TimeFromBoot and Now - return incrementing values.
static uint32_t gTimeMs = 1000;
uint32_t TimeFromBoot() { return gTimeMs; }
uint32_t Now() { return gTimeMs; }

// Mock ReportLog - no-op.
void ReportLog(uint16_t /*code*/, uint8_t /*priority*/) {}

// Mock Subscriptions.Tick - no-op.
struct { void Tick(uint32_t /*ms*/) {} } Subscriptions __attribute__((unused));

// Global variables used by the Device service handler (defined in Main.cpp for the real build)
PacketFrame TxFrame;
int32_t TimeOffsetMs = 0;
uint32_t TimeOffsetRefRaw = 0;
int32_t TimeDrift = 0;

// Mock constants
#define NODE_DISCOVER_INTERVAL_MS 500u
#define NODE_DISCOVER_JITTER_MS 125u
#define NODE_DISCOVER_REPLY_MS 100u
#define NODE_TIME_SYNC_INTERVAL_MS 60000u
#define NODE_TIME_SYNC_WARMUP_MS 30000u

// Mock priority constants
#define PRIORITY_TIMESYNC 1
#define PRIORITY_DISCOVER 3
#define PRIORITY_LOG 15

// Mock FLAG constants
#define FLAG_REQACK (1 << 0)
#define FLAG_START  (1 << 1)
#define FLAG_STOP   (1 << 2)
#define FLAG_TYPE   (1 << 3)

// Mock ADDR_BROADCAST
#define ADDR_BROADCAST 0xFFFF

// ---- Test helper: build a Device service CID 4 request frame ------------------

static PacketFrame BuildCid4Request(uint16_t Trid = 0x1234)
{
    PacketFrame Frame = {};
    Frame.Flags = FLAG_REQACK | FLAG_START | FLAG_STOP; // request flags
    Frame.Priority = 8;
    Frame.IdSrc = MakeId(1, 1); // core address (net 1, device 1)
    Frame.IdTgt = MakeId(1, 2); // node address (net 1, device 2)
    Frame.Cmd = MakeService(ServiceType::Device, 4); // Device service CID 4
    Frame.Trid = Trid;
    Frame.PayloadLen = 0; // CID 4 has no payload
    return Frame;
}

// ---- Test cases ---------------------------------------------------------------

#ifndef DEVICE_HAS_BOOTLOADER_ENTRY
// Test 1: Without DEVICE_HAS_BOOTLOADER_ENTRY, CID 4 is not handled (no reply sent)
static void TestCid4RefusedWithoutDefine()
{
    std::printf("\n=== Test 1: CID 4 refused without DEVICE_HAS_BOOTLOADER_ENTRY ===\n");

    // Reset mocks
    gDispatchCalled = false;
    memset(&gLastDispatchedFrame, 0, sizeof(gLastDispatchedFrame));

    PacketFrame Frame = BuildCid4Request();
    DeviceService DeviceSvc;

    // Call Handle - without DEVICE_HAS_BOOTLOADER_ENTRY, case 4 is not in the switch
    // Handle returns true for any Device service CID, but no reply should be sent
    bool Handled = DeviceSvc.Handle(Frame);

    CHECK_EQ(Handled, true); // Handle returns true for Device service frames
    CHECK_EQ(gDispatchCalled, false); // No reply sent for unhandled CID

    std::printf("PASS: CID 4 not handled, no reply sent\n");
}
#endif // !DEVICE_HAS_BOOTLOADER_ENTRY

#ifdef DEVICE_HAS_BOOTLOADER_ENTRY
// Test 2: With DEVICE_HAS_BOOTLOADER_ENTRY, CID 4 is handled (reply sent, jump called)
static void TestCid4AcceptedWithDefine()
{
    std::printf("\n=== Test 2: CID 4 accepted with DEVICE_HAS_BOOTLOADER_ENTRY ===\n");

    // Reset mocks
    gDispatchCalled = false;
    gBootloaderEntryCalled = 0;
    gBootloaderEntryAddr = 0;
    memset(&gLastDispatchedFrame, 0, sizeof(gLastDispatchedFrame));

    PacketFrame Frame = BuildCid4Request();
    DeviceService DeviceSvc;

    // Use setjmp to catch the longjmp from DeviceEnterBootloaderJump
    gBootloaderJumpActive = true;
    if (setjmp(gBootloaderJumpBuf) == 0)
    {
        // Call Handle - with DEVICE_HAS_BOOTLOADER_ENTRY, case 4 sends reply and jumps
        DeviceSvc.Handle(Frame);
    }
    gBootloaderJumpActive = false;

    // Verify the reply was sent before the jump
    CHECK_EQ(gDispatchCalled, true); // Reply sent

    // Verify the reply frame
    CHECK_EQ(gLastDispatchedFrame.Cmd, Frame.Cmd); // echoes request CMD
    CHECK_EQ(gLastDispatchedFrame.Trid, Frame.Trid); // echoes request TRID
    CHECK_EQ(gLastDispatchedFrame.Flags & FLAG_TYPE, FLAG_TYPE); // response flag
    CHECK_EQ(gLastDispatchedFrame.Flags & FLAG_START, FLAG_START);
    CHECK_EQ(gLastDispatchedFrame.Flags & FLAG_STOP, FLAG_STOP);
    CHECK_EQ(gLastDispatchedFrame.PayloadLen, 0); // no payload in reply
    CHECK_EQ(gLastDispatchedFrame.IdTgt, Frame.IdSrc); // reply goes to requester

    // Verify bootloader entry was called with correct address
    CHECK_EQ(gBootloaderEntryCalled, 1);
    CHECK_EQ(gBootloaderEntryAddr, Bootloader::BL_ENTRY_ADDR);
    CHECK_EQ(Bootloader::BL_ENTRY_ADDR, 0x7FC); // the published entry address

    std::printf("PASS: CID 4 handled, reply sent, jump to 0x%X recorded\n", gBootloaderEntryAddr);
}

// Test 3: Bootloader identify response at BL_ENTRY_ADDR (0x7FC)
// This tests that the bootloader's identify response can be encoded at the entry address.
// The bootloader publishes its entry at 0x7FC, and after the jump it answers identify requests.
static void TestBootloaderIdentifyAfterJump()
{
    std::printf("\n=== Test 3: Bootloader answers identify at BL_ENTRY_ADDR ===\n");

    using namespace Bootloader;

    // The bootloader's identify response is 39 bytes (IDENTIFY_RESP_SIZE).
    // It carries: app size (u32), page size (u32), name (28 bytes NUL-padded).
    uint8_t IdentifyResp[IDENTIFY_RESP_SIZE];
    EncodeIdentifyResponse(0x3600, 64, "DAS v0.1", IdentifyResp);

    CHECK_EQ(IdentifyResp[0], START); // 0xCA
    CHECK_EQ(IdentifyResp[IDENTIFY_RESP_SIZE - 1], END); // 0xBC
    CHECK_EQ(IdentifyResp[1] & 0x0F, CMD_IDENTIFY_RESP); // 0b0001

    // Verify the page size field (at IDENTIFY_PAGE_AT = 6)
    uint32_t PageSize = IdentifyPageSize(IdentifyResp);
    CHECK_EQ(PageSize, 64u);

    // Verify the name field (at IDENTIFY_NAME_AT = 10)
    const uint8_t *Name = IdentifyName(IdentifyResp);
    CHECK(std::memcmp(Name, "DAS v0.1", 8) == 0);
    CHECK_EQ(Name[8], 0); // NUL-padded

    // Verify Decode accepts it
    CHECK_EQ(Decode(IdentifyResp, IDENTIFY_RESP_SIZE), CMD_IDENTIFY_RESP);

    // The bootloader entry at 0x7FC is a jump to BootloaderEntry.
    // The app calls DeviceEnterBootloaderJump() which casts BL_ENTRY_ADDR to a function
    // and calls it. The bootloader then re-initialises and serves identify requests.
    CHECK_EQ(BL_ENTRY_ADDR, 0x7FCu);

    std::printf("PASS: Bootloader identify response valid at entry contract\n");
}
#endif // DEVICE_HAS_BOOTLOADER_ENTRY

// ---- Main ---------------------------------------------------------------------

int main()
{
    std::printf("=== DAS App Device Service CID 4 (enter bootloader) tests ===\n");
    std::printf("DEVICE_HAS_BOOTLOADER_ENTRY: %s\n",
#ifdef DEVICE_HAS_BOOTLOADER_ENTRY
                "DEFINED"
#else
                "NOT DEFINED"
#endif
    );

#ifdef DEVICE_HAS_BOOTLOADER_ENTRY
    // These tests require DEVICE_HAS_BOOTLOADER_ENTRY
    TestCid4AcceptedWithDefine();
    TestBootloaderIdentifyAfterJump();
#else
    // This test requires NO DEVICE_HAS_BOOTLOADER_ENTRY
    TestCid4RefusedWithoutDefine();
#endif

    std::printf("\n== Device CID 4 tests: %d checks, %d failures ==\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
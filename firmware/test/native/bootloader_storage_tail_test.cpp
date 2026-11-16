// Host test for the bootloader's storage tail boundary (CodeSize refusal).
// The DAS bootloader protects the last 512 B of the app region (the app's own storage:
// .TABLE/.SV file table) by refusing writes and erases at or past CodeSize (0x3600).
// This was added in commit bdc6d8a and tested on hardware; this test provides native coverage.

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "Core/Functions/Bootloader.h"

#include "check.h"

namespace
{
    constexpr uint32_t kAppSize = 0x3800;  // the DAS upload window (APP_LIMIT - APP_BASE)
    constexpr uint32_t kCodeSize = 0x3600; // the code space inside the window (storage above)

    // A NOR flash model (programming only clears bits) driven by the real bootloader rule.
    struct NorFlash
    {
        static uint8_t Bytes[kAppSize];
        static int Erases;
        static int Unlocks;
        static int Locks;
        static bool EraseTakesEffect;

        static void Reset()
        {
            memset(Bytes, 0xFF, sizeof(Bytes));
            Erases = Unlocks = Locks = 0;
            EraseTakesEffect = true;
        }

        static bool ErasePage(uint32_t Offset)
        {
            Erases++;
            const uint32_t Page = Offset & ~(Bootloader::PAGE_SIZE - 1u);
            if (EraseTakesEffect) memset(Bytes + Page, 0xFF, Bootloader::PAGE_SIZE);
            return FirstWord(Page) == 0xFFFFFFFFu;
        }

        static void ProgramWord(uint32_t Offset, uint32_t Data)
        {
            for (uint32_t i = 0; i < 4; i++)
                Bytes[Offset + i] &= (uint8_t)(Data >> (8u * i));
        }

        static uint32_t ReadWord(uint32_t Offset)
        {
            uint32_t Word;
            memcpy(&Word, Bytes + Offset, sizeof(Word));
            return Word;
        }

        static void Unlock() { Unlocks++; }
        static void Lock() { Locks++; }

        static uint32_t FirstWord(uint32_t Offset)
        {
            uint32_t Word;
            memcpy(&Word, Bytes + (Offset & ~(Bootloader::PAGE_SIZE - 1u)), sizeof(Word));
            return Word;
        }

        static bool Holds(uint32_t Offset, const uint8_t *Src, uint32_t Len)
        {
            return memcmp(Bytes + Offset, Src, Len) == 0;
        }

        static bool AllErased(uint32_t Offset, uint32_t Len)
        {
            for (uint32_t i = 0; i < Len; i++)
                if (Bytes[Offset + i] != 0xFF) return false;
            return true;
        }
    };

    uint8_t NorFlash::Bytes[kAppSize];
    int NorFlash::Erases = 0;
    int NorFlash::Unlocks = 0;
    int NorFlash::Locks = 0;
    bool NorFlash::EraseTakesEffect = true;

    uint32_t LastWritten = 0xFFFFFFFFu;

    // The node's write handler: calls the real WriteChunk/WriteAck rule.
    uint32_t HandleWrite(uint32_t Offset, const uint8_t *Payload)
    {
        const Bootloader::ChunkResult R =
            Bootloader::WriteChunk<NorFlash>(Offset, Payload, kAppSize, kCodeSize, LastWritten);
        return Bootloader::WriteAck(R, Offset, LastWritten);
    }

    // The node's erase handler: calls the real ErasePage rule.
    uint32_t HandleErase(uint32_t Offset)
    {
        return Bootloader::ErasePage<NorFlash>(Offset, kCodeSize);
    }

    void Pattern(uint8_t *Out, uint32_t Offset, uint8_t Seed)
    {
        for (uint32_t i = 0; i < Bootloader::PAYLOAD_SIZE; i++)
            Out[i] = (uint8_t)(Seed + Offset + i * 7u);
    }

    // The page a response's offset names (bit 0 = failure flag).
    uint32_t AckPage(uint32_t Ack) { return Ack & ~1u; }
} // namespace

int main()
{
    using namespace Bootloader;
    uint8_t Chunk[PAYLOAD_SIZE];

    // -------------------------------------------------------------------------
    // 1. Write AT CodeSize (0x3600) -> expect refusal with page flagged (0x3601)
    // -------------------------------------------------------------------------
    NorFlash::Reset();
    LastWritten = kCodeSize - PAGE_SIZE; // previous chunk landed at last code page
    {
        Pattern(Chunk, kCodeSize, 0x10);
        const uint32_t Ack = HandleWrite(kCodeSize, Chunk);

        // Refused: ack is the page (0x3600) with failure flag (bit 0) -> 0x3601
        CHECK(Ack != kCodeSize);                    // not an ack of the chunk
        CHECK_EQ(Ack, kCodeSize | 1u);              // the PAGE, flagged
        CHECK_EQ(AckPage(Ack), kCodeSize);
        CHECK(NorFlash::AllErased(kCodeSize, PAGE_SIZE)); // nothing programmed
        CHECK_EQ(LastWritten, kCodeSize - PAGE_SIZE);     // progress did not move
        CHECK_EQ(NorFlash::Erases, 0);                     // no flash touched
        std::printf("PASS: write at CodeSize (0x3600) refused with page 0x3601\n");
    }

    // -------------------------------------------------------------------------
    // 2. Write at CodeSize - 32 (0x35E0) -> expect SUCCESS (last chunk of code space)
    // -------------------------------------------------------------------------
    NorFlash::Reset();
    LastWritten = kCodeSize - PAGE_SIZE; // page start chunk already written
    {
        // Erase the last code page first
        const uint32_t LastCodePage = kCodeSize - PAGE_SIZE;
        CHECK_EQ(HandleErase(LastCodePage), LastCodePage);
        CHECK(NorFlash::AllErased(LastCodePage, PAGE_SIZE));

        // Write the page-start chunk (already done in setup, LastWritten = LastCodePage)
        // Now write the mid-page chunk at CodeSize - 32
        Pattern(Chunk, kCodeSize - PAYLOAD_SIZE, 0x20);
        const uint32_t Ack = HandleWrite(kCodeSize - PAYLOAD_SIZE, Chunk);

        // Success: ack is the chunk offset
        CHECK_EQ(Ack, kCodeSize - PAYLOAD_SIZE);
        CHECK(NorFlash::Holds(kCodeSize - PAYLOAD_SIZE, Chunk, PAYLOAD_SIZE));
        CHECK_EQ(LastWritten, kCodeSize - PAYLOAD_SIZE);
        std::printf("PASS: write at CodeSize-32 (0x35E0) succeeds\n");
    }

    // -------------------------------------------------------------------------
    // 3. Erase AT CodeSize (0x3600) -> expect refusal with page flagged (0x3601)
    // -------------------------------------------------------------------------
    NorFlash::Reset();
    {
        const uint32_t Ack = HandleErase(kCodeSize);

        // Refused: page address with failure flag
        CHECK_EQ(Ack, kCodeSize | 1u);
        CHECK_EQ(AckPage(Ack), kCodeSize);
        CHECK(NorFlash::AllErased(kCodeSize, kAppSize - kCodeSize)); // nothing above erased
        CHECK_EQ(NorFlash::Erases, 0);                                // no erase ran
        std::printf("PASS: erase at CodeSize (0x3600) refused with page 0x3601\n");
    }

    // -------------------------------------------------------------------------
    // 4. Verify the bound is EXACTLY at CodeSize
    //    - Page at CodeSize - PAGE_SIZE (0x35C0) accepts both chunks
    //    - Page at CodeSize (0x3600) refuses both chunks
    //    - Page at CodeSize + PAGE_SIZE (0x3640) refuses both chunks
    // -------------------------------------------------------------------------
    NorFlash::Reset();
    LastWritten = 0xFFFFFFFFu;
    {
        // The last code page (0x35C0 - 0x35FF) must accept its two chunks
        const uint32_t LastCodePage = kCodeSize - PAGE_SIZE;
        CHECK_EQ(HandleErase(LastCodePage), LastCodePage);

        Pattern(Chunk, LastCodePage, 0x30);
        CHECK_EQ(HandleWrite(LastCodePage, Chunk), LastCodePage);

        Pattern(Chunk, LastCodePage + PAYLOAD_SIZE, 0x30);
        CHECK_EQ(HandleWrite(LastCodePage + PAYLOAD_SIZE, Chunk), LastCodePage + PAYLOAD_SIZE);

        // The storage page at CodeSize (0x3600) refuses its page-start chunk
        Pattern(Chunk, kCodeSize, 0x40);
        CHECK_EQ(HandleWrite(kCodeSize, Chunk), kCodeSize | 1u);
        CHECK(NorFlash::AllErased(kCodeSize, PAGE_SIZE));

        // The storage page at CodeSize refuses its mid-page chunk too
        Pattern(Chunk, kCodeSize + PAYLOAD_SIZE, 0x40);
        CHECK_EQ(HandleWrite(kCodeSize + PAYLOAD_SIZE, Chunk), kCodeSize | 1u);
        CHECK(NorFlash::AllErased(kCodeSize, PAGE_SIZE));

        // The next storage page (0x3640) also refuses
        Pattern(Chunk, kCodeSize + PAGE_SIZE, 0x50);
        CHECK_EQ(HandleWrite(kCodeSize + PAGE_SIZE, Chunk), (kCodeSize + PAGE_SIZE) | 1u);
        CHECK(NorFlash::AllErased(kCodeSize + PAGE_SIZE, PAGE_SIZE));

        Pattern(Chunk, kCodeSize + PAGE_SIZE + PAYLOAD_SIZE, 0x50);
        CHECK_EQ(HandleWrite(kCodeSize + PAGE_SIZE + PAYLOAD_SIZE, Chunk), (kCodeSize + PAGE_SIZE) | 1u);
        CHECK(NorFlash::AllErased(kCodeSize + PAGE_SIZE, PAGE_SIZE));

        std::printf("PASS: bound verified exactly at CodeSize (0x3600)\n");
    }

    // -------------------------------------------------------------------------
    // 5. Erase bound: last code page erases, storage page refuses
    // -------------------------------------------------------------------------
    NorFlash::Reset();
    {
        const uint32_t LastCodePage = kCodeSize - PAGE_SIZE;
        CHECK_EQ(HandleErase(LastCodePage), LastCodePage);          // last code page: OK
        CHECK(NorFlash::AllErased(LastCodePage, PAGE_SIZE));
        CHECK_EQ(NorFlash::Erases, 1);

        CHECK_EQ(HandleErase(kCodeSize), kCodeSize | 1u);           // storage page: refused
        CHECK_EQ(HandleErase(kCodeSize + 0x40u), (kCodeSize + 0x40u) | 1u); // misaligned in storage
        CHECK(NorFlash::AllErased(kCodeSize, kAppSize - kCodeSize)); // nothing above erased
        CHECK_EQ(NorFlash::Erases, 1);                               // only the real erase ran

        std::printf("PASS: erase bound exactly at CodeSize\n");
    }

    std::printf("\n== bootloader storage tail boundary tests: %d checks, %d failures ==\n",
                checks, failures);
    return failures == 0 ? 0 : 1;
}
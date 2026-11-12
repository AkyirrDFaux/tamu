// Host test for the bootloader's chunk writer (Core/Functions/Bootloader.h, card t_5195d4bc).
//
// The device's rule is thin and easy to get wrong in exactly one way: an upload sends 32-byte
// chunks in order, so the SECOND chunk of every 64-byte page lands in a page the first chunk just
// programmed. This pins the two decisions that make that work:
//
//   * the erase decision is POSITIONAL - only the chunk whose offset starts a page erases it, and
//     it erases it whatever the page holds; a chunk inside a page never erases;
//   * the ack rule is the verification - the response names the offset only when the 32 bytes read
//     back as sent, so a chunk that landed in a slot nobody erased is refused instead of acked.
//
// The model below is NOR, like the chip: a program only clears bits, so a chunk written onto a slot
// that was never erased reads back different. The test drives the REAL rule - WriteChunk is the
// function the node runs; only the register access behind the adapter is modelled here.

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "Core/Functions/Bootloader.h"

#include "check.h"

namespace
{
    constexpr uint32_t kAppSize = 0x3800; // the DAS upload window (APP_LIMIT - APP_BASE)

    // A NOR flash seen through the offsets the bootloader uses (0 == APP_BASE). Static state, like
    // fake_flash.h: WriteChunk<Flash> calls the adapter's members on the type.
    struct NorFlash
    {
        static uint8_t Bytes[kAppSize];
        static int Erases;
        static int Unlocks;
        static int Locks;
        static bool EraseTakesEffect; // false models an erase the controller declines to perform

        static void Reset()
        {
            memset(Bytes, 0xFF, sizeof(Bytes));
            Erases = Unlocks = Locks = 0;
            EraseTakesEffect = true;
        }

        // The device's page erase reports the read-back of the page's first word, not its intent.
        static bool ErasePage(uint32_t Offset)
        {
            Erases++;
            if (EraseTakesEffect) memset(Bytes + (Offset & ~(Bootloader::PAGE_SIZE - 1u)), 0xFF, Bootloader::PAGE_SIZE);
            return FirstWord(Offset) == 0xFFFFFFFFu;
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
        static void Fill(uint32_t Offset, const uint8_t *Src, uint32_t Len) { memcpy(Bytes + Offset, Src, Len); }
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

    uint32_t Last = 0xFFFFFFFFu;

    // The node's write handler, less the response frame: this IS the rule under test.
    bool HandleWrite(uint32_t Offset, const uint8_t *Payload)
    {
        return Bootloader::WriteChunk<NorFlash>(Offset, Payload, kAppSize, Last);
    }

    void Pattern(uint8_t *Out, uint32_t Offset, uint8_t Seed)
    {
        for (uint32_t i = 0; i < Bootloader::PAYLOAD_SIZE; i++)
            Out[i] = (uint8_t)(Seed + Offset + i * 7u);
    }
} // namespace

int main()
{
    using namespace Bootloader;
    uint8_t Chunk[PAYLOAD_SIZE];

    // --- 1. Two whole pages, four 32-byte chunks in order ---------------------
    // Four chunks over 128 bytes: the two page-start chunks erase, the two inside the pages must
    // not, or each would take the chunk that just landed in the other half with it.
    NorFlash::Reset();
    Last = 0xFFFFFFFFu;
    {
        bool Acked[4] = {false, false, false, false};
        for (uint32_t n = 0; n < 4; n++)
        {
            Pattern(Chunk, n * PAYLOAD_SIZE, 0x10);
            Acked[n] = HandleWrite(n * PAYLOAD_SIZE, Chunk);
            CHECK_EQ(Last, n * PAYLOAD_SIZE); // the ack names the chunk it wrote
        }
        CHECK(Acked[0] && Acked[1] && Acked[2] && Acked[3]);

        // All 128 bytes are in flash, byte for byte.
        bool All = true;
        for (uint32_t n = 0; n < 4; n++)
        {
            Pattern(Chunk, n * PAYLOAD_SIZE, 0x10);
            All = All && NorFlash::Holds(n * PAYLOAD_SIZE, Chunk, PAYLOAD_SIZE);
        }
        CHECK(All);

        // Exactly one erase per page - the positional rule, and no extra erase for the second half.
        CHECK_EQ(NorFlash::Erases, 2);
        // Every chunk left the controller locked behind it.
        CHECK_EQ(NorFlash::Unlocks, 4);
        CHECK_EQ(NorFlash::Locks, 4);
    }

    // --- 2. A page that already holds an image is replaced --------------------
    // The re-flash case the defect was measured on: the page holds an old image and the chunk that
    // starts it must erase it, not skip it because it is not blank.
    NorFlash::Reset();
    Last = 0xFFFFFFFFu;
    {
        uint8_t Old[64];
        for (uint32_t i = 0; i < 64; i++) Old[i] = (uint8_t)(0xA0 + i);
        NorFlash::Fill(0, Old, sizeof(Old));

        Pattern(Chunk, 0, 0x30);
        CHECK(HandleWrite(0, Chunk));
        CHECK_EQ(Last, 0u);
        CHECK(NorFlash::Holds(0, Chunk, PAYLOAD_SIZE));
        // The page's other 32 bytes were erased by the page erase, not left holding the old image.
        CHECK(NorFlash::AllErased(PAYLOAD_SIZE, PAYLOAD_SIZE));
        CHECK_EQ(NorFlash::Erases, 1);
    }

    // --- 3. An erase that does not take effect refuses the chunk --------------
    // The measured defect's shape: the page keeps its old content, so the chunk did not land and
    // must not be acked (the response has to keep the previous offset).
    NorFlash::Reset();
    {
        uint8_t Old[64];
        memset(Old, 0x5A, sizeof(Old));
        NorFlash::Fill(0, Old, sizeof(Old));
        NorFlash::EraseTakesEffect = false;
        Last = 0x1234u;

        Pattern(Chunk, 0, 0x40);
        CHECK(!HandleWrite(0, Chunk));
        CHECK_EQ(Last, 0x1234u);                   // unchanged: the tool sees it was not acked
        CHECK(NorFlash::Holds(0, Old, PAYLOAD_SIZE)); // and nothing was written either
    }

    // --- 4. A chunk inside a blank page programs and does not erase -----------
    NorFlash::Reset();
    Last = 0xFFFFFFFFu;
    {
        Pattern(Chunk, PAYLOAD_SIZE, 0x50);
        CHECK(HandleWrite(PAYLOAD_SIZE, Chunk)); // offset 32: inside page 0
        CHECK_EQ(Last, PAYLOAD_SIZE);
        CHECK(NorFlash::Holds(PAYLOAD_SIZE, Chunk, PAYLOAD_SIZE));
        CHECK_EQ(NorFlash::Erases, 0);              // a mid-page chunk never erases
        CHECK(NorFlash::AllErased(0, PAYLOAD_SIZE)); // the first half is untouched
    }

    // --- 5. A chunk inside a page that holds other data is refused ------------
    // The other case the tool has to be able to tell apart: the 32 bytes cannot be programmed onto
    // what is there, so the write is refused instead of being acked over bytes that never changed.
    NorFlash::Reset();
    {
        uint8_t Old[64];
        for (uint32_t i = 0; i < 64; i++) Old[i] = (uint8_t)(i * 3u);
        NorFlash::Fill(0, Old, sizeof(Old));
        Last = 0x00C0u;

        Pattern(Chunk, PAYLOAD_SIZE, 0x60);
        CHECK(!HandleWrite(PAYLOAD_SIZE, Chunk));
        CHECK_EQ(Last, 0x00C0u); // refused with the previous offset, not acked
        CHECK(!NorFlash::Holds(PAYLOAD_SIZE, Chunk, PAYLOAD_SIZE)); // the bytes did not become it
        CHECK_EQ(NorFlash::Erases, 0);
    }

    // --- 6. A re-sent identical chunk is acked (the app's retry round) --------
    // Programming only clears bits, so re-writing the same bytes verifies: a retransmission must not
    // be reported as a lost chunk.
    NorFlash::Reset();
    Last = 0xFFFFFFFFu;
    {
        Pattern(Chunk, PAYLOAD_SIZE, 0x60);
        CHECK(HandleWrite(PAYLOAD_SIZE, Chunk));
        CHECK(HandleWrite(PAYLOAD_SIZE, Chunk)); // the same chunk again
        CHECK_EQ(Last, PAYLOAD_SIZE);
        CHECK(NorFlash::Holds(PAYLOAD_SIZE, Chunk, PAYLOAD_SIZE));
    }

    // --- 7. Misaligned and out-of-range chunks touch nothing ------------------
    NorFlash::Reset();
    {
        Last = 0x0040u;
        Pattern(Chunk, 0, 0x70);

        CHECK(!HandleWrite(0x10u, Chunk));                 // not 32-byte aligned
        CHECK(!HandleWrite(kAppSize - 16u, Chunk));        // 16 bytes left: does not fit
        CHECK(!HandleWrite(kAppSize, Chunk));              // exactly at the end
        CHECK(!HandleWrite(0xFFFFFFF8u, Chunk));           // wraps if the bound is written as a sum
        CHECK_EQ(Last, 0x0040u);
        CHECK_EQ(NorFlash::Erases, 0);
        CHECK(NorFlash::AllErased(0, sizeof(NorFlash::Bytes))); // no flash was touched at all

        CHECK(HandleWrite(kAppSize - PAYLOAD_SIZE, Chunk)); // the last chunk does fit
        CHECK_EQ(Last, kAppSize - PAYLOAD_SIZE);
    }

    // --- 8. A page-start chunk erases even when the page is already blank -----
    // The erase decision must not be content-based: skipping a blank page would make the rule depend
    // on what the page holds, which is the class of bug the card removed.
    NorFlash::Reset();
    Last = 0xFFFFFFFFu;
    {
        Pattern(Chunk, 2 * PAGE_SIZE, 0x80);
        CHECK(HandleWrite(2 * PAGE_SIZE, Chunk));
        CHECK_EQ(NorFlash::Erases, 1);
        CHECK(NorFlash::Holds(2 * PAGE_SIZE, Chunk, PAYLOAD_SIZE));
    }

    // --- 9. One unlock and one lock per accepted chunk -----------------------
    NorFlash::Reset();
    Last = 0xFFFFFFFFu;
    {
        Pattern(Chunk, 4 * PAGE_SIZE, 0x90);
        CHECK(HandleWrite(4 * PAGE_SIZE, Chunk));
        Pattern(Chunk, 4 * PAGE_SIZE + PAYLOAD_SIZE, 0x90);
        CHECK(HandleWrite(4 * PAGE_SIZE + PAYLOAD_SIZE, Chunk));
        CHECK_EQ(NorFlash::Unlocks, 2); // one unlock per chunk
        CHECK_EQ(NorFlash::Locks, 2);   // and the controller is left locked
    }

    std::printf("== bootloader write-path tests (erase decision + ack rule): %d checks, %d failures ==\n",
                checks, failures);
    return failures == 0 ? 0 : 1;
}

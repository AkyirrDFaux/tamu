// Host test for the bootloader's chunk writer (Core/Functions/Bootloader.h, cards t_5195d4bc and
// t_cb8bd722).
//
// The device's rule is thin and easy to get wrong in exactly one way: an upload sends 32-byte
// chunks in order, so the SECOND chunk of every 64-byte page lands in a page the first chunk
// programmed - and the erase unit is the whole 64-byte page. This pins the decisions that make that
// work, and the one that keeps a lost write from leaving a hole:
//
//   * the erase decision is POSITIONAL - only the chunk whose offset starts a page erases it, and
//     it erases it whatever the page holds; a chunk inside a page never erases (E84);
//   * a chunk inside a page is only programmed when that page's first half is known to be there
//     (it follows it directly, or the half already holds data), so the E85 shape - a page-start
//     request that never arrived - is REFUSED and answered with the page address instead of
//     leaving the page half blank;
//   * the ack rule is the verification - the response names the chunk only when the 32 bytes read
//     back as sent, names the PAGE (bit 0 set) when the page could not be written, and keeps the
//     previous offset when nothing was touched.
//
// The model below is NOR, like the chip: a program only clears bits, so a chunk written onto a slot
// that was never erased reads back different. The test drives the REAL rule - WriteChunk and WriteAck
// are the functions the node runs; only the register access behind the adapter is modelled here.

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

    // The node's write handler, less the response frame: this IS the rule under test, and the value
    // it returns is exactly what the write response carries - the chunk when it landed, the PAGE
    // with bit 0 set when the page could not be written, the previous offset when nothing was
    // touched. A tool reads it as an ack only when it names the chunk it asked for.
    uint32_t HandleWrite(uint32_t Offset, const uint8_t *Payload)
    {
        const Bootloader::ChunkResult R =
            Bootloader::WriteChunk<NorFlash>(Offset, Payload, kAppSize, Last);
        return Bootloader::WriteAck(R, Offset, Last);
    }

    void Pattern(uint8_t *Out, uint32_t Offset, uint8_t Seed)
    {
        for (uint32_t i = 0; i < Bootloader::PAYLOAD_SIZE; i++)
            Out[i] = (uint8_t)(Seed + Offset + i * 7u);
    }

    // The page a response's offset names, for the "it answered with the PAGE" assertions.
    uint32_t AckPage(uint32_t Ack) { return Ack & ~1u; }
} // namespace

int main()
{
    using namespace Bootloader;
    uint8_t Chunk[PAYLOAD_SIZE];

    // --- 1. Two whole pages, four 32-byte chunks in order ---------------------
    // Four chunks over 128 bytes: the two page-start chunks erase, the two inside the pages must
    // not, or each would take the chunk that landed in the other half with it.
    NorFlash::Reset();
    Last = 0xFFFFFFFFu;
    {
        bool Acked[4] = {false, false, false, false};
        for (uint32_t n = 0; n < 4; n++)
        {
            Pattern(Chunk, n * PAYLOAD_SIZE, 0x10);
            const uint32_t Ack = HandleWrite(n * PAYLOAD_SIZE, Chunk);
            CHECK_EQ(Ack, n * PAYLOAD_SIZE); // the ack names the chunk it wrote
            Acked[n] = Ack == n * PAYLOAD_SIZE;
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

    // --- 2. A page that already holds an image is replaced (E84) --------------
    // The re-flash case the defect was measured on: the page holds an old image and the chunk that
    // starts it must erase it, not skip it because it is not blank.
    NorFlash::Reset();
    Last = 0xFFFFFFFFu;
    {
        uint8_t Old[64];
        for (uint32_t i = 0; i < 64; i++) Old[i] = (uint8_t)(0xA0 + i);
        NorFlash::Fill(0, Old, sizeof(Old));

        Pattern(Chunk, 0, 0x30);
        CHECK_EQ(HandleWrite(0, Chunk), 0u);
        CHECK(NorFlash::Holds(0, Chunk, PAYLOAD_SIZE));
        // The page's other 32 bytes were erased by the page erase, not left holding the old image.
        CHECK(NorFlash::AllErased(PAYLOAD_SIZE, PAYLOAD_SIZE));
        CHECK_EQ(NorFlash::Erases, 1);

        Pattern(Chunk, PAYLOAD_SIZE, 0x30);
        CHECK_EQ(HandleWrite(PAYLOAD_SIZE, Chunk), PAYLOAD_SIZE);
        CHECK(NorFlash::Holds(PAYLOAD_SIZE, Chunk, PAYLOAD_SIZE));
        CHECK_EQ(NorFlash::Erases, 1); // a chunk inside the page never erases
    }

    // --- 3. An erase that does not take effect refuses the chunk --------------
    // The measured defect's shape: the page keeps its old content, so the chunk did not land and
    // must not be acked - and the refusal names the PAGE, with bit 0 set so that a failed page-start
    // chunk (whose page address is its own offset) cannot be read as a success.
    NorFlash::Reset();
    {
        uint8_t Old[64];
        memset(Old, 0x5A, sizeof(Old));
        NorFlash::Fill(0, Old, sizeof(Old));
        NorFlash::EraseTakesEffect = false;
        Last = 0x1234u;

        Pattern(Chunk, 0, 0x40);
        const uint32_t Ack = HandleWrite(0, Chunk);
        CHECK(Ack != 0u);                    // NOT acked: the tool sees a failure
        CHECK_EQ(Ack, 0x1u);                 // the PAGE (0) with the failure flag
        CHECK_EQ(AckPage(Ack), 0u);
        CHECK_EQ(Last, 0x1234u);             // the acknowledged progress did not move
        CHECK(NorFlash::Holds(0, Old, PAYLOAD_SIZE)); // and nothing was written either
    }

    // --- 4. A chunk inside a page whose first half never arrived is refused ---
    // The E85 shape, on a blank page: the page-start request was lost, so programming the second
    // half alone would leave 32 bytes of FF in the image. It must be refused with the page address
    // and touch no flash at all.
    NorFlash::Reset();
    Last = 0xFFFFFFFFu;
    {
        Pattern(Chunk, PAYLOAD_SIZE, 0x50);
        const uint32_t Ack = HandleWrite(PAYLOAD_SIZE, Chunk);
        CHECK(Ack != PAYLOAD_SIZE);       // not acked as this chunk
        CHECK_EQ(Ack, 0x1u);              // the page address (0) with the failure flag
        CHECK_EQ(NorFlash::Erases, 0);    // a chunk inside a page never erases
        CHECK(NorFlash::AllErased(0, PAGE_SIZE)); // and nothing was programmed into it
        CHECK_EQ(Last, 0xFFFFFFFFu);      // the tool is told nothing has landed
    }

    // --- 5. A chunk inside a page that holds other data is refused ------------
    // The page's first half holds data (so this is the correction-round shape), but the 32 bytes
    // cannot be programmed onto what is there: refused with the page address, and no erase.
    NorFlash::Reset();
    {
        uint8_t Old[64];
        for (uint32_t i = 0; i < 64; i++) Old[i] = (uint8_t)(i * 3u);
        NorFlash::Fill(0, Old, sizeof(Old));
        Last = 0x00C0u;

        Pattern(Chunk, PAYLOAD_SIZE, 0x60);
        const uint32_t Ack = HandleWrite(PAYLOAD_SIZE, Chunk);
        CHECK(Ack != PAYLOAD_SIZE);          // refused
        CHECK_EQ(Ack, 0x1u);                 // with the page it could not write
        CHECK(!NorFlash::Holds(PAYLOAD_SIZE, Chunk, PAYLOAD_SIZE)); // the bytes did not become it
        CHECK_EQ(NorFlash::Erases, 0);
    }

    // --- 6. A re-sent identical chunk is acked (the app's retry round) --------
    // Programming only clears bits, so re-writing the same bytes verifies: a retransmission of a
    // chunk must not be reported as a lost chunk (the second half of a written page is accepted
    // because its first half holds data, which is the correction-round case too).
    NorFlash::Reset();
    Last = 0xFFFFFFFFu;
    {
        Pattern(Chunk, 0, 0x60);
        CHECK_EQ(HandleWrite(0, Chunk), 0u);
        Pattern(Chunk, PAYLOAD_SIZE, 0x60);
        CHECK_EQ(HandleWrite(PAYLOAD_SIZE, Chunk), PAYLOAD_SIZE);
        CHECK_EQ(HandleWrite(PAYLOAD_SIZE, Chunk), PAYLOAD_SIZE); // the same chunk again
        CHECK(NorFlash::Holds(PAYLOAD_SIZE, Chunk, PAYLOAD_SIZE));
    }

    // --- 7. Misaligned and out-of-range chunks touch nothing ------------------
    NorFlash::Reset();
    {
        Last = 0x0040u;
        Pattern(Chunk, 0, 0x70);

        CHECK_EQ(HandleWrite(0x10u, Chunk), 0x0040u);        // not 32-byte aligned
        CHECK_EQ(HandleWrite(kAppSize - 16u, Chunk), 0x0040u); // 16 bytes left: does not fit
        CHECK_EQ(HandleWrite(kAppSize, Chunk), 0x0040u);     // exactly at the end
        CHECK_EQ(HandleWrite(0xFFFFFFF8u, Chunk), 0x0040u);  // wraps if the bound is written as a sum
        CHECK_EQ(NorFlash::Erases, 0);
        CHECK(NorFlash::AllErased(0, sizeof(NorFlash::Bytes))); // no flash was touched at all

        // The last page of the region: its first half, then the chunk that ends it.
        Pattern(Chunk, kAppSize - PAGE_SIZE, 0x70);
        CHECK_EQ(HandleWrite(kAppSize - PAGE_SIZE, Chunk), kAppSize - PAGE_SIZE);
        Pattern(Chunk, kAppSize - PAYLOAD_SIZE, 0x70);
        CHECK_EQ(HandleWrite(kAppSize - PAYLOAD_SIZE, Chunk), kAppSize - PAYLOAD_SIZE); // it fits
    }

    // --- 8. A page-start chunk erases even when the page is already blank -----
    // The erase decision must not be content-based: skipping a blank page would make the rule depend
    // on what the page holds, which is the class of bug the card removed.
    NorFlash::Reset();
    Last = 0xFFFFFFFFu;
    {
        Pattern(Chunk, 2 * PAGE_SIZE, 0x80);
        CHECK_EQ(HandleWrite(2 * PAGE_SIZE, Chunk), 2 * PAGE_SIZE);
        CHECK_EQ(NorFlash::Erases, 1);
        CHECK(NorFlash::Holds(2 * PAGE_SIZE, Chunk, PAYLOAD_SIZE));
    }

    // --- 9. One unlock and one lock per accepted chunk -----------------------
    NorFlash::Reset();
    Last = 0xFFFFFFFFu;
    {
        Pattern(Chunk, 4 * PAGE_SIZE, 0x90);
        CHECK_EQ(HandleWrite(4 * PAGE_SIZE, Chunk), 4 * PAGE_SIZE);
        Pattern(Chunk, 4 * PAGE_SIZE + PAYLOAD_SIZE, 0x90);
        CHECK_EQ(HandleWrite(4 * PAGE_SIZE + PAYLOAD_SIZE, Chunk), 4 * PAGE_SIZE + PAYLOAD_SIZE);
        CHECK_EQ(NorFlash::Unlocks, 2); // one unlock per chunk
        CHECK_EQ(NorFlash::Locks, 2);   // and the controller is left locked
    }

    // --- 10. The E85 repair path: a lost page start is named, then fixed ------
    // The measured rig shape end to end, on the fresh-chip case: the page-start request is lost, so
    // the page's first half is blank. The second half must be refused with the page address and
    // must not be programmed - that is the 32 bytes of FF E85 measured - and re-sending the page's
    // first chunk must then put the whole page in.
    NorFlash::Reset();
    {
        Last = 0x0060u; // the chunk before this page landed; this page's start never arrived

        Pattern(Chunk, 0x80 + PAYLOAD_SIZE, 0x20);
        const uint32_t Ack = HandleWrite(0x80 + PAYLOAD_SIZE, Chunk);
        CHECK_EQ(Ack, 0x81u);              // refused, naming the page at 0x80
        CHECK_EQ(AckPage(Ack), 0x80u);
        CHECK(NorFlash::AllErased(0x80, PAGE_SIZE)); // nothing was programmed: no half-written page

        Pattern(Chunk, 0x80, 0x20);        // re-send the page's first chunk, then the second
        CHECK_EQ(HandleWrite(0x80, Chunk), 0x80u);
        Pattern(Chunk, 0x80 + PAYLOAD_SIZE, 0x20);
        CHECK_EQ(HandleWrite(0x80 + PAYLOAD_SIZE, Chunk), 0x80 + PAYLOAD_SIZE);

        uint8_t New[64];
        Pattern(New, 0x80, 0x20);
        Pattern(New + PAYLOAD_SIZE, 0x80 + PAYLOAD_SIZE, 0x20);
        CHECK(NorFlash::Holds(0x80, New, sizeof(New))); // the page is the image, both halves
        CHECK_EQ(NorFlash::Erases, 1);                  // one positional erase, at the page start
    }

    // --- 11. A page holding an older image is not left half blank either ------
    // The re-flash shape of the same loss: the page holds an old image, the page-start request was
    // lost. The lone second half cannot be programmed over what is there, so it is refused with the
    // page address - the tool is told which page to re-send, instead of the run moving on over a
    // page that is now half old, half missing.
    NorFlash::Reset();
    {
        uint8_t Old[64];
        for (uint32_t i = 0; i < 64; i++) Old[i] = (uint8_t)(0x11 + i);
        NorFlash::Fill(0x80, Old, sizeof(Old));
        Last = 0x0060u;

        Pattern(Chunk, 0x80 + PAYLOAD_SIZE, 0x20);
        const uint32_t Ack = HandleWrite(0x80 + PAYLOAD_SIZE, Chunk);
        CHECK_EQ(Ack, 0x81u);              // refused, naming the page at 0x80
        CHECK(!NorFlash::Holds(0x80 + PAYLOAD_SIZE, Chunk, PAYLOAD_SIZE)); // the half did not land
        CHECK_EQ(NorFlash::Erases, 0);     // and no chunk inside a page erased it

        // Re-sending the page's first chunk replaces the whole page, old image and all.
        Pattern(Chunk, 0x80, 0x20);
        CHECK_EQ(HandleWrite(0x80, Chunk), 0x80u);
        Pattern(Chunk, 0x80 + PAYLOAD_SIZE, 0x20);
        CHECK_EQ(HandleWrite(0x80 + PAYLOAD_SIZE, Chunk), 0x80 + PAYLOAD_SIZE);

        uint8_t New[64];
        Pattern(New, 0x80, 0x20);
        Pattern(New + PAYLOAD_SIZE, 0x80 + PAYLOAD_SIZE, 0x20);
        CHECK(NorFlash::Holds(0x80, New, sizeof(New)));
    }

    std::printf("== bootloader write-path tests (page rule + erase decision + ack rule): %d checks, %d failures ==\n",
                checks, failures);
    return failures == 0 ? 0 : 1;
}

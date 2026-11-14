// Host test for the bootloader's chunk writer and page eraser (Core/Functions/Bootloader.h,
// protocol v2: the erase is its own command).
//
// The rule the node runs is thin, and v2 split one thing in two:
//
//   * WriteChunk only PROGRAMS. It no longer erases anything - a page is blanked by the ERASE
//     command (ErasePage) - so a lost write costs one re-sent write and can never leave a page
//     blank, which is the E85 shape the erase-in-write rule could not remove;
//   * the one page check that remains runs on a PAGE-START chunk only (offset % PAGE_SIZE == 0):
//     the page's first word must read 0xFFFFFFFF, or the chunk is REFUSED with the PAGE named.
//     It catches a tool writing into a page it never erased. A chunk INSIDE a page is never
//     checked, because by then its page's first word is the one the upload just programmed;
//   * the ack rule is the verification: the response names the chunk only when its 32 bytes read
//     back as sent, names the PAGE (bit 0 set) when the page could not be written, and keeps the
//     previous offset when nothing was touched;
//   * ErasePage blanks the page its offset NAMES - the offset must be page-aligned, and a
//     misaligned one is refused with the flag (never rounded into the page below it) - and answers
//     with the page, with bit 0 set when the page is not blank afterwards (a failed erase, an
//     offset past the region, or a misaligned offset, which is answered with its own value).
//
// The model below is NOR, like the chip: programming only clears bits, so a chunk written onto a
// slot that was not blank reads back different. The test drives the REAL rule - WriteChunk,
// WriteAck and ErasePage are the functions the node runs; only the register access behind the
// adapter is modelled here.

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "Core/Functions/Bootloader.h"

#include "check.h"

namespace
{
    constexpr uint32_t kAppSize = 0x3800; // the DAS upload window (APP_LIMIT - APP_BASE)
    constexpr uint32_t kCodeSize = 0x3600; // what an erase is allowed to reach (storage above)

    // A NOR flash seen through the offsets the bootloader uses (0 == APP_BASE). Static state, like
    // fake_flash.h: the rule calls the adapter's members on the type.
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
        static void Fill(uint32_t Offset, const uint8_t *Src, uint32_t Len) { memcpy(Bytes + Offset, Src, Len); }
        static void FillByte(uint32_t Offset, uint8_t Value, uint32_t Len) { memset(Bytes + Offset, Value, Len); }
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
    // it returns is exactly what the write response carries.
    uint32_t HandleWrite(uint32_t Offset, const uint8_t *Payload)
    {
        const Bootloader::ChunkResult R =
            Bootloader::WriteChunk<NorFlash>(Offset, Payload, kAppSize, Last);
        return Bootloader::WriteAck(R, Offset, Last);
    }

    // The node's erase handler, less the response frame: the page it blanked, bit 0 = not blank.
    uint32_t HandleErase(uint32_t Offset) { return Bootloader::ErasePage<NorFlash>(Offset, kCodeSize); }

    void Pattern(uint8_t *Out, uint32_t Offset, uint8_t Seed)
    {
        for (uint32_t i = 0; i < Bootloader::PAYLOAD_SIZE; i++)
            Out[i] = (uint8_t)(Seed + Offset + i * 7u);
    }

    // The page a response's offset names, for the "it answered with the PAGE" assertions.
    uint32_t AckPage(uint32_t Ack) { return Ack & ~1u; }

    // A whole 64-byte page of image bytes, from the two chunk patterns.
    void Page(uint8_t *Out, uint32_t PageOffset, uint8_t Seed)
    {
        Pattern(Out, PageOffset, Seed);
        Pattern(Out + Bootloader::PAYLOAD_SIZE, PageOffset + Bootloader::PAYLOAD_SIZE, Seed);
    }
} // namespace

int main()
{
    using namespace Bootloader;
    uint8_t Chunk[PAYLOAD_SIZE];

    // --- 1. The v2 upload shape: erase the page, write both chunks, no erase in the writes ---
    NorFlash::Reset();
    Last = 0xFFFFFFFFu;
    {
        CHECK_EQ(HandleErase(0), 0u);            // the page, unflagging
        CHECK(NorFlash::AllErased(0, PAGE_SIZE));
        CHECK_EQ(NorFlash::Erases, 1);

        Pattern(Chunk, 0, 0x10);
        CHECK_EQ(HandleWrite(0, Chunk), 0u);     // the page-start chunk
        Pattern(Chunk, PAYLOAD_SIZE, 0x10);
        CHECK_EQ(HandleWrite(PAYLOAD_SIZE, Chunk), PAYLOAD_SIZE);

        // The write path erased nothing: the only erase is the command's.
        CHECK_EQ(NorFlash::Erases, 1);

        uint8_t Whole[64];
        Page(Whole, 0, 0x10);
        CHECK(NorFlash::Holds(0, Whole, sizeof(Whole)));
    }

    // --- 2. Two pages in order: one erase each, in front of its chunks -------------
    NorFlash::Reset();
    Last = 0xFFFFFFFFu;
    {
        for (uint32_t p = 0; p < 2; p++)
        {
            const uint32_t Base = p * PAGE_SIZE;
            CHECK_EQ(HandleErase(Base), Base);
            Pattern(Chunk, Base, 0x20);
            CHECK_EQ(HandleWrite(Base, Chunk), Base);
            Pattern(Chunk, Base + PAYLOAD_SIZE, 0x20);
            CHECK_EQ(HandleWrite(Base + PAYLOAD_SIZE, Chunk), Base + PAYLOAD_SIZE);
        }
        CHECK_EQ(NorFlash::Erases, 2);
        CHECK_EQ(NorFlash::Unlocks, 4); // one unlock per chunk still
        CHECK_EQ(NorFlash::Locks, 4);

        uint8_t Whole[64];
        for (uint32_t p = 0; p < 2; p++)
        {
            Page(Whole, p * PAGE_SIZE, 0x20);
            CHECK(NorFlash::Holds(p * PAGE_SIZE, Whole, sizeof(Whole)));
        }
    }

    // --- 3. A lost write is re-sent as a write - nothing is erased ----------------
    // The whole point of the split: the page was erased on purpose, so chunk 1 going missing costs
    // one re-sent chunk. Under the erase-in-write rule the page would have been blank until a
    // re-sent page start (E85).
    NorFlash::Reset();
    Last = 0xFFFFFFFFu;
    {
        CHECK_EQ(HandleErase(0), 0u);
        Pattern(Chunk, 0, 0x30);
        CHECK_EQ(HandleWrite(0, Chunk), 0u);
        // chunk 1 is "lost" (never sent), then re-sent
        Pattern(Chunk, PAYLOAD_SIZE, 0x30);
        CHECK_EQ(HandleWrite(PAYLOAD_SIZE, Chunk), PAYLOAD_SIZE);
        CHECK_EQ(NorFlash::Erases, 1);
        uint8_t Whole[64];
        Page(Whole, 0, 0x30);
        CHECK(NorFlash::Holds(0, Whole, sizeof(Whole)));
    }

    // --- 4. A page-start chunk into a page that still holds data is REFUSED -------
    // The tool skipped the erase (lost ack, or an uploader that assumes the old behaviour). The
    // chunk must not program into dirty bits, and the answer names the page so the tool erases it.
    NorFlash::Reset();
    {
        uint8_t Old[64];
        for (uint32_t i = 0; i < 64; i++) Old[i] = (uint8_t)(0xA0 + i);
        NorFlash::Fill(0, Old, sizeof(Old));
        Last = 0x1234u;

        Pattern(Chunk, 0, 0x40);
        const uint32_t Ack = HandleWrite(0, Chunk);
        CHECK(Ack != 0u);                            // not an ack
        CHECK_EQ(Ack, 0x1u);                         // the PAGE (0) with the failure flag
        CHECK_EQ(AckPage(Ack), 0u);
        CHECK_EQ(Last, 0x1234u);                     // progress did not move
        CHECK(NorFlash::Holds(0, Old, PAYLOAD_SIZE)); // and the page is untouched
        CHECK_EQ(NorFlash::Erases, 0);               // the write did not erase it either

        // The tool's recovery: erase, then write the page again.
        CHECK_EQ(HandleErase(0), 0u);
        CHECK_EQ(HandleWrite(0, Chunk), 0u);
    }

    // --- 5. A chunk INSIDE a page is never checked ---------------------------------
    // Its page's first word is whatever this upload programmed there, so a content check on it
    // would refuse every second chunk of every page. Proof: a page whose first word is 0x00000000
    // (not blank) still accepts a chunk in its second half.
    NorFlash::Reset();
    {
        NorFlash::FillByte(0, 0x00, PAYLOAD_SIZE); // the page's first half holds data
        Last = 0xFFFFFFFFu;

        Pattern(Chunk, PAYLOAD_SIZE, 0x50);
        CHECK_EQ(HandleWrite(PAYLOAD_SIZE, Chunk), PAYLOAD_SIZE); // landed, not checked
        CHECK(NorFlash::Holds(PAYLOAD_SIZE, Chunk, PAYLOAD_SIZE));
        CHECK_EQ(NorFlash::Erases, 0);

        // The same offsets, one page start: there the check DOES run and refuses.
        Pattern(Chunk, 0, 0x50);
        CHECK_EQ(HandleWrite(0, Chunk), 0x1u);
    }

    // --- 6. A chunk that cannot be programmed onto what is there is refused -------
    // No erase, no verify-by-proof: the read-back disagrees, so the ack names the page.
    NorFlash::Reset();
    {
        uint8_t Old[64];
        for (uint32_t i = 0; i < 64; i++) Old[i] = (uint8_t)(i * 3u);
        NorFlash::Fill(0, Old, sizeof(Old));
        Last = 0x00C0u;

        Pattern(Chunk, PAYLOAD_SIZE, 0x60);
        const uint32_t Ack = HandleWrite(PAYLOAD_SIZE, Chunk);
        CHECK(Ack != PAYLOAD_SIZE);
        CHECK_EQ(Ack, 0x1u);                 // the page it could not write
        CHECK(!NorFlash::Holds(PAYLOAD_SIZE, Chunk, PAYLOAD_SIZE));
        CHECK_EQ(NorFlash::Erases, 0);
    }

    // --- 7. Retries: a mid-page chunk re-sends freely, a page start is refused -----
    // A re-sent mid-page chunk is never checked, so it verifies and is acked. A re-sent PAGE-START
    // chunk is NOT: its page is no longer blank (this upload programmed it), so the check refuses
    // it and names the page - the tool's recovery is erase + both chunks, which is exactly what v2
    // asks of an uploader and what the erase-in-write rule could not express.
    NorFlash::Reset();
    Last = 0xFFFFFFFFu;
    {
        CHECK_EQ(HandleErase(0), 0u);
        Pattern(Chunk, 0, 0x70);
        CHECK_EQ(HandleWrite(0, Chunk), 0u);
        Pattern(Chunk, PAYLOAD_SIZE, 0x70);
        CHECK_EQ(HandleWrite(PAYLOAD_SIZE, Chunk), PAYLOAD_SIZE);
        CHECK_EQ(HandleWrite(PAYLOAD_SIZE, Chunk), PAYLOAD_SIZE); // the same chunk again: acked
        CHECK(NorFlash::Holds(PAYLOAD_SIZE, Chunk, PAYLOAD_SIZE));

        Pattern(Chunk, 0, 0x70);
        CHECK_EQ(HandleWrite(0, Chunk), 0x1u); // the page is not blank any more: refused
        uint8_t Whole[64];
        Page(Whole, 0, 0x70);
        CHECK(NorFlash::Holds(0, Whole, sizeof(Whole))); // and nothing was damaged by the refusal
    }

    // --- 8. Misaligned and out-of-range chunks touch nothing ----------------------
    NorFlash::Reset();
    {
        Last = 0x0040u;
        Pattern(Chunk, 0, 0x80);

        CHECK_EQ(HandleWrite(0x10u, Chunk), 0x0040u);          // not 32-byte aligned
        CHECK_EQ(HandleWrite(kAppSize - 16u, Chunk), 0x0040u);  // 16 bytes left: does not fit
        CHECK_EQ(HandleWrite(kAppSize, Chunk), 0x0040u);        // exactly at the end
        CHECK_EQ(HandleWrite(0xFFFFFFF8u, Chunk), 0x0040u);     // wraps if the bound is a sum
        CHECK(NorFlash::AllErased(0, sizeof(NorFlash::Bytes))); // no flash was touched

        // The last CODE page of the region: erase it, then its two chunks. (The page above it is
        // the storage page, which an erase refuses.)
        const uint32_t LastCodePage = kCodeSize - PAGE_SIZE;
        CHECK_EQ(HandleErase(LastCodePage), LastCodePage);
        Pattern(Chunk, LastCodePage, 0x80);
        CHECK_EQ(HandleWrite(LastCodePage, Chunk), LastCodePage);
        Pattern(Chunk, LastCodePage + PAYLOAD_SIZE, 0x80);
        CHECK_EQ(HandleWrite(LastCodePage + PAYLOAD_SIZE, Chunk), LastCodePage + PAYLOAD_SIZE);
    }

    // --- 9. ErasePage: the page, the alignment refusal, the bound, a failed erase ----
    NorFlash::Reset();
    {
        // A page's own base offset names it, and it is blanked.
        CHECK_EQ(HandleErase(PAGE_SIZE), PAGE_SIZE);
        CHECK(NorFlash::AllErased(PAGE_SIZE, PAGE_SIZE));
        CHECK_EQ(NorFlash::Erases, 1);

        // A misaligned offset is REFUSED with the flag and blanks nothing - never rounded down
        // into the page below it. An erase is the one destructive command, so a tool computing a
        // bad offset has to see the refusal instead of a page it never asked about.
        NorFlash::FillByte(PAGE_SIZE * 2u, 0x5A, PAGE_SIZE);
        CHECK_EQ(HandleErase(PAGE_SIZE * 2u + 5u), (PAGE_SIZE * 2u + 5u) | 1u);
        CHECK_EQ(NorFlash::Erases, 1);                                     // no erase ran
        CHECK_EQ(*(NorFlash::Bytes + PAGE_SIZE * 2u), 0x5A);                // untouched, not blanked
        CHECK_EQ(HandleErase(1u), 1u | 1u);                                // the smallest misalignment
        CHECK_EQ(HandleErase(PAGE_SIZE - 1u), (PAGE_SIZE - 1u) | 1u);
        CHECK_EQ(NorFlash::Erases, 1);

        // The last code page is inside the bound; the storage page above it is not, and is not
        // touched: a tool cannot wipe the app's own file table through an erase.
        const uint32_t LastCodePage = kCodeSize - PAGE_SIZE;
        CHECK_EQ(HandleErase(LastCodePage), LastCodePage);
        CHECK_EQ(HandleErase(kCodeSize), kCodeSize | 1u);            // the storage page: refused
        CHECK_EQ(HandleErase(kCodeSize + 0x40u), (kCodeSize + 0x40u) | 1u);
        CHECK(NorFlash::AllErased(kCodeSize, kAppSize - kCodeSize)); // nothing above was erased
        CHECK_EQ(NorFlash::Erases, 2);                               // only the two real erases ran

        // An erase the controller declines to perform is answered with the flag, so the tool does
        // not go on to write into a page that still holds data.
        NorFlash::EraseTakesEffect = false;
        NorFlash::FillByte(PAGE_SIZE * 3u, 0x00, PAGE_SIZE);
        CHECK_EQ(HandleErase(PAGE_SIZE * 3u), PAGE_SIZE * 3u | 1u);
        NorFlash::EraseTakesEffect = true;
    }

    // --- 10. The delta upload: one dirty page re-erased, the clean one kept ------
    // Two pages hold an image; page 1 differs. The tool erases page 1 and writes its two chunks;
    // page 0 must keep every byte and must not be erased.
    NorFlash::Reset();
    Last = 0xFFFFFFFFu;
    {
        for (uint32_t p = 0; p < 2; p++)
        {
            CHECK_EQ(HandleErase(p * PAGE_SIZE), p * PAGE_SIZE);
            Pattern(Chunk, p * PAGE_SIZE, 0x90);
            CHECK_EQ(HandleWrite(p * PAGE_SIZE, Chunk), p * PAGE_SIZE);
            Pattern(Chunk, p * PAGE_SIZE + PAYLOAD_SIZE, 0x90);
            CHECK_EQ(HandleWrite(p * PAGE_SIZE + PAYLOAD_SIZE, Chunk), p * PAGE_SIZE + PAYLOAD_SIZE);
        }
        uint8_t Clean[64];
        Page(Clean, 0, 0x90);
        CHECK(NorFlash::Holds(0, Clean, sizeof(Clean)));

        const int ErasesAfterFirstPass = NorFlash::Erases;
        // The delta: page 1 changes.
        CHECK_EQ(HandleErase(PAGE_SIZE), PAGE_SIZE);
        Pattern(Chunk, PAGE_SIZE, 0xA0);
        CHECK_EQ(HandleWrite(PAGE_SIZE, Chunk), PAGE_SIZE);
        Pattern(Chunk, PAGE_SIZE + PAYLOAD_SIZE, 0xA0);
        CHECK_EQ(HandleWrite(PAGE_SIZE + PAYLOAD_SIZE, Chunk), PAGE_SIZE + PAYLOAD_SIZE);

        CHECK_EQ(NorFlash::Erases, ErasesAfterFirstPass + 1); // exactly one more erase
        CHECK(NorFlash::Holds(0, Clean, sizeof(Clean)));      // page 0 untouched
        uint8_t Changed[64];
        Page(Changed, PAGE_SIZE, 0xA0);
        CHECK(NorFlash::Holds(PAGE_SIZE, Changed, sizeof(Changed)));
    }

    // --- 11. A page holding an older image is replaced by erase + write ----------
    // The re-flash case: the page holds an old image and the tool erases it first.
    NorFlash::Reset();
    {
        uint8_t Old[64];
        for (uint32_t i = 0; i < 64; i++) Old[i] = (uint8_t)(0x11 + i);
        NorFlash::Fill(0x80, Old, sizeof(Old));
        Last = 0xFFFFFFFFu;

        CHECK_EQ(HandleErase(0x80), 0x80u);
        CHECK(NorFlash::AllErased(0x80, PAGE_SIZE));
        Pattern(Chunk, 0x80, 0x20);
        CHECK_EQ(HandleWrite(0x80, Chunk), 0x80u);
        Pattern(Chunk, 0x80 + PAYLOAD_SIZE, 0x20);
        CHECK_EQ(HandleWrite(0x80 + PAYLOAD_SIZE, Chunk), 0x80 + PAYLOAD_SIZE);

        uint8_t New[64];
        Page(New, 0x80, 0x20);
        CHECK(NorFlash::Holds(0x80, New, sizeof(New)));
    }

    std::printf("== bootloader write-path tests (v2: erase command + page-start check + ack rule): "
                "%d checks, %d failures ==\n",
                checks, failures);
    return failures == 0 ? 0 : 1;
}

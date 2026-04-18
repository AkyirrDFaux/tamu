namespace HW
{
    void FlashInitDevice();
    void FlashInit();
    // Raw functions
    int FlashRead(uint32_t Offset, void *Buffer, uint32_t Length);
    int FlashWrite(uint32_t Offset, const void *Buffer, uint32_t Length);
    int FlashErase(uint32_t Offset, uint32_t Length);
    uint16_t GapPointer = 0; // Tracks our "Current" position in the circle

    typedef struct
    {
        uint8_t Status;   // 0xFF (Empty), 0xEE (Valid), 0x88 (Stale), 0x00 (Erased),
        uint8_t Checksum; // CRC8
        uint16_t Length;
        union // This is start of Baseclass.Compress() (Net/first byte ignored)
        {
            uint16_t ID;
            struct
            {
                uint8_t GroupID;
                uint8_t DeviceID;
            };
        };
        uint8_t Type;
        uint8_t Flags;
    } __attribute__((packed)) MemoryHeader;

#if defined BOARD_Tamu_v1_0 || defined BOARD_Tamu_v2_0
#include "esp_partition.h"
#define PAGE_SIZE 4096         // Hardware limitation
#define SEGMENT_SIZE PAGE_SIZE // Custom segmentation, must be multiple of page size (incl. 1)
    uint32_t FlashSize = 0;
    uint16_t *FreeSpace = nullptr;
    static const esp_partition_t *Partition = NULL;

    void FlashInitDevice()
    {
        Partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                             ESP_PARTITION_SUBTYPE_DATA_SPIFFS,
                                             "storage");
        FlashSize = Partition->size;
        // FlashSize = PAGE_SIZE * 4;
        FreeSpace = new uint16_t[FlashSize / SEGMENT_SIZE];
    }

    // Generic Raw Read
    int FlashRead(uint32_t Offset, void *Buffer, uint32_t Length)
    {
        return esp_partition_read(Partition, Offset, Buffer, Length) == ESP_OK ? 0 : -1;
    }

    // Generic Raw Write
    int FlashWrite(uint32_t Offset, const void *Buffer, uint32_t Length)
    {
        return esp_partition_write(Partition, Offset, Buffer, Length) == ESP_OK ? 0 : -1;
    }

    bool FlashFormat()
    {
        ESP_LOGW("FLASH", "Performing Full Partition Erase...");
        // Erasing from 0 to FlashSize wipes everything
        esp_err_t res = esp_partition_erase_range(Partition, 0, FlashSize);
        if (res == ESP_OK)
        {
            GapPointer = 0;
            for (int i = 0; i < FlashSize / SEGMENT_SIZE; i++)
                FreeSpace[i] = SEGMENT_SIZE;
            return true;
        }

        return false;
    }

    // Generic Raw Erase
    int FlashErase(uint32_t Offset, uint32_t Length)
    {
        // Length must be a multiple of Sector Size
        if (esp_partition_erase_range(Partition, Offset, Length) == ESP_OK)
            return 0;
        return -1;
    }

#else
#define CHIP_FLASH_SIZE (64 * 1024)
#define PAGE_SIZE 256
#define SECTOR_SIZE 4096
#define SEGMENT_SIZE (PAGE_SIZE * 4)

    // Logic Helpers
    const uint32_t FlashStart = 0xD000;           // Relative to 0x08000000
    const uint32_t FlashSize = (SECTOR_SIZE * 3); // 12KB
    uint16_t FreeSpace[FlashSize / SEGMENT_SIZE];

#define GET_PHYS_ADDR(off) (0x08000000 + FlashStart + (off))

    // Reserved block for the Linker
    const uint8_t flash_reservation[SECTOR_SIZE*3] __attribute__((section(".fixed_data"), used)) = {0xFF};

    void FlashInitDevice()
    {
        FLASH_Unlock();
        // FLASH_Unlock_Fast();

        FLASH_ClearFlag(FLASH_FLAG_BSY | FLASH_FLAG_EOP | FLASH_FLAG_WRPRTERR);

        FLASH->ACTLR = (FLASH->ACTLR & ~0x1F) | 0x12;
        FLASH_Access_Clock_Cfg(SystemCoreClock);

        // FLASH->CTLR |= (1 << 13);
    }

    int FlashRead(uint32_t Offset, void *Buffer, uint32_t Length)
    {
        if (Offset + Length > FlashSize)
            return -1;

        FLASH->CTLR &= ~(FLASH_CTLR_PG | FLASH_CTLR_PER | FLASH_CTLR_STRT);

        // Clear any leftover flags that might be blocking the bus
        FLASH->STATR = FLASH_STATR_EOP | FLASH_STATR_WRPRTERR;

        // 2. Memory barrier
        __asm__ volatile("fence r, rw" : : : "memory");

        uint32_t *src = (uint32_t *)GET_PHYS_ADDR(Offset);
        memcpy(Buffer, src, Length);
        return 0;
    }

    // 2. STANDARD WRITE: Word-by-Word (Most robust mode)
    int FlashWrite(uint32_t Offset, const void *Buffer, uint32_t Length)
    {
        Length = Align(Length);
        if (Offset + Length > FlashSize)
            return -1;

        uint32_t Address = GET_PHYS_ADDR(Offset);
        uint32_t *DataPtr = (uint32_t *)Buffer;

        for (uint32_t Index = 0; Index < Length / 4; Index++)
        {
            FLASH_ClearFlag(FLASH_FLAG_BSY | FLASH_FLAG_EOP | FLASH_FLAG_WRPRTERR);

            FLASH_Status status = FLASH_ProgramWord(Address + (Index * 4), DataPtr[Index]);

            if (status != FLASH_COMPLETE)
                return -1;
        }

        // Force CPU to see the new data
        __asm__ volatile("fence.i" : : : "memory");
        return 0;
    }

    // 3. FAST ERASE: 256-byte increments (Used for targeted erasing)
    int FlashErase(uint32_t Offset, uint32_t Length)
    {
        uint32_t Address = GET_PHYS_ADDR(Offset);

        // Ensure Fast Mode is unlocked for this specific operation
        FLASH_Unlock_Fast();

        for (uint32_t Page = 0; Page < Length; Page += PAGE_SIZE)
        {
            FLASH_ClearFlag(FLASH_FLAG_BSY | FLASH_FLAG_EOP | FLASH_FLAG_WRPRTERR);

            FLASH_ErasePage_Fast(Address + Page);
            for (uint32_t Word = 0; Word < PAGE_SIZE; Word += 4)
            {
                if (*(volatile uint32_t *)GET_PHYS_ADDR(Address + Page + Word) != 0xFFFFFFFF)
                    FLASH_ProgramWord(GET_PHYS_ADDR(Address + Page + Word), 0xFFFFFFFF); // Prevent incorrect reading
            }
        }

        FLASH_Lock_Fast();
        return 0;
    }

    int FlashFormat()
    {
        FLASH_Unlock();
        for (uint32_t Sector = 0; Sector < FlashSize; Sector += SECTOR_SIZE)
        {
            FLASH_ClearFlag(FLASH_FLAG_BSY | FLASH_FLAG_EOP | FLASH_FLAG_WRPRTERR);
            // uint32_t flag =
            FLASH_ErasePage(GET_PHYS_ADDR(Sector));
            // USB_Send(ByteArray(flag).CreateMessage());

            for (uint32_t Word = 0; Word < SECTOR_SIZE; Word += 4)
            {
                if (*(volatile uint32_t *)GET_PHYS_ADDR(Sector + Word) != 0xFFFFFFFF)
                    FLASH_ProgramWord(GET_PHYS_ADDR(Sector + Word), 0xFFFFFFFF); // Prevent incorrect reading
            }
        }

        GapPointer = 0;
        for (uint32_t i = 0; i < FlashSize / SEGMENT_SIZE; i++)
            FreeSpace[i] = SEGMENT_SIZE;

        return 0;
    }

#endif

    void FlashInit()
    {
        FlashInitDevice();
        uint32_t numSegments = FlashSize / SEGMENT_SIZE;
        GapPointer = 0; // Default to start

        for (uint32_t s = 0; s < numSegments; s++)
        {
            uint32_t cursor = s * SEGMENT_SIZE;
            uint32_t segmentEnd = cursor + SEGMENT_SIZE;

            // 1. Scan headers in the current segment
            while (cursor + sizeof(MemoryHeader) <= segmentEnd)
            {
                MemoryHeader head;
                FlashRead(cursor, &head, sizeof(MemoryHeader));

                if (head.Status == 0xFF)
                    break; // End of data in this segment

                cursor += sizeof(MemoryHeader) + Align(head.Length);
            }

            // 2. Store the calculated free space
            FreeSpace[s] = (uint16_t)(segmentEnd - cursor);

            // 3. Logic to find the Gap:
            // If this segment is 100% empty, and the previous one was NOT,
            // then this is the leading edge of our empty Gap.
            if (s > 0)
            {
                if (FreeSpace[s] == SEGMENT_SIZE && FreeSpace[s - 1] < SEGMENT_SIZE)
                {
                    GapPointer = (uint16_t)s;
                }
            }
        }

        // 4. Wrap-around check:
        // If Segment 0 is empty but the last segment has data,
        // the gap actually starts at index 0.
        if (FreeSpace[0] == SEGMENT_SIZE && FreeSpace[numSegments - 1] < SEGMENT_SIZE)
        {
            GapPointer = 0;
        }
    }

    uint32_t FindNextEntry(uint32_t Last)
    {
        // If Last is 0xFFFFFFFF, start at 0, otherwise start after the current entry
        uint32_t cursor = (Last == 0xFFFFFFFF) ? 0 : Last;

        // If we were given a real address, we need to skip the current entry's header + data
        if (Last != 0xFFFFFFFF)
        {
            MemoryHeader lastHead;
            if (FlashRead(Last, &lastHead, sizeof(MemoryHeader)) != 0)
                return 0xFFFFFFFF;
            cursor += sizeof(MemoryHeader) + Align(lastHead.Length);
        }

        while (cursor + sizeof(MemoryHeader) <= FlashSize)
        {
            // ESP_LOGI("MEM", "Searching valid next entry %d\n", cursor);
            //  Handle segment boundaries: if a header can't fit at the end of a segment,
            //  move to the start of the next segment.
            uint32_t remainingInSegment = SEGMENT_SIZE - (cursor % SEGMENT_SIZE);
            if (remainingInSegment < sizeof(MemoryHeader))
            {
                cursor += remainingInSegment;
                continue;
            }

            MemoryHeader head;
            if (FlashRead(cursor, &head, sizeof(MemoryHeader)) != 0)
                break;

            if (head.Status == 0xEE)
            { // Valid Entry Found
                // ESP_LOGI("MEM", "Found valid next entry %d", cursor);
                return cursor;
            }

            if (head.Status == 0xFF)
            {
                // Reached empty space in this segment. Skip to the next segment start.
                // ESP_LOGI("MEM", "Empty next entry %d", cursor);
                cursor += remainingInSegment;
                continue;
            }

            // It's Stale (0x88) or Erased (0x00), skip to next entry in this segment
            cursor += sizeof(MemoryHeader) + Align(head.Length);
        }

        return 0xFFFFFFFF; // Nothing found
    }

    uint32_t FindEntry(uint16_t ID)
    {
        uint32_t cursor = 0;

        while (cursor + sizeof(MemoryHeader) <= FlashSize)
        {
            // ESP_LOGI("MEM", "Searching valid entry %d", cursor);
            uint32_t remainingInSegment = SEGMENT_SIZE - (cursor % SEGMENT_SIZE);

            if (remainingInSegment < sizeof(MemoryHeader))
            {
                cursor += remainingInSegment;
                continue;
            }

            MemoryHeader head;
            if (FlashRead(cursor, &head, sizeof(MemoryHeader)) != 0)
                break;

            if (head.Status == 0xFF)
            {
                // End of data in this segment
                // ESP_LOGI("MEM", "Empty entry %d", cursor);
                cursor += remainingInSegment;
                continue;
            }

            if (head.Status == 0xEE && head.ID == ID)
            {
                // ESP_LOGI("MEM", "Found ID %d at %d", ID, cursor);
                return cursor; // Found it!
            }

            // Move to next entry
            cursor += sizeof(MemoryHeader) + Align(head.Length);
        }

        return 0xFFFFFFFF;
    }

    uint32_t FindSpace(uint16_t Length)
    {
        // Important: We must use the same alignment as FlashInit
        uint16_t totalNeeded = Align(Length);
        uint32_t numSegments = FlashSize / SEGMENT_SIZE;

        // A) Scan all for partials
        for (uint32_t s = 0; s < numSegments; s++)
        {
            if (FreeSpace[s] >= totalNeeded && FreeSpace[s] < SEGMENT_SIZE)
            {
                uint32_t offset = (s * SEGMENT_SIZE) + (SEGMENT_SIZE - FreeSpace[s]);
                FreeSpace[s] -= totalNeeded; // RAM Update
                // ESP_LOGI("MEM", "Found space in segment %d at %d", s, offset);
                return offset;
            }
        }

        // B) Use Gap
        if (FreeSpace[GapPointer] == SEGMENT_SIZE)
        {
            uint32_t offset = (GapPointer * SEGMENT_SIZE);
            FreeSpace[GapPointer] -= totalNeeded;
            // ESP_LOGI("MEM", "Found empty segment %d at %d", GapPointer, offset);
            GapPointer = (GapPointer + 1) % numSegments; // Move Gap
            return offset;
        }

        return 0xFFFFFFFF;
    }

    void CleanMemory()
    {
        uint32_t numSegments = FlashSize / SEGMENT_SIZE;
        uint16_t emptyCount = 0;

        // 1. Count current empty segments
        for (uint32_t i = 0; i < numSegments; i++)
        {
            if (FreeSpace[i] == SEGMENT_SIZE)
                emptyCount++;
        }

        // 2. The "Snowplow" Logic: Start at GapPointer and move forward
        for (uint32_t i = 0; i < numSegments; i++)
        {
            // Stop if we've recovered enough breathing room
            if (emptyCount >= 2)
                break;

            uint32_t targetIdx = (GapPointer + i) % numSegments;

            // Skip segments that are already empty
            if (FreeSpace[targetIdx] == SEGMENT_SIZE)
                continue;

            // --- Forced Defragmentation / Evacuation ---
            uint32_t cursor = targetIdx * SEGMENT_SIZE;
            uint32_t segmentEnd = cursor + SEGMENT_SIZE;

            while (cursor + sizeof(MemoryHeader) <= segmentEnd)
            {
                MemoryHeader head;
                FlashRead(cursor, &head, sizeof(MemoryHeader));

                if (head.Status == 0xFF)
                    break; // End of data in this segment

                if (head.Status == 0xEE) // Valid Entry Found
                {
                    // Read existing data
                    FlexArray data(head.Length);
                    FlashRead(cursor + sizeof(MemoryHeader), data.Array, head.Length);
                    data.Length = head.Length;

                    // Relocate data to a new segment
                    uint32_t newAddr = FindSpace(head.Length + sizeof(MemoryHeader));
                    if (newAddr != 0xFFFFFFFF)
                    {
                        head.Status = 0xEE;
                        FlashWrite(newAddr, &head, sizeof(MemoryHeader));
                        FlashWrite(newAddr + sizeof(MemoryHeader), data.Array, head.Length);
                    }
                }

                // Move to next entry in the current target segment
                cursor += sizeof(MemoryHeader) + Align(head.Length);
            }

            // 3. Finalize segment cleaning
            FlashErase(targetIdx * SEGMENT_SIZE, SEGMENT_SIZE);
            // ESP_LOGI("MEM", "Cleaned segment %d (%d)", targetIdx, targetIdx * SEGMENT_SIZE);
            FreeSpace[targetIdx] = SEGMENT_SIZE;
            emptyCount++;
        }
    }

    void LoadAll()
    {
        uint32_t cursor = 0xFFFFFFFF; // Start signal for FindNextEntry

        // ESP_LOGI("MEM", "Starting LoadAll");

        while (true)
        {
            // 1. Find the next valid (0xEE) entry
            cursor = FindNextEntry(cursor);

            // ESP_LOGI("MEM", "Loading %d", cursor);

            // 2. If no more valid entries are found, we are done
            if (cursor == 0xFFFFFFFF)
                break;

            // 3. Read the header to get metadata (ID, Type, Length)
            MemoryHeader head;
            if (FlashRead(cursor, &head, sizeof(MemoryHeader)) != 0)
                break;

            // 4. Integrity Check: Verify Checksum before instantiating
            // We read the data into a temporary buffer to check CRC
            FlexArray dataBuffer(head.Length);
            FlashRead(cursor + sizeof(MemoryHeader), dataBuffer.Array, head.Length);
            dataBuffer.Length = head.Length;

            // ESP_LOGI("MEM", "Loaded %d, length %d", cursor, head.Length + sizeof(MemoryHeader));
            //   6. Factory Instantiation
            //   Assuming Objects.Create takes the Type and the Payload to rebuild the class
            BaseClass *Object = nullptr;
            int32_t Existing = Objects.Search(Reference::Global(0, head.GroupID, head.DeviceID));
            if (Existing != -1)
                Object = Objects.Object[Existing].Object;
            else
                Object = CreateObject(Reference::Global(0, head.GroupID, head.DeviceID), (ObjectTypes)head.Type);

            if (Object == nullptr)
                continue;

            Object->Flags = (FlagClass)head.Flags;
            memcpy(&Object->RunPeriod, dataBuffer.Array, 2);
            uint16_t NameLength = dataBuffer.Array[2];
            if (NameLength > 0)
                Object->Name = Text(dataBuffer.Array + 3, NameLength);
            // ESP_LOGI("MEM", "Deserializing (%d.%d)", head.GroupID, head.DeviceID);
            Object->Values.Deserialize(dataBuffer, 3 + NameLength, true);
        }
    }

    bool Invalidate(const Reference &ID)
    {
        // ID mapping: Group is low byte, Device is high byte (Little Endian)
        uint16_t lookupID = ((uint16_t)ID.Device << 8) | ID.Group;
        bool foundAny = false;

        while (true)
        {
            uint32_t oldAddr = FindEntry(lookupID);
            if (oldAddr == 0xFFFFFFFF)
                break;

            // 1. Read the existing first word of the header
            uint32_t firstWord;
            if (FlashRead(oldAddr, &firstWord, 4) == 0)
            {
                // 2. Modify only the Status byte (first byte of MemoryHeader)
                // Mask out bits 7:0 and set them to 0x88 (Stale)
                firstWord = (firstWord & 0xFFFFFF00) | 0x88;

                // 3. Write the full 4-byte word back
                if (FlashWrite(oldAddr, &firstWord, 4) == 0)
                {
                    foundAny = true;
                }
                else
                {
                    break; // Flash Write Failure
                }
            }
            else
            {
                break; // Flash Read Failure
            }
        }
        return foundAny;
    }

    void InvalidateAll()
    {
        uint32_t Addr = FindNextEntry(0xFFFFFFFF);
        while (Addr != 0xFFFFFFFF)
        {
            // 1. Read existing word
            uint32_t firstWord;
            if (FlashRead(Addr, &firstWord, 4) == 0)
            {
                // 2. Modify Status byte to 0x88
                firstWord = (firstWord & 0xFFFFFF00) | 0x88;

                // 3. Write back the full word
                FlashWrite(Addr, &firstWord, 4);
            }

            Addr = FindNextEntry(Addr);
        }
    }

    bool Save(const Reference &ID)
    {
        // Saves specific object
        BaseClass *Object = Objects.At(ID);
        if (Object == nullptr)
        {
            ReportError(Status::InvalidID);
            return false;
        }

        Object->Flags -= Flags::Dirty;
        FlexArray Payload = Object->Compress(true); // Ignores net
        MemoryHeader Header = {
            .Status = 0xEE,
            .Checksum = 0x00,
            .Length = (uint16_t)(Payload.Length - 4) // ID, Type and flags are in header (-4)
        };
        FlexArray Data = FlexArray((char *)&Header, 4);
        Data.Append(Payload.Array, Payload.Length); // Net is ignored

        Invalidate(ID);

        // 4. Ensure we have room (Maintenance)
        // If we are low on empty segments, push the Gap forward
        CleanMemory();

        // 5. Find Space
        uint32_t writeAddr = FindSpace(Data.Length);
        if (writeAddr == 0xFFFFFFFF)
        {
            // If still no space after cleaning, we are truly full
            ReportError(Status::OutOfFlash);
            return false;
        }
        FlashWrite(writeAddr, Data.Array, Data.Length);
        // ESP_LOGI("MEM", "Saved at %d, length %d", writeAddr, Data.Length);
        // Note: FindSpace already updated FreeSpace[s] for us.
        return true;
    }

    int32_t GetFreeFlash()
    {
        int32_t total = 0;
        const size_t numSegments = sizeof(FreeSpace) / sizeof(FreeSpace[0]);

        for (size_t i = 0; i < numSegments; i++)
        {
            total += FreeSpace[i];
        }

        return total;
    }
}
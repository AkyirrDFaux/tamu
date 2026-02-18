namespace HW
{
    uint32_t FlashWriteHead = 0;
    uint32_t FlashReadHead = 0;

    void FlashInitDevice();
    void FlashInit();
    int FlashRead(uint32_t Offset, void *Buffer, uint32_t Length);
    int FlashWrite(uint32_t Offset, const void *Buffer, uint32_t Length);
    int FlashErase(uint32_t Offset, uint32_t Length);

    typedef struct
    {
        uint8_t Status;    // 0xEE (Valid), 0x88 (Stale), 0xFF (Empty), 0x00 (Erased),
        uint8_t Checksum;  // CRC8
        uint16_t reserved; // Padding
        uint32_t Length;
        uint32_t ID; // Part of data
        // uint8_t Data[];
    } __attribute__((packed)) MemoryHeader;

#if defined BOARD_Tamu_v1_0 || defined BOARD_Tamu_v2_0
#include "esp_partition.h"
#define PAGE_SIZE 4096
#define FLASH_PADDING 4
    uint32_t FlashStart = 0;
    uint32_t FlashSize = 0xED000;
    static const esp_partition_t *Partition = NULL;

    void FlashInitDevice()
    {
        Partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                             ESP_PARTITION_SUBTYPE_DATA_SPIFFS,
                                             "storage");
        FlashSize = Partition->size;
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
            return true;
        return false;
    }

    // Generic Raw Erase
    int FlashErase(uint32_t Offset, uint32_t Length)
    {
        // Length must be a multiple of Sector Size
        return esp_partition_erase_range(Partition, Offset, Length) == ESP_OK ? 0 : -1;
    }

#else
// #include "ch32v20x_flash.h"
#define CHIP_FLASH_SIZE (64 * 1024)
#define PAGE_SIZE 256
#define SECTOR_SIZE 4096
#define FLASH_PADDING 4

    // Logic Helpers
    uint32_t FlashSize = (SECTOR_SIZE * 2); // 8KB
    uint32_t FlashStart = 0xE000;           // Relative to 0x08000000

#define GET_PHYS_ADDR(off) (0x0800E000 + FlashStart + (off))

    // Reserved block for the Linker
    const uint8_t flash_reservation[8192] __attribute__((section(".fixed_data"), used)) = {0xFF};

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

        // ByteArray B = ByteArray();

        volatile uint32_t *src = (volatile uint32_t *)GET_PHYS_ADDR(Offset);
        uint32_t *dest = (uint32_t *)Buffer;

        for (uint32_t i = 0; i < Length / 4; i++)
        {
            dest[i] = src[i];
            // B = B << ByteArray(dest[i]);
        }
        // USB_Send(B.CreateMessage());
        return 0;
    }

    // 2. STANDARD WRITE: Word-by-Word (Most robust mode)
    int FlashWrite(uint32_t Offset, const void *Buffer, uint32_t Length)
    {
        if (Offset + Length > FlashSize)
            return -1;

        uint32_t Address = GET_PHYS_ADDR(Offset);
        uint32_t *DataPtr = (uint32_t *)Buffer;

        for (uint32_t Index = 0; Index < Length/4; Index++)
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

        return 0;
    }
    
#endif

    void FlashInit()
    {
        FlashInitDevice();
        FlashReadHead = 0;
        FlashWriteHead = 0;
        uint32_t Current = 0;
        MemoryHeader Header;
        
        while (Current < FlashSize)
        {
            FlashRead(Current, &Header, 8);
            if (Header.Status == 0xFF) // Found gap
            {
                FlashWriteHead = Current;
                break;
            }
            Current += Header.Length + 8;
        }

        while (Current < FlashSize)
        {
            FlashRead(Current, &Header, 4);
            if (Header.Status != 0xFF) // Found erased or something
                break;
            Current = Current + (PAGE_SIZE - (Current % PAGE_SIZE)); // Go to next start of sector
        }

        while (Current < FlashSize)
        {
            FlashRead(Current, &Header, 4);
            if (Header.Status != 0x00) // Found first packet after gap
            {
                FlashReadHead = Current;
                break;
            }
            Current = Current + 4; // Continue
        }
        /*ESP_LOGI("FLASH", "Init Complete:");
        ESP_LOGI("FLASH", "  > Start:      0x%08X", FlashStart);
        ESP_LOGI("FLASH", "  > Read Head:  0x%08X", FlashReadHead);
        ESP_LOGI("FLASH", "  > Write Head: 0x%08X", FlashWriteHead);*/
    }

    int32_t FlashFind(IDClass ID)
    {
        // ESP_LOGI("FLASH", "Searching");
        uint32_t Current = FlashReadHead;
        MemoryHeader Header;
        // HW::USB_Send(ByteArray(Current).CreateMessage());
        while ((FlashReadHead > FlashWriteHead && (Current >= FlashReadHead || Current < FlashWriteHead)) ||
               (FlashReadHead <= FlashWriteHead && Current <= FlashWriteHead))
        {
            // HW::USB_Send(ByteArray(Current).CreateMessage());
            //  ESP_LOGI("FLASH", "Searching at 0x%08X", Current);
            if (Current >= FlashSize && FlashReadHead > FlashWriteHead && Current > FlashReadHead) // Go to start (circular)
                Current = 0;
            else if (Current >= FlashSize) // It's the end (linear)
                return -1;

            FlashRead(Current, &Header, 12);
            // ESP_LOGI("FLASH", "Searching Header Status 0x%02X, ID 0x%08X", Header.Status, Header.ID);
            // HW::USB_Send(ByteArray(Header.Status).CreateMessage());
            if (Header.ID == ID.ID && Header.Status == 0xEE)
                return Current;
            else if (Header.Status == 0xFF && FlashReadHead > FlashWriteHead && Current > FlashReadHead) // There can be a gap at the end (circular)
                Current = 0;
            else if (Header.Status == 0xFF) // It's the end (gap or linear)
                return -1;

            Current += Header.Length + 8;
        }
        return -1;
    }

    ByteArray FlashLoad(IDClass ID)
    {
        // ESP_LOGI("FLASH", "Loading");
        // HW::USB_Send(ByteArray(ID).CreateMessage());
        int32_t Current = FlashFind(ID);
        if (Current == -1)
        {
            ReportError(Status::NotInFlash);
            return ByteArray();
        }
        // ESP_LOGI("FLASH", "Found at 0x%08X", Current);
        // HW::USB_Send(ByteArray(Current).CreateMessage());

        uint32_t Length;
        FlashRead(Current + 4, &Length, 4);

        ByteArray Data = ByteArray();
        Data.Length = Length + 4; // Need to add back ID type, Would be + 1, but needs aligned to 4
        Data.Array = new char[Length + 4];
        FlashRead(Current + 8, Data.Array + 4, Length);
        Data.Array[3] = (char)Types::ID;
        return Data.SubArray(3, Length + 1);
    }

    bool FlashMatch(uint32_t Offset, const ByteArray &Data)
    {
        MemoryHeader Header;
        FlashRead(Offset, &Header, 8);
        if (Header.Length != Data.Length)
            return false;

        char *Buf = new char[Data.Length];
        FlashRead(Offset + 8, Buf, Data.Length);
        int Res = memcmp(Buf, Data.Array, Data.Length);
        delete[] Buf;
        return Res == 0;
    };

    uint32_t GetWritableSpace(uint32_t Write, uint32_t Read)
    {
        uint32_t AbsoluteGap;

        if (Write < Read)
            AbsoluteGap = Read - Read % PAGE_SIZE - Write;
        else
            AbsoluteGap = (FlashSize - Write) + (Read - Read % PAGE_SIZE);

        if (AbsoluteGap <= PAGE_SIZE * 3)
            return 0;
        return AbsoluteGap - PAGE_SIZE * 3;
    }

    bool FlashPrepare(int32_t &Offset, uint32_t Length)
    {
        if (FlashWriteHead + Length > FlashSize)
        {
            // TODO: Near the end, defragment something first if possible
            FlashWriteHead = 0;
        }

        uint32_t Current = FlashReadHead;
        MemoryHeader Header;
        // If not, start defragmenting
        while (GetWritableSpace(FlashWriteHead, FlashReadHead) <= 0) // Real writable space
        {
            FlashRead(Current, &Header, 8);
            if (Header.Status == 0xEE) // Find non-stale
            {
                if (GetWritableSpace(FlashWriteHead, Current) > 0) // Done
                {
                    for (uint32_t Sector = FlashReadHead / PAGE_SIZE; Sector < Current / PAGE_SIZE; Sector++) // Erase stale{}
                        FlashErase(Sector * PAGE_SIZE, PAGE_SIZE);

                    uint32_t Empty = 0;
                    for (uint32_t Index = Current - Current % PAGE_SIZE; Index < Current; Index += 4) // Mark Empty
                        FlashWrite(Index, &Empty, 4);

                    FlashReadHead = Current;
                    break;
                }
                else
                {
                    // Copy
                    uint32_t Source = Current;
                    uint32_t Dest = FlashWriteHead;
                    uint32_t Remaining = Header.Length + 8;
                    uint8_t Buf[64];

                    while (Remaining > 0)
                    {
                        uint32_t Chunk = (Remaining > 64) ? 64 : Remaining;
                        FlashRead(Source, Buf, Chunk);
                        FlashWrite(Dest, Buf, Chunk);
                        Source += Chunk;
                        Dest += Chunk;
                        Remaining -= Chunk;
                    }
                    FlashWriteHead = Dest;
                    // Mark as stale
                    Header.Status = 0x88;
                    FlashWrite(Current, &Header, 4);
                }
            }
            for (uint32_t Sector = FlashReadHead / PAGE_SIZE; Sector < Current / PAGE_SIZE; Sector++) // Erase stale{}
                FlashErase(Sector * PAGE_SIZE, PAGE_SIZE);
            FlashReadHead = Current;

            Current += Header.Length + 8;
        }
        Offset = FlashWriteHead;
        return true;
    };

    bool FlashSave(const ByteArray &Data)
    {
        IDClass ID;
        memcpy((void *)&ID, Data.Array, sizeof(IDClass));
        int32_t Current = FlashFind(ID);

        if (Current != -1)
        {
            if (FlashMatch(Current, Data))
                return true;
            else
            {
                MemoryHeader Header;
                Header.Status = 0x88;
                FlashWrite(Current, &Header, 4); // Mark as stale
            }
        }

        if (FlashPrepare(Current, Data.Length + 8) == false)
        {
            ReportError(Status::OutOfFlash);
            return false; // Not enough memory
        }
        // ESP_LOGI("FLASH", "Writing to 0x%08X, length %d", Current, Data.Length);
        MemoryHeader Header;
        Header.Status = 0xEE;
        Header.Length = Data.Length;

        // HW::USB_Send(ByteArray(FlashWriteHead).CreateMessage());
        FlashWrite(Current, &Header, 8);                  // Length + Status
        FlashWrite(Current + 8, Data.Array, Data.Length); // Data
        FlashWriteHead = Current + Data.Length + 8;
        ReportError(Status::FlashWritten);

        return true;
    }
}
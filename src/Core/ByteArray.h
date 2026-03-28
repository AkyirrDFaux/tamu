// Align to multiple of 4
#define Align(x) (((x) + 3) & ~3)

struct Header
{
    Types Type = Types::Undefined;
    uint8_t Depth = 0;
    uint16_t Length = 0;
    uint16_t Pointer = 0;
    uint16_t Skip = 0;
};

struct FindResult
{
    int16_t Header;
    Types Type;
    char *Value;
};

template <typename T>
struct Getter
{
    bool Success;
    T Value;

    inline operator T() const { return Value; }
};

class ByteArray
{
public:
    Header *HeaderArray = nullptr;
    uint16_t HeaderAllocated = 0;
    char *Array = nullptr;
    uint16_t Length = 0;    // Current tight length
    uint16_t Allocated = 0; // Capacity, aligned to 4

    ByteArray() {};
    ByteArray(const ByteArray &Copied);
    ~ByteArray();
    ByteArray(const char *data, uint16_t size);

    template <class C>
    ByteArray(const C &Data);

    void operator=(const ByteArray &Other);
    ByteArray &operator<<(const ByteArray &Data);
    ByteArray &operator+=(const ByteArray &Data);

    // --- Core Navigation (Uses Reference) ---
    FindResult Find(const Reference &Location, bool StopAtReferences = true) const;
    void UpdateSkip();
    void ResizeData(uint16_t HIdx, uint16_t NewSize);
    void Delete(const Reference &Location);
    void InsertAtIndex(uint16_t Idx, Types Type, uint8_t Depth, uint16_t Size);
    uint16_t Insert(const Reference &Location, int16_t NewDataSize);
    Types Type(const Reference &Location) const;

    // --- Template Accessors (Reference) ---
    __attribute__((noinline)) const void *Get(const Reference &Location, Types Type) const;

    template <class C>
    Getter<C> Get(const Reference &Location) const;

    __attribute__((noinline)) void Set(const void *Data, size_t Size, Types Type, const Reference &Location);

    template <class C>
    void Set(const C &Data, const Reference &Location);

    ByteArray Copy(const Reference &Location) const;

    // --- Protocol Logic ---
    ByteArray CreateMessage() const;
    ByteArray ExtractMessage();
};

ByteArray::ByteArray(const ByteArray &Copied)
{
    HeaderAllocated = Copied.HeaderAllocated;
    HeaderArray = new Header[HeaderAllocated];
    memcpy(HeaderArray, Copied.HeaderArray, HeaderAllocated * sizeof(Header));
    Length = Copied.Length;
    Allocated = Align(Length);
    Array = new char[Allocated];
    memcpy(Array, Copied.Array, Length);
};

ByteArray::~ByteArray()
{
    if (HeaderArray != nullptr)
        delete[] HeaderArray;
    if (Array != nullptr)
        delete[] Array;
};

ByteArray::ByteArray(const char *data, uint16_t size)
{
    Length = size;
    Allocated = Align(Length);

    // Safety: Handle empty initialization
    if (Allocated == 0)
    {
        HeaderAllocated = 0;
        HeaderArray = nullptr;
        Array = nullptr;
        return;
    }

    HeaderAllocated = 1;
    HeaderArray = new Header[1];
    HeaderArray[0].Type = Types::Message;
    HeaderArray[0].Depth = 0;
    HeaderArray[0].Length = size;
    HeaderArray[0].Pointer = 0;
    HeaderArray[0].Skip = 0;

    Array = new char[Allocated];
    if (size > 0 && data != nullptr)
    {
        memcpy(Array, data, size);
    }
}

template <class C>
ByteArray::ByteArray(const C &Data)
{
    Set(Data, {0});
};

void ByteArray::operator=(const ByteArray &Other)
{
    if (this == &Other)
        return;

    if (HeaderArray != nullptr)
        delete[] HeaderArray;
    HeaderAllocated = Other.HeaderAllocated;
    HeaderArray = new Header[HeaderAllocated];
    memcpy(HeaderArray, Other.HeaderArray, HeaderAllocated * sizeof(Header));

    if (Allocated < Other.Length)
    {
        if (Array != nullptr)
            delete[] Array;
        Allocated = Align(Other.Length);
        Array = new char[Allocated];
    }
    if (Other.Length > 0)
        memcpy(Array, Other.Array, Other.Length);
    Length = Other.Length;
};

ByteArray &ByteArray::operator+=(const ByteArray &Data)
{
    if (Data.Length == 0 || Data.Array == nullptr)
        return *this;

    // If we are empty, just become the other data
    if (Length == 0)
    {
        *this = Data;
        return *this;
    }

    uint16_t RequiredLength = Length + Data.Length;

    // 1. Expand Data Array capacity if needed
    if (RequiredLength > Allocated)
    {
        uint16_t NewAlloc = Align(RequiredLength + 16);
        char *NewData = new char[NewAlloc];

        // Use NewData consistently
        if (Array)
            memcpy(NewData, Array, Length);

        delete[] Array;
        Array = NewData;
        Allocated = NewAlloc;
    }

    // 2. Raw append: no alignment, no header logic
    memcpy(Array + Length, Data.Array, Data.Length);
    Length = RequiredLength;

    // 3. Update the very first header's length to encompass the new data
    if (HeaderAllocated > 0)
    {
        HeaderArray[0].Length = Length;
    }

    return *this;
}

ByteArray &ByteArray::operator<<(const ByteArray &Data)
{
    if (Data.HeaderAllocated == 0 || Data.Array == nullptr)
        return *this;

    if (HeaderAllocated == 0 || HeaderArray == nullptr)
    {
        *this = Data;
        return *this;
    }

    // ALWAYS APPEND as a new set of headers
    uint16_t DataOffset = Align(Length);
    uint16_t RequiredLength = DataOffset + Data.Length;

    // 1. Expand Header Array
    uint16_t NewHeaderCount = HeaderAllocated + Data.HeaderAllocated;
    Header *NewHeaders = new Header[NewHeaderCount];
    memcpy(NewHeaders, HeaderArray, HeaderAllocated * sizeof(Header));

    // 2. Expand Data Array capacity if needed
    if (RequiredLength > Allocated)
    {
        uint16_t NewAlloc = Align(RequiredLength + 16);
        char *NewData = new char[NewAlloc];
        if (Array)
            memcpy(NewData, Array, Length);
        delete[] Array;
        Array = NewData;
        Allocated = NewAlloc;
    }

    // 3. Map new headers with the aligned offset
    for (uint16_t i = 0; i < Data.HeaderAllocated; i++)
    {
        Header h = Data.HeaderArray[i];
        h.Pointer += DataOffset;
        NewHeaders[HeaderAllocated + i] = h;
    }

    // 4. Copy actual bytes
    memcpy(Array + DataOffset, Data.Array, Data.Length);

    delete[] HeaderArray;
    HeaderArray = NewHeaders;
    HeaderAllocated = NewHeaderCount;
    Length = RequiredLength;

    UpdateSkip();
    return *this;
}

void ByteArray::UpdateSkip()
{
    if (HeaderAllocated == 0 || !HeaderArray)
        return;

    for (int i = 0; i < HeaderAllocated; i++)
        HeaderArray[i].Skip = 0;

    for (int i = HeaderAllocated - 1; i > 0; i--)
    {
        if (HeaderArray[i].Depth == 0)
            continue;

        uint8_t targetParentDepth = HeaderArray[i].Depth - 1;
        for (int p = i - 1; p >= 0; p--)
        {
            if (HeaderArray[p].Depth == targetParentDepth)
            {
                HeaderArray[p].Skip += (HeaderArray[i].Skip + 1);
                break;
            }
            if (HeaderArray[p].Depth < targetParentDepth)
                break;
        }
    }
}

void ByteArray::Delete(const Reference &Location)
{
    // 1. Find the starting header of the branch/node to delete
    // Uses the new bit-tagged pathing logic
    FindResult Found = Find(Location);
    if (Found.Header < 0)
        return;

    uint16_t HIdx = (uint16_t)Found.Header;

    // 2. Determine the scope of the deletion
    // NodesToRemove includes the parent and all its children (via the Skip count)
    uint16_t NodesToRemove = HeaderArray[HIdx].Skip + 1;

    // 3. Remove all associated data in the blob
    // We process this backwards so that ResizeData (which shifts memory and
    // updates pointers) doesn't invalidate the addresses of trailing nodes
    // before they are also deleted.
    for (int i = NodesToRemove - 1; i >= 0; i--)
    {
        ResizeData(HIdx + i, 0);
    }

    // 4. Shrink the Header Array
    uint16_t NewCount = HeaderAllocated - NodesToRemove;
    if (NewCount > 0)
    {
        Header *NewHeaders = new Header[NewCount];

        // Copy existing headers that appear BEFORE the deleted branch
        if (HIdx > 0)
        {
            memcpy(NewHeaders, HeaderArray, HIdx * sizeof(Header));
        }

        // Copy existing headers that appear AFTER the deleted branch
        if (HIdx + NodesToRemove < HeaderAllocated)
        {
            uint16_t RemainingCount = HeaderAllocated - (HIdx + NodesToRemove);
            memcpy(NewHeaders + HIdx, HeaderArray + HIdx + NodesToRemove, RemainingCount * sizeof(Header));
        }

        delete[] HeaderArray;
        HeaderArray = NewHeaders;
        HeaderAllocated = NewCount;
    }
    else
    {
        // If the entire array was the target, clear the headers completely
        delete[] HeaderArray;
        HeaderArray = nullptr;
        HeaderAllocated = 0;
    }

    // 5. Recalculate parent Skip values to reflect the missing branch
    // This is critical to keep future Find() and Insert() calls accurate
    UpdateSkip();
}

void ByteArray::ResizeData(uint16_t HIdx, uint16_t NewSize)
{
    // 1. Calculate the Aligned Difference
    // Internal buffer ALWAYS keeps data on 4-byte boundaries
    uint16_t AlignedOld = Align(HeaderArray[HIdx].Length);
    uint16_t AlignedNew = Align(NewSize);
    int16_t Diff = (int16_t)AlignedNew - (int16_t)AlignedOld;

    if (Diff != 0)
    {
        uint16_t TargetPointer = HeaderArray[HIdx].Pointer;
        uint16_t OldDataEnd = TargetPointer + AlignedOld;
        uint16_t NewTotalLength = Length + Diff;

        // 2. Manage Buffer Capacity
        if (NewTotalLength > Allocated)
        {
            uint16_t NewAlloc = Align(NewTotalLength + 16);
            char *NewArray = new char[NewAlloc];
            if (Array)
            {
                memcpy(NewArray, Array, TargetPointer);
                if (Length > OldDataEnd)
                    memcpy(NewArray + TargetPointer + AlignedNew, Array + OldDataEnd, Length - OldDataEnd);
                delete[] Array;
            }
            Array = NewArray;
            Allocated = NewAlloc;
        }
        else
        {
            // Move tail data forward or backward
            if (Length > OldDataEnd)
                memmove(Array + TargetPointer + AlignedNew, Array + OldDataEnd, Length - OldDataEnd);
        }

        // 3. Update Pointers
        // We shift any pointer that starts AFTER or AT the same spot (except ourselves)
        for (uint16_t i = 0; i < HeaderAllocated; i++)
        {
            if (i != HIdx && HeaderArray[i].Pointer >= TargetPointer)
            {
                HeaderArray[i].Pointer += Diff;
            }
        }
        Length = NewTotalLength;
    }

    // Header always stores the TIGHT (logical) length
    HeaderArray[HIdx].Length = NewSize;
}

void ByteArray::InsertAtIndex(uint16_t Idx, Types Type, uint8_t Depth, uint16_t Size)
{
    // Find where the data should start.
    // If middle, take existing pointer. If end, take Length.
    uint16_t DataPointer = (Idx < HeaderAllocated) ? HeaderArray[Idx].Pointer : Length;

    Header *NewHeaders = new Header[HeaderAllocated + 1];
    if (HeaderArray)
    {
        if (Idx > 0)
            memcpy(NewHeaders, HeaderArray, Idx * sizeof(Header));
        if (Idx < HeaderAllocated)
            memcpy(NewHeaders + Idx + 1, HeaderArray + Idx, (HeaderAllocated - Idx) * sizeof(Header));
        delete[] HeaderArray;
    }
    HeaderArray = NewHeaders;
    HeaderAllocated++;

    // Create entry with 0 length initially so ResizeData can expand it
    HeaderArray[Idx] = {Type, Depth, 0, DataPointer, 0};

    UpdateSkip();

    // Resize handles the actual memory shift and alignment
    if (Size > 0)
        ResizeData(Idx, Size);
}

uint16_t ByteArray::Insert(const Reference &Location, int16_t NewDataSize)
{
    uint16_t CurrentIdx = 0;
    uint8_t totalDepth = Location.PathLen();

    for (uint8_t Layer = 0; Layer < totalDepth; Layer++)
    {
        uint8_t TargetSibling = Location.Path[Layer];
        uint16_t SiblingCount = 0;

        // Move through siblings
        while (SiblingCount < TargetSibling)
        {
            if (CurrentIdx >= HeaderAllocated || HeaderArray[CurrentIdx].Depth < Layer)
            {
                InsertAtIndex(CurrentIdx, Types::Undefined, Layer, 0);
            }
            CurrentIdx += HeaderArray[CurrentIdx].Skip + 1;
            SiblingCount++;
        }

        // Prepare to descend
        if (Layer < totalDepth - 1)
        {
            if (CurrentIdx >= HeaderAllocated || HeaderArray[CurrentIdx].Depth < Layer)
            {
                InsertAtIndex(CurrentIdx, Types::Undefined, Layer, 0);
            }
            CurrentIdx++; // Move to first child slot
        }
    }

    // Final node placement
    uint8_t TargetDepth = totalDepth - 1;
    if (CurrentIdx >= HeaderAllocated || HeaderArray[CurrentIdx].Depth != TargetDepth)
    {
        InsertAtIndex(CurrentIdx, Types::Undefined, TargetDepth, NewDataSize);
    }

    return CurrentIdx;
}

Types ByteArray::Type(const Reference &Location) const
{
    return Find(Location, false).Type;
}

ByteArray ByteArray::Copy(const Reference &Location) const
{
    // 1. Locate the item in the current database
    FindResult Result = Find(Location);

    // 2. If not found, return an empty ByteArray
    if (Result.Header == -1 || Result.Value == nullptr)
    {
        return ByteArray();
    }

    const Header &TargetHeader = HeaderArray[Result.Header];
    ByteArray NewArray;

    NewArray.HeaderArray = new Header[1];
    NewArray.HeaderAllocated = 1;

    // Reset navigation fields for the new root
    NewArray.HeaderArray[0].Type = TargetHeader.Type;
    NewArray.HeaderArray[0].Depth = 0;
    NewArray.HeaderArray[0].Length = TargetHeader.Length;
    NewArray.HeaderArray[0].Pointer = 0;
    NewArray.HeaderArray[0].Skip = 0;

    NewArray.Allocated = Align(TargetHeader.Length);
    if (NewArray.Allocated > 0)
    {
        NewArray.Array = new char[NewArray.Allocated];
        memcpy(NewArray.Array, Result.Value, TargetHeader.Length);
        NewArray.Length = TargetHeader.Length;
    }

    return NewArray;
}

ByteArray ByteArray::CreateMessage() const
{
    // Calculates PACKED size for the wire (no padding)
    uint16_t packedDataSize = 0;
    for (uint16_t i = 0; i < HeaderAllocated; i++)
        packedDataSize += HeaderArray[i].Length;

    uint16_t headerAreaSize = HeaderAllocated * 4;
    uint16_t totalSize = 4 + headerAreaSize + packedDataSize;

    ByteArray msg;
    msg.Array = new char[totalSize];
    msg.Length = totalSize;
    msg.Allocated = totalSize;

    // 1. Prefix
    memcpy(msg.Array, &totalSize, 2);
    memcpy(msg.Array + 2, &HeaderAllocated, 2);

    char *hPtr = msg.Array + 4;
    char *dPtr = msg.Array + 4 + headerAreaSize;

    // 2. Pack
    for (uint16_t i = 0; i < HeaderAllocated; i++)
    {
        hPtr[0] = (uint8_t)HeaderArray[i].Type;
        hPtr[1] = HeaderArray[i].Depth;
        memcpy(hPtr + 2, &HeaderArray[i].Length, 2);
        hPtr += 4;

        if (HeaderArray[i].Length > 0)
        {
            // Pull from our PADDED internal buffer
            memcpy(dPtr, Array + HeaderArray[i].Pointer, HeaderArray[i].Length);
            dPtr += HeaderArray[i].Length;
        }
    }
    return msg;
}

ByteArray ByteArray::ExtractMessage()
{
    if (Length < 4)
        return ByteArray();

    uint16_t totalWireSize = *(uint16_t *)Array;
    uint16_t count = *(uint16_t *)(Array + 2);

    if (Length < totalWireSize)
        return ByteArray();

    ByteArray res;
    res.HeaderAllocated = count;
    res.HeaderArray = new Header[count];

    char *hRead = Array + 4;
    char *dRead = Array + 4 + (count * 4);
    uint16_t internalPtr = 0;

    // Convert PACKED wire data to PADDED internal data
    for (uint16_t i = 0; i < count; i++)
    {
        res.HeaderArray[i].Type = (Types)hRead[0];
        res.HeaderArray[i].Depth = hRead[1];
        memcpy(&res.HeaderArray[i].Length, hRead + 2, 2);
        hRead += 4;

        res.HeaderArray[i].Pointer = internalPtr;
        // The core fix: internal buffer is PADDED
        internalPtr += Align(res.HeaderArray[i].Length);
    }

    res.Length = internalPtr;
    res.Allocated = Align(res.Length);
    res.Array = new char[res.Allocated];

    for (uint16_t i = 0; i < count; i++)
    {
        if (res.HeaderArray[i].Length > 0)
        {
            memcpy(res.Array + res.HeaderArray[i].Pointer, dRead, res.HeaderArray[i].Length);
            dRead += res.HeaderArray[i].Length;
        }
    }
    res.UpdateSkip();

    // Consume bytes from input stream
    uint16_t remaining = Length - totalWireSize;
    if (remaining > 0)
    {
        memmove(Array, Array + totalWireSize, remaining);
        Length = remaining;
    }
    else
        Length = 0;

    return res;
}
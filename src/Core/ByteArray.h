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

    // --- Core Navigation (Uses Path) ---
    FindResult Find(const Path &Location) const;
    void UpdateSkip();
    void ResizeData(uint16_t HIdx, uint16_t NewSize);
    void Delete(const Path &Location);
    void InsertAtIndex(uint16_t Idx, Types Type, uint8_t Depth, uint16_t Size);
    uint16_t Insert(const Path &Location, int16_t NewDataSize);
    Types Type(const Path &Location) const;

    // --- Template Accessors (Path) ---
    __attribute__((noinline)) const void *Get(const Path &Location, Types Type) const;
    template <class C>
    Getter<C> Get(const Path &Location) const;

    __attribute__((noinline)) void Set(const void *Data, size_t Size, Types Type, const Path &Location);
    template <class C>
    void Set(const C &Data, const Path &Location);

    ByteArray Copy(const Path &Location) const;

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
        if (Array) memcpy(NewData, Array, Length);
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

void ByteArray::Delete(const Path &Location)
{
    // Find the starting header of the branch/node to delete
    FindResult Found = Find(Location);
    if (Found.Header < 0)
        return;

    uint16_t HIdx = (uint16_t)Found.Header;
    // NodesToRemove includes the parent and all its children (Skip count)
    uint16_t NodesToRemove = HeaderArray[HIdx].Skip + 1;

    // 1. Remove all associated data in the blob
    // We do this backwards so that ResizeData (which shifts memory)
    // doesn't invalidate the pointers we are about to use for the next node.
    for (int i = NodesToRemove - 1; i >= 0; i--)
    {
        ResizeData(HIdx + i, 0);
    }

    // 2. Shrink the Header Array
    uint16_t NewCount = HeaderAllocated - NodesToRemove;
    if (NewCount > 0)
    {
        Header *NewHeaders = new Header[NewCount];

        // Copy segments before the deleted range
        if (HIdx > 0)
        {
            memcpy(NewHeaders, HeaderArray, HIdx * sizeof(Header));
        }

        // Copy segments after the deleted range
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
        // If everything was deleted
        delete[] HeaderArray;
        HeaderArray = nullptr;
        HeaderAllocated = 0;
    }

    // 3. Recalculate parent Skip values to reflect the missing branch
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

uint16_t ByteArray::Insert(const Path &Location, int16_t NewDataSize)
{
    uint16_t CurrentIdx = 0;

    // Traverse each layer of the path (0-indexed)
    for (uint8_t Layer = 0; Layer < Location.Length; Layer++)
    {
        uint8_t TargetSibling = Location.Indexing[Layer];
        uint16_t SiblingCount = 0;

        // 1. Move through siblings at this current depth
        while (SiblingCount < TargetSibling)
        {
            // If we run out of headers or hit a shallower depth,
            // we must insert missing siblings to reach our TargetSibling
            if (CurrentIdx >= HeaderAllocated || HeaderArray[CurrentIdx].Depth < Layer)
            {
                InsertAtIndex(CurrentIdx, Types::Undefined, Layer, 0);
            }

            // Jump over the sibling and its entire subtree
            CurrentIdx += HeaderArray[CurrentIdx].Skip + 1;
            SiblingCount++;
        }

        // 2. Prepare to descend
        // If this is NOT the last layer, we need to ensure the parent node exists
        if (Layer < Location.Length - 1)
        {
            if (CurrentIdx >= HeaderAllocated || HeaderArray[CurrentIdx].Depth < Layer)
            {
                InsertAtIndex(CurrentIdx, Types::Undefined, Layer, 0);
            }

            // Move from the parent header to its first child slot
            CurrentIdx++;
        }
    }

    // 3. Final placement
    // Check if the exact node already exists at the target depth.
    // Target depth is simply Location.Length - 1.
    uint8_t TargetDepth = Location.Length - 1;

    if (CurrentIdx >= HeaderAllocated || HeaderArray[CurrentIdx].Depth != TargetDepth)
    {
        InsertAtIndex(CurrentIdx, Types::Undefined, TargetDepth, NewDataSize);
    }

    return CurrentIdx;
}

FindResult ByteArray::Find(const Path &Location) const
{
    // Safety check for empty buffer or invalid path
    if (Array == nullptr || HeaderArray == nullptr || Location.Length == 0)
        return {-1, Types::Undefined, nullptr};

    int16_t Pointer = 0;
    uint16_t End = HeaderAllocated;

    // Loop through each layer of the Local Path (0 to Length-1)
    for (uint8_t Layer = 0; Layer < Location.Length; Layer++)
    {
        bool layerFound = false;
        uint8_t targetIdx = Location.Indexing[Layer];

        // Search siblings at the current depth
        for (uint16_t Search = 0; Search <= targetIdx; Search++)
        {
            if (Pointer >= End)
                return {-1, Types::Undefined, nullptr};

            Header &header = HeaderArray[Pointer];

            if (Search == targetIdx)
            {
                // 1. Exact path match found at this layer
                if (Layer == Location.Length - 1)
                {
                    return {Pointer, header.Type, Array + header.Pointer};
                }

                // 2. Is this a leaf that we are trying to treat as a packed object?
                // (e.g., accessing a byte inside a Vec3)
                if (IsPacked(header.Type))
                {
                    uint16_t offset = 0;
                    Types subType = Types::Byte;

                    // ... Your existing switch(header.Type) logic for offsets ...

                    return {-2, subType, Array + header.Pointer + offset};
                }

                // 3. Hybrid logic: Descend into this node to find children
                Pointer++;                   // Move to the first child immediately following this header
                End = Pointer + header.Skip; // Limit search to this parent's subtree
                layerFound = true;
                break;
            }

            // Not the sibling we are looking for: Skip it and its entire subtree
            Pointer += header.Skip + 1;
        }

        // If we exhausted siblings and didn't find the target index
        if (!layerFound)
            return {-1, Types::Undefined, nullptr};
    }

    return {-1, Types::Undefined, nullptr};
}

Types ByteArray::Type(const Path &Location) const
{
    return Find(Location).Type;
};

ByteArray ByteArray::Copy(const Path &Location) const
{
    // 1. Locate the item in the current database
    FindResult Result = Find(Location);

    // 2. If not found, return an empty ByteArray
    if (Result.Header == -1 || Result.Value == nullptr)
    {
        return ByteArray();
    }

    // 3. Access the specific Header for this value
    const Header &TargetHeader = HeaderArray[Result.Header];

    // 4. Create a new ByteArray and pre-allocate the required space
    // We only need 1 Header and enough space for the value's Length
    ByteArray NewArray;

    // Manual setup for the new "Singular" ByteArray
    NewArray.HeaderArray = new Header[1];
    NewArray.HeaderAllocated = 1;

    // Copy the header info but reset navigation fields
    NewArray.HeaderArray[0].Type = TargetHeader.Type;
    NewArray.HeaderArray[0].Depth = 0; // Reset depth as this is now the root
    NewArray.HeaderArray[0].Length = TargetHeader.Length;
    NewArray.HeaderArray[0].Pointer = 0; // Starts at the beginning of the data array
    NewArray.HeaderArray[0].Skip = 0;    // No other headers to skip

    // 5. Allocate and copy the actual raw data
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
    if (Length < 4) return ByteArray();

    uint16_t totalWireSize = *(uint16_t *)Array;
    uint16_t count = *(uint16_t *)(Array + 2);

    if (Length < totalWireSize) return ByteArray();

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
    else Length = 0;

    return res;
}
// Align to multiple of 4
#define Align(x) (((x) + 3) & ~3)

struct Header
{
    Types Type = Types::Undefined;
    uint8_t Depth = 0;
    int16_t Pointer = -1;
    uint16_t Length = 0;
    uint16_t Skip = 0;
};

struct FindResult
{
    int16_t Header;
    Types Type;
    char *Value;
};

class ByteArray
{
private:
    // Generic Get and Set to reduce templates
    __attribute__((noinline)) const void *Get(const Reference &Reference, Types Type) const;
    __attribute__((noinline)) void Set(const void *Data, size_t Size, Types Type, const Reference &Reference);

public:
    Header *HeaderArray = nullptr;
    uint16_t HeaderAllocated = 0;
    char *Array = nullptr;
    uint16_t Length = 0;    // End
    uint16_t Allocated = 0; // Ready, aligned to 4

    ByteArray() {};
    ByteArray(const ByteArray &Copied);
    ~ByteArray();
    template <class C>
    ByteArray(const C &Data);
    void operator=(const ByteArray &Other); // Copy

    ByteArray operator<<(const ByteArray &Data); // Flat concatenation, appends

    FindResult Find(const Reference &Reference) const; // Get byte-index of index-th type
    void UpdateSkip();
    void ResizeData(uint16_t HIdx, uint16_t NewSize);
    void Delete(const Reference &Ref);
    void InsertAtIndex(uint16_t Idx, Types Type, uint8_t Depth, uint16_t Size);
    uint16_t Insert(const Reference &Ref, int16_t NewDataSize); // Prepare index-th entry for data of specified size
    Types Type(const Reference &Reference) const;
    template <class C>
    const C *Get(const Reference &Reference) const; // Assumes it's valid, and type is checked
    template <class C>
    void Set(const C &Data, const Reference &Reference);

    ByteArray CreateMessage() const; // Removes alignment gaps, adds length
    ByteArray ExtractMessage(); // Removes length, adds gaps
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

template <class C>
ByteArray::ByteArray(const C &Data)
{
    uint8_t Start[4] = {0, 0, 0, 0};
    Set(&Data, sizeof(C), GetType<C>(), Reference({4, Start}));
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

ByteArray ByteArray::operator<<(const ByteArray &Data)
{
    if (Data.HeaderAllocated == 0) return *this;

    // 1. Expand Header Array
    uint16_t NewHeaderCount = HeaderAllocated + Data.HeaderAllocated;
    Header *NewHeaders = new Header[NewHeaderCount];

    if (HeaderArray) {
        memcpy(NewHeaders, HeaderArray, HeaderAllocated * sizeof(Header));
        delete[] HeaderArray;
    }

    // 2. Offset and Copy New Headers
    // New data starts at the current aligned end of our blob
    uint16_t DataOffset = Align(Length); 

    for (uint16_t i = 0; i < Data.HeaderAllocated; i++) {
        Header h = Data.HeaderArray[i];
        h.Pointer += DataOffset; // Shift pointer to the new "tail"
        NewHeaders[HeaderAllocated + i] = h;
    }

    HeaderArray = NewHeaders;
    uint16_t OldHeaderCount = HeaderAllocated; // Store to update skips later
    HeaderAllocated = NewHeaderCount;

    // 3. Expand Data Array
    uint16_t RequiredLength = DataOffset + Data.Length;
    if (RequiredLength > Allocated) {
        uint16_t NewAlloc = Align(RequiredLength + 16);
        char *NewData = new char[NewAlloc];
        if (Array) {
            memcpy(NewData, Array, Length);
            delete[] Array;
        }
        Array = NewData;
        Allocated = NewAlloc;
    }

    // Copy the actual bytes into the offset position
    if (Data.Array) {
        memcpy(Array + DataOffset, Data.Array, Data.Length);
    }
    Length = RequiredLength;

    // 4. Update the structural links
    UpdateSkip();

    return *this; // Return reference to allow chaining: a << b << c;
};

void ByteArray::UpdateSkip()
{
    if (HeaderAllocated == 0)
        return;

    for (int i = 0; i < HeaderAllocated; i++)
        HeaderArray[i].Skip = 0;

    for (int i = HeaderAllocated - 1; i > 0; i--)
    {
        uint8_t targetParentDepth = HeaderArray[i].Depth - 1;
        for (int p = i - 1; p >= 0; p--)
        {
            // Must be the immediate parent (first node found with Depth - 1)
            if (HeaderArray[p].Depth == targetParentDepth)
            {
                HeaderArray[p].Skip += (HeaderArray[i].Skip + 1);
                break;
            }
            // If we hit a sibling or higher depth before finding a parent,
            // the tree is malformed or we are at root.
            if (HeaderArray[p].Depth < targetParentDepth)
                break;
        }
    }
}

void ByteArray::Delete(const Reference &Ref)
{
    FindResult Found = Find(Ref);
    if (Found.Header < 0)
        return;

    uint16_t HIdx = (uint16_t)Found.Header;
    uint16_t NodesToRemove = HeaderArray[HIdx].Skip + 1;

    // 1. Remove all associated data in the blob
    // We do this backwards so the pointers of the nodes we're about to delete don't matter
    for (int i = NodesToRemove - 1; i >= 0; i--)
    {
        ResizeData(HIdx + i, 0);
    }

    // 2. Shrink the Header Array
    uint16_t NewCount = HeaderAllocated - NodesToRemove;
    if (NewCount > 0)
    {
        Header *NewHeaders = new Header[NewCount];

        // Copy parts before the deleted range
        if (HIdx > 0)
        {
            memcpy(NewHeaders, HeaderArray, HIdx * sizeof(Header));
        }
        // Copy parts after the deleted range
        if (HIdx + NodesToRemove < HeaderAllocated)
        {
            memcpy(NewHeaders + HIdx, HeaderArray + HIdx + NodesToRemove, (HeaderAllocated - (HIdx + NodesToRemove)) * sizeof(Header));
        }

        delete[] HeaderArray;
        HeaderArray = NewHeaders;
        HeaderAllocated = NewCount;
    }
    else
    {
        delete[] HeaderArray;
        HeaderArray = nullptr;
        HeaderAllocated = 0;
    }

    UpdateSkip();
}

void ByteArray::ResizeData(uint16_t HIdx, uint16_t NewSize)
{
    // We align the size to 4 bytes to ensure the NEXT node starts on an aligned address
    // Note: If the data is only 1 byte, we could skip alignment,
    // but for simplicity and safety, Align(NewSize) is the gold standard.
    uint16_t AlignedNewSize = Align(NewSize);
    uint16_t AlignedOldSize = Align(HeaderArray[HIdx].Length);

    int16_t Diff = (int16_t)AlignedNewSize - AlignedOldSize;
    if (Diff == 0)
    {
        HeaderArray[HIdx].Length = NewSize; // Size changed but alignment didn't
        return;
    }

    uint16_t TargetPointer = HeaderArray[HIdx].Pointer;
    uint16_t OldDataEnd = TargetPointer + AlignedOldSize;
    uint16_t NewTotalLength = Length + Diff;

    if (NewTotalLength > Allocated)
    {
        uint16_t NewAlloc = Align(NewTotalLength + 16);
        char *NewArray = new char[NewAlloc];
        if (Array)
        {
            memcpy(NewArray, Array, TargetPointer);
            if (Length > OldDataEnd)
            {
                // Shift the suffix by the Aligned Diff
                memcpy(NewArray + TargetPointer + AlignedNewSize,
                       Array + OldDataEnd,
                       Length - OldDataEnd);
            }
            delete[] Array;
        }
        Array = NewArray;
        Allocated = NewAlloc;
    }
    else if (NewTotalLength > 0)
    {
        // Shift existing memory within the current buffer
        memmove(Array + TargetPointer + AlignedNewSize,
                Array + OldDataEnd,
                Length - OldDataEnd);
    }

    // Update all subsequent headers to point to aligned starts
    for (uint16_t i = 0; i < HeaderAllocated; i++)
    {
        if (HeaderArray[i].Pointer > TargetPointer)
        {
            HeaderArray[i].Pointer += Diff;
        }
    }

    HeaderArray[HIdx].Length = NewSize;
    Length = NewTotalLength;
}

void ByteArray::InsertAtIndex(uint16_t Idx, Types Type, uint8_t Depth, uint16_t Size)
{
    // Determine where the data should actually go
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

    HeaderArray[Idx].Type = Type;
    HeaderArray[Idx].Depth = Depth;
    HeaderArray[Idx].Length = 0; // Crucial: Start at 0
    HeaderArray[Idx].Skip = 0;
    HeaderArray[Idx].Pointer = DataPointer;

    UpdateSkip();

    // Now ResizeData sees AlignedOldSize = Align(0) = 0.
    // It will shift everything from DataPointer forward by Align(Size).
    if (Size > 0)
        ResizeData(Idx, Size);
}

uint16_t ByteArray::Insert(const Reference &Ref, int16_t NewDataSize)
{
    uint16_t CurrentIdx = 0;

    for (int16_t Layer = 3; Layer < Ref.Length; Layer++)
    {
        uint8_t TargetSibling = Ref.Indexing[Layer];
        uint16_t SiblingCount = 0;

        while (SiblingCount < TargetSibling)
        {
            if (CurrentIdx >= HeaderAllocated || HeaderArray[CurrentIdx].Depth < (Layer - 3))
            {
                InsertAtIndex(CurrentIdx, Types::Group, Layer - 3, 0);
                // The node is now at CurrentIdx.
                // We increment sibling count and jump to the next potential sibling spot.
            }

            CurrentIdx += HeaderArray[CurrentIdx].Skip + 1;
            SiblingCount++;
        }

        // Deepen the search
        if (Layer < Ref.Length - 1)
        {
            // If the slot is empty or we hit a different branch, create the parent group
            if (CurrentIdx >= HeaderAllocated || HeaderArray[CurrentIdx].Depth < (Layer - 3))
            {
                InsertAtIndex(CurrentIdx, Types::Group, Layer - 3, 0);
            }

            // "Upgrade" existing data nodes to groups if necessary
            if (HeaderArray[CurrentIdx].Type != Types::Group)
            {
                HeaderArray[CurrentIdx].Type = Types::Group;
                ResizeData(CurrentIdx, 0);
            }
            CurrentIdx++; // Move to child level
        }
    }

    InsertAtIndex(CurrentIdx, Types::Undefined, Ref.Length - 4, NewDataSize);
    return CurrentIdx;
}

FindResult ByteArray::Find(const Reference &Reference) const
{
    // Current index
    if (Array == nullptr || HeaderArray == nullptr)
        return {-1, Types::Undefined, nullptr};

    uint16_t Pointer = 0;
    uint16_t End = HeaderAllocated;

    for (int16_t Layer = 3; Layer < Reference.Length; Layer++)
    {
        for (int16_t Search = 0; Search <= Reference.Indexing[Layer]; Search++)
        {
            if (Pointer >= End) // Overrun
                return {-1, Types::Undefined, nullptr};

            Header Header = HeaderArray[Pointer];

            if (Search == Reference.Indexing[Layer]) // Search sucessful, check if done
            {
                if (Layer == Reference.Length - 1) // End layer
                    return {Pointer, Header.Type, Array + Header.Pointer};
                else if (Layer == Reference.Length - 2 && IsPacked(Header.Type)) // The type is compound, cannot directly go lower
                {
                    switch (Header.Type)
                    {
                    case Types::Text:
                    case Types::Reference:
                    case Types::Colour:
                        return {-2, Types::Byte, Array + Header.Pointer + Reference.Indexing[Layer + 1]};
                    case Types::Vector2D:
                    case Types::Vector3D:
                    case Types::Coord2D:
                    case Types::Coord3D:
                        return {-2, Types::Number, Array + Header.Pointer + Reference.Indexing[Layer + 1] * sizeof(Number)};
                    default:
                        return {-1, Types::Undefined, nullptr};
                    }
                }
                else if (IsPacked(Header.Type) == false) // Can be expanded
                {
                    Pointer++;
                    End = Pointer + Header.Skip + 1;
                    continue;
                }
                else
                    return {-1, Types::Undefined, nullptr};
            }
            Pointer += Header.Skip + 1;
        }
    }
    return {-1, Types::Undefined, nullptr};
};

Types ByteArray::Type(const Reference &Reference) const
{
    return Find(Reference).Type;
};

const void *ByteArray::Get(const Reference &Reference, Types Type) const
{
    FindResult Result = Find(Reference);
    if (Result.Type != Type)
        return nullptr;
    return Result.Value;
}

template <class C>
const C *ByteArray::Get(const Reference &Reference) const
{
    return (C *)Get(Reference, GetType<C>());
}

//Use <char*>
template <>
const char *ByteArray::Get(const Reference &Reference) const
{
    // Find the node and return the pointer to the start of the characters
    FindResult Result = Find(Reference);
    if (Result.Header < 0 || Result.Type != Types::Text) return nullptr;
    
    return (const char*)Result.Value;
}

void ByteArray::Set(const void *Data, size_t Size, Types Type, const Reference &Reference)
{
    FindResult Found = Find(Reference);

    // Packed types
    if (Found.Header == -2)
    {
        if (Type == Found.Type && Found.Value != nullptr)
            memcpy(Found.Value, Data, Size);
        return;
    }

    // Case 2: Target is an Explicit Node (Header exists)
    if (Found.Header == -1)
        Found.Header = Insert(Reference, Size); // New Node
    else if (HeaderArray[Found.Header].Length != Size)
        ResizeData(Found.Header, Size); // Update size

    // Write
    HeaderArray[Found.Header].Type = Type;
    HeaderArray[Found.Header].Length = Size;
    memcpy(Array + HeaderArray[Found.Header].Pointer, Data, Size);
}

template <class C>
void ByteArray::Set(const C &Data, const Reference &Reference)
{
    Set(&Data, sizeof(C), GetType<C>(), Reference);
};

//Use <char*>
template <>
void ByteArray::Set(const char* const &Data, const Reference &Reference)
{
    if (Data == nullptr) return;
    
    // We include the null terminator (+1) so the pointer 
    // returned by Get<> is a valid C-String.
    uint16_t StrLen = (uint16_t)strlen(Data) + 1;
    
    Set(Data, StrLen, Types::Text, Reference);
}

template <>
void ByteArray::Set(const Reference &Data, const Reference &Reference)
{
    if (Data.Length == 0) return;

    char Raw[Data.Length + 1];
    Raw[0] = Data.Length;
    memcpy(Raw+1, Data.Indexing, Data.Length);
    Set(Raw, Data.Length + 1, Types::Reference, Reference);
}

ByteArray ByteArray::CreateMessage() const
{
    uint16_t packedDataSize = 0;
    for (uint16_t i = 0; i < HeaderAllocated; i++) {
        packedDataSize += HeaderArray[i].Length;
    }

    uint16_t headerAreaSize = HeaderAllocated * 4; // Type(1) + Depth(1) + Length(2)
    uint16_t prefixSize = 4;
    uint16_t totalSize = prefixSize + headerAreaSize + packedDataSize;

    ByteArray msg;
    msg.Array = new char[totalSize];
    msg.Length = totalSize;
    msg.Allocated = totalSize;

    // 1. Write Prefix
    memcpy(msg.Array, &totalSize, 2);
    memcpy(msg.Array + 2, &HeaderAllocated, 2);

    // 2. Pack Headers and Data
    char* hPtr = msg.Array + prefixSize;
    char* dPtr = msg.Array + prefixSize + headerAreaSize;

    for (uint16_t i = 0; i < HeaderAllocated; i++) {
        hPtr[0] = (uint8_t)HeaderArray[i].Type;
        hPtr[1] = HeaderArray[i].Depth;
        memcpy(hPtr + 2, &HeaderArray[i].Length, 2);
        hPtr += 4;

        if (HeaderArray[i].Length > 0) {
            memcpy(dPtr, Array + HeaderArray[i].Pointer, HeaderArray[i].Length);
            dPtr += HeaderArray[i].Length;
        }
    }
    return msg;
}

ByteArray ByteArray::ExtractMessage()
{
    if (Length < 4) return ByteArray();

    uint16_t totalWireSize = *(uint16_t*)Array;
    uint16_t count = *(uint16_t*)(Array + 2);
    
    // Bounds check
    if (Length < totalWireSize) return ByteArray();

    ByteArray res;
    res.HeaderAllocated = count;
    res.HeaderArray = new Header[count];

    char* hRead = Array + 4;
    char* dRead = Array + 4 + (count * 4);
    uint16_t internalPtr = 0;

    // Phase 1: Restore Headers & Calculate Pointers
    for (uint16_t i = 0; i < count; i++) {
        res.HeaderArray[i].Type = (Types)hRead[0];
        res.HeaderArray[i].Depth = hRead[1];
        memcpy(&res.HeaderArray[i].Length, hRead + 2, 2);
        hRead += 4;

        // Restore alignment!
        res.HeaderArray[i].Pointer = internalPtr;
        internalPtr += Align(res.HeaderArray[i].Length);
    }

    // Phase 2: Re-align the Data Blob
    res.Length = internalPtr;
    res.Allocated = Align(res.Length);
    res.Array = new char[res.Allocated];
    memset(res.Array, 0, res.Allocated); // Zero-out padding for safety

    for (uint16_t i = 0; i < count; i++) {
        if (res.HeaderArray[i].Length > 0) {
            memcpy(res.Array + res.HeaderArray[i].Pointer, dRead, res.HeaderArray[i].Length);
            dRead += res.HeaderArray[i].Length;
        }
    }

    // Phase 3: Restore the Tree Structure
    res.UpdateSkip();

    return res;
}
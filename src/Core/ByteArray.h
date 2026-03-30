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

// Layer 1: The Handle (The "Where")
struct Bookmark
{
    ByteArray *Map = nullptr;
    uint16_t HeaderIdx = 0;
};

// Layer 2: The Value Snapshot (The "What")
struct SearchResult
{
    void *Value = nullptr; // Direct pointer to the POD data
    Types Type = Types::Undefined;
    uint16_t Length = 0; // 0 = Failure
    Bookmark Location;   // Attached handle for restarting/storing
};

class ByteArray
{
public:
    Header *HeaderArray = nullptr;
    uint16_t HeaderAllocated = 0;
    char *Array = nullptr;
    uint16_t Length = 0;
    uint16_t Allocated = 0;

    ByteArray() {};
    ByteArray(const ByteArray &Copied);
    ~ByteArray();
    ByteArray(const void *data, uint16_t size, Types Type);

    void operator=(const ByteArray &Other);
    ByteArray &operator<<(const ByteArray &Data);
    ByteArray &operator+=(const ByteArray &Data);

    // --- Core Navigation (The New Standard) ---
    // Use Find to get the initial SearchResult
    SearchResult Find(const Reference &Location, bool StopAtReferences = false) const;
    SearchResult Next(const Bookmark &Parent) const;  // Never evaluates references
    SearchResult Child(const Bookmark &Parent) const; // Never evaluates references
    SearchResult This(const Bookmark &Parent) const;  // Always evaluates references

    // --- Data Manipulation ---
    void UpdateSkip();
    void ResizeData(uint16_t HIdx, uint16_t NewSize);
    void Delete(const Reference &Location);
    void InsertAtIndex(uint16_t Idx, Types Type, uint8_t Depth, uint16_t Size);
    uint16_t Insert(const Reference &Location, int16_t NewDataSize);

    // --- Optimized Accessors ---
    // Replaces the old templated Get/Set.
    // Logic: Use SearchResult.Value for reading, SetDirect for writing.
    void SetDirect(const void *Data, size_t Size, Types Type, const Bookmark &Point);
    void Set(const void *Data, size_t Size, Types Type, const Reference &Location);

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

ByteArray::ByteArray(const void *data, uint16_t size, Types Type)
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

    // Initialize the root header with the specific Type
    HeaderAllocated = 1;
    HeaderArray = new Header[1];
    HeaderArray[0].Type = Type;
    HeaderArray[0].Depth = 0;
    HeaderArray[0].Length = size;
    HeaderArray[0].Pointer = 0;
    HeaderArray[0].Skip = 0;

    // Allocate the aligned data buffer
    Array = new char[Allocated];

    if (size > 0)
    {
        if (data != nullptr)
        {
            memcpy(Array, data, size);
        }
        else
        {
            // If size is requested but data is null,
            // zero-initialize the buffer for safety.
            memset(Array, 0, size);
        }
    }
}

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
    SearchResult Found = Find(Location, true);
    if (Found.Length == 0)
        return;

    uint16_t HIdx = Found.Location.HeaderIdx;
    uint16_t NodesToRemove = HeaderArray[HIdx].Skip + 1;

    for (int i = NodesToRemove - 1; i >= 0; i--)
    {
        ResizeData(HIdx + i, 0);
    }

    uint16_t NewCount = HeaderAllocated - NodesToRemove;
    if (NewCount > 0)
    {
        Header *NewHeaders = new Header[NewCount];
        if (HIdx > 0)
            memcpy(NewHeaders, HeaderArray, HIdx * sizeof(Header));
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

    // Correctly initialize the NEW header
    HeaderArray[Idx].Type = Type;
    HeaderArray[Idx].Depth = Depth;
    HeaderArray[Idx].Length = 0;
    HeaderArray[Idx].Pointer = DataPointer;
    HeaderArray[Idx].Skip = 0;

    // Skip calculation must happen BEFORE ResizeData shifts pointers
    UpdateSkip();

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
        uint8_t currentSiblingCount = 0;

        // 1. Move horizontally across siblings at the CURRENT depth
        while (currentSiblingCount < TargetSibling)
        {
            // If we run out of headers at this level, we must insert a "filler" sibling
            if (CurrentIdx >= HeaderAllocated || HeaderArray[CurrentIdx].Depth < Layer)
            {
                InsertAtIndex(CurrentIdx, Types::Undefined, Layer, 0);
            }

            // If the node at CurrentIdx is at a HIGHER depth (Parent level),
            // it means there are no siblings here at all. Insert one.
            if (HeaderArray[CurrentIdx].Depth < Layer)
            {
                InsertAtIndex(CurrentIdx, Types::Undefined, Layer, 0);
            }

            // Skip the current sibling and all its children to find the next sibling
            CurrentIdx += HeaderArray[CurrentIdx].Skip + 1;
            currentSiblingCount++;
        }

        // 2. We are now at the correct Sibling index for this Layer.
        // Check if we need to create this node because it doesn't exist yet.
        if (CurrentIdx >= HeaderAllocated || HeaderArray[CurrentIdx].Depth != Layer)
        {
            InsertAtIndex(CurrentIdx, Types::Undefined, Layer, 0);
        }

        // 3. Prepare for the next layer (Descend)
        // If there are more layers in the path, move CurrentIdx to the first child slot.
        if (Layer < totalDepth - 1)
        {
            CurrentIdx++;
        }
    }

    // 4. We have arrived at the exact HIdx for the Location.
    // Resize it to the requested data size.
    if (NewDataSize > 0)
        ResizeData(CurrentIdx, NewDataSize);

    return CurrentIdx;
}

ByteArray ByteArray::Copy(const Reference &Location) const
{
    SearchResult res = Find(Location);
    if (res.Length == 0)
        return ByteArray();

    const Header &h = res.Location.Map->HeaderArray[res.Location.HeaderIdx];
    ByteArray NewArray;
    NewArray.HeaderArray = new Header[1];
    NewArray.HeaderAllocated = 1;
    NewArray.HeaderArray[0] = {h.Type, 0, h.Length, 0, 0};

    NewArray.Length = h.Length;
    NewArray.Allocated = Align(h.Length);
    if (NewArray.Allocated > 0)
    {
        NewArray.Array = new char[NewArray.Allocated];
        memcpy(NewArray.Array, res.Value, h.Length);
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

void ByteArray::SetDirect(const void *Data, size_t Size, Types Type, const Bookmark &Point)
{
    if (!Point.Map)
        return;

    // If the size is different, we must trigger a re-allocation/shift
    if (Point.Map->HeaderArray[Point.HeaderIdx].Length != Size)
    {
        Point.Map->ResizeData(Point.HeaderIdx, (uint16_t)Size);
    }

    Point.Map->HeaderArray[Point.HeaderIdx].Type = Type;
    if (Size > 0 && Data != nullptr)
    {
        memcpy(Point.Map->Array + Point.Map->HeaderArray[Point.HeaderIdx].Pointer, Data, Size);
    }
}

void ByteArray::Set(const void *Data, size_t Size, Types Type, const Reference &Location)
{
    SearchResult Found = Find(Location, true);

    // If it's a packed sub-value (Vector component), we write directly to memory
    // Note: SearchResult.Location for packed items points to the PARENT header
    if (Found.Length > 0 && Found.Location.HeaderIdx != 0xFFFF)
    {
        // If the type matches the packed expectation, copy directly
        if (Found.Type == Type)
        {
            memcpy(Found.Value, Data, Size);
            return;
        }
    }

    // If not found, insert. If found, update.
    uint16_t HIdx = (Found.Length == 0) ? Insert(Location, (uint16_t)Size) : Found.Location.HeaderIdx;
    SetDirect(Data, Size, Type, {this, HIdx});
}

SearchResult ByteArray::Next(const Bookmark &Parent) const
{
    if (!Parent.Map || Parent.HeaderIdx >= Parent.Map->HeaderAllocated)
        return {};

    Header &h = Parent.Map->HeaderArray[Parent.HeaderIdx];
    uint16_t nextIdx = Parent.HeaderIdx + h.Skip + 1;

    // Boundary check
    if (nextIdx >= Parent.Map->HeaderAllocated)
        return {};

    Header &nextH = Parent.Map->HeaderArray[nextIdx];

    // Sibling check: Must be at the same depth to be a true sibling
    if (nextH.Depth != h.Depth)
        return {};

    return {
        Parent.Map->Array + nextH.Pointer,
        nextH.Type,
        nextH.Length,
        {Parent.Map, nextIdx}};
}

SearchResult ByteArray::Child(const Bookmark &Parent) const
{
    if (!Parent.Map)
        return {};

    Header &h = Parent.Map->HeaderArray[Parent.HeaderIdx];

    // If Skip is 0, this is a leaf node (no children)
    if (h.Skip == 0)
        return {};

    uint16_t childIdx = Parent.HeaderIdx + 1;
    if (childIdx >= Parent.Map->HeaderAllocated)
        return {};

    Header &childH = Parent.Map->HeaderArray[childIdx];

    return {
        Parent.Map->Array + childH.Pointer,
        childH.Type,
        childH.Length,
        {Parent.Map, childIdx}};
}


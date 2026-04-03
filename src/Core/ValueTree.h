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
    ValueTree *Map = nullptr;
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

class ValueTree
{
private:
    void EnsureCapacity(uint16_t required);
    void EnsureHeaderCapacity(uint16_t count);
    
public:
    Header *HeaderArray = nullptr;
    uint16_t HeaderAllocated = 0;
    char *Array = nullptr;
    uint16_t Length = 0;
    uint16_t Allocated = 0;

    ValueTree() {};
    ValueTree(const ValueTree &Copied);
    ~ValueTree();
    ValueTree(const void *data, uint16_t size, Types Type);

    void operator=(const ValueTree &Other);
    ValueTree &operator<<(const ValueTree &Data);

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
    uint16_t InsertNext(const void *Data, uint16_t Size, Types Type, uint16_t CurrentIdx);
    uint16_t InsertChild(const void *Data, uint16_t Size, Types Type, uint16_t ParentIdx);

    ValueTree Copy(const Reference &Location) const;

    // --- Protocol Logic ---
    FlexArray Serialize() const;
    ValueTree Deserialize(const FlexArray &in, uint16_t startIndex);
};

void ValueTree::EnsureCapacity(uint16_t required)
{
    if (required <= Allocated)
        return;

    // Add a small buffer (16 bytes) to prevent constant reallocs
    uint16_t newAlloc = Align(required + 16);

    // realloc handles:
    // 1. Allocating if Array is null
    // 2. Copying old data to new location
    // 3. Freeing old location
    void *newArray = realloc(Array, newAlloc);
    if (newArray)
    {
        Array = (char *)newArray;
        Allocated = newAlloc;
    }
}

// Ensures the Header Array can hold N headers
void ValueTree::EnsureHeaderCapacity(uint16_t count)
{
    // We use exact sizing for headers to save RAM,
    // but you can add slack here if you insert often.
    void *newHeaders = realloc(HeaderArray, count * sizeof(Header));
    if (newHeaders)
    {
        HeaderArray = (Header *)newHeaders;
    }
}

ValueTree::ValueTree(const ValueTree &Copied)
{
    *this = Copied; // Reuse the assignment operator logic
}

ValueTree::~ValueTree()
{
    free(HeaderArray); // free(nullptr) is safe in C/C++
    free(Array);
    HeaderArray = nullptr;
    Array = nullptr;
}

ValueTree::ValueTree(const void *data, uint16_t size, Types Type)
{
    if (size > 0 || Type != Types::Undefined)
    {
        EnsureHeaderCapacity(1);
        HeaderAllocated = 1;
        HeaderArray[0] = {Type, 0, size, 0, 0};

        Length = size;
        EnsureCapacity(size);
        if (data && Array)
            memcpy(Array, data, size);
        else if (Array)
            memset(Array, 0, size);
    }
}

void ValueTree::operator=(const ValueTree &Other)
{
    if (this == &Other)
        return;

    EnsureHeaderCapacity(Other.HeaderAllocated);
    HeaderAllocated = Other.HeaderAllocated;
    memcpy(HeaderArray, Other.HeaderArray, HeaderAllocated * sizeof(Header));

    Length = Other.Length;
    EnsureCapacity(Length);
    if (Other.Array && Array)
        memcpy(Array, Other.Array, Length);
}

ValueTree &ValueTree::operator<<(const ValueTree &Data)
{
    if (Data.HeaderAllocated == 0 || Data.Array == nullptr)
        return *this;
    if (HeaderAllocated == 0)
    {
        *this = Data;
        return *this;
    }

    uint16_t DataOffset = Align(Length);
    uint16_t NewCount = HeaderAllocated + Data.HeaderAllocated;

    EnsureCapacity(DataOffset + Data.Length);
    EnsureHeaderCapacity(NewCount);

    for (uint16_t i = 0; i < Data.HeaderAllocated; i++)
    {
        Header h = Data.HeaderArray[i];
        h.Pointer += DataOffset;
        HeaderArray[HeaderAllocated + i] = h;
    }

    memcpy(Array + DataOffset, Data.Array, Data.Length);
    HeaderAllocated = NewCount;
    Length = DataOffset + Data.Length;

    UpdateSkip();
    return *this;
}

void ValueTree::UpdateSkip()
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

void ValueTree::Delete(const Reference &Location)
{
    SearchResult Found = Find(Location, true);
    if (Found.Length == 0)
        return;

    uint16_t HIdx = Found.Location.HeaderIdx;
    uint16_t NodesToRemove = HeaderArray[HIdx].Skip + 1;

    // Shrink data for all nodes being removed
    for (int i = NodesToRemove - 1; i >= 0; i--)
    {
        ResizeData(HIdx + i, 0);
    }

    // Shift headers left and shrink array
    if (HIdx + NodesToRemove < HeaderAllocated)
    {
        memmove(HeaderArray + HIdx, HeaderArray + HIdx + NodesToRemove,
                (HeaderAllocated - (HIdx + NodesToRemove)) * sizeof(Header));
    }

    HeaderAllocated -= NodesToRemove;
    EnsureHeaderCapacity(HeaderAllocated);
    UpdateSkip();
}

void ValueTree::ResizeData(uint16_t HIdx, uint16_t NewSize)
{
    uint16_t AlignedOld = Align(HeaderArray[HIdx].Length);
    uint16_t AlignedNew = Align(NewSize);
    int16_t Diff = (int16_t)AlignedNew - (int16_t)AlignedOld;

    if (Diff != 0)
    {
        uint16_t TargetPtr = HeaderArray[HIdx].Pointer;
        uint16_t OldEnd = TargetPtr + AlignedOld;

        EnsureCapacity(Length + Diff);

        // Slide the tail of the data buffer
        if (Length > OldEnd)
        {
            memmove(Array + TargetPtr + AlignedNew, Array + OldEnd, Length - OldEnd);
        }

        // Shift all subsequent pointers
        for (uint16_t i = 0; i < HeaderAllocated; i++)
        {
            if (i != HIdx && HeaderArray[i].Pointer >= TargetPtr)
            {
                HeaderArray[i].Pointer += Diff;
            }
        }
        Length += Diff;
    }
    HeaderArray[HIdx].Length = NewSize;
}

void ValueTree::InsertAtIndex(uint16_t Idx, Types Type, uint8_t Depth, uint16_t Size)
{
    uint16_t DataPointer = (Idx < HeaderAllocated) ? HeaderArray[Idx].Pointer : Length;

    // Expand header array and shift existing headers right using memmove
    EnsureHeaderCapacity(HeaderAllocated + 1);
    if (Idx < HeaderAllocated)
    {
        memmove(HeaderArray + Idx + 1, HeaderArray + Idx, (HeaderAllocated - Idx) * sizeof(Header));
    }
    HeaderAllocated++;

    HeaderArray[Idx] = {Type, Depth, 0, DataPointer, 0};

    UpdateSkip();
    if (Size > 0)
        ResizeData(Idx, Size);
}

uint16_t ValueTree::Insert(const Reference &Location, int16_t NewDataSize)
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

ValueTree ValueTree::Copy(const Reference &Location) const
{
    SearchResult res = Find(Location);
    if (res.Length == 0)
        return ValueTree();

    const Header &h = res.Location.Map->HeaderArray[res.Location.HeaderIdx];
    ValueTree NewArray;
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

FlexArray ValueTree::Serialize() const
{
    uint16_t packedDataSize = 0;
    for (uint16_t i = 0; i < HeaderAllocated; i++)
        packedDataSize += HeaderArray[i].Length;

    uint16_t headerAreaSize = HeaderAllocated * 4;
    uint16_t totalSize = 4 + headerAreaSize + packedDataSize;

    FlexArray result(totalSize);
    result.Length = totalSize;

    // Use a single pointer and offsets to keep logic tight
    char *base = result.Array;
    memcpy(base, &totalSize, 2);
    memcpy(base + 2, &HeaderAllocated, 2);

    char *hPtr = base + 4;
    char *dPtr = base + 4 + headerAreaSize;

    for (uint16_t i = 0; i < HeaderAllocated; i++)
    {
        const Header& h = HeaderArray[i];
        
        // Manual packing into the 4-byte wire format
        hPtr[0] = (uint8_t)h.Type;
        hPtr[1] = h.Depth;
        memcpy(hPtr + 2, &h.Length, 2);
        hPtr += 4;

        if (h.Length > 0)
        {
            memcpy(dPtr, Array + h.Pointer, h.Length);
            dPtr += h.Length;
        }
    }

    return result;
}

ValueTree ValueTree::Deserialize(const FlexArray &in, uint16_t startIndex)
{
    // Bounds check for metadata
    if (startIndex + 4 > in.Length) return {};

    const char *src = in.Array + startIndex;
    uint16_t totalWireSize = *(uint16_t *)src;
    uint16_t count = *(uint16_t *)(src + 2);

    // Bounds check for full payload
    if (startIndex + totalWireSize > in.Length) return {};

    ValueTree res;
    if (count == 0) return res;

    // Use our standardized allocation helpers
    res.EnsureHeaderCapacity(count);
    res.HeaderAllocated = count;

    const char *hRead = src + 4;
    const char *dRead = src + 4 + (count * 4);
    uint16_t internalOffset = 0;

    // Pass 1: Setup Headers and calculate internal required length
    for (uint16_t i = 0; i < count; i++)
    {
        Header& h = res.HeaderArray[i];
        h.Type = (Types)hRead[0];
        h.Depth = hRead[1];
        memcpy(&h.Length, hRead + 2, 2);
        hRead += 4;

        h.Pointer = internalOffset;
        internalOffset += Align(h.Length); 
    }

    // Pass 2: Allocate data buffer once and copy data chunks
    res.Length = internalOffset;
    res.EnsureCapacity(res.Length);

    for (uint16_t i = 0; i < count; i++)
    {
        uint16_t len = res.HeaderArray[i].Length;
        if (len > 0)
        {
            memcpy(res.Array + res.HeaderArray[i].Pointer, dRead, len);
            dRead += len;
        }
    }

    res.UpdateSkip();
    return res;
}

void ValueTree::SetDirect(const void *Data, size_t Size, Types Type, const Bookmark &Point)
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

void ValueTree::Set(const void *Data, size_t Size, Types Type, const Reference &Location)
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

SearchResult ValueTree::Next(const Bookmark &Parent) const
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

SearchResult ValueTree::Child(const Bookmark &Parent) const
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

uint16_t ValueTree::InsertNext(const void *Data, uint16_t Size, Types Type, uint16_t CurrentIdx)
{
    uint16_t nextIdx = CurrentIdx + HeaderArray[CurrentIdx].Skip + 1;
    uint8_t depth = HeaderArray[CurrentIdx].Depth;

    InsertAtIndex(nextIdx, Type, depth, Size);
    if (Data && Size > 0) memcpy(Array + HeaderArray[nextIdx].Pointer, Data, Size);
    
    return nextIdx;
}

uint16_t ValueTree::InsertChild(const void *Data, uint16_t Size, Types Type, uint16_t ParentIdx)
{
    uint16_t childIdx = (ParentIdx == 0xFFFF) ? 0 : ParentIdx + 1;
    uint8_t depth = (ParentIdx == 0xFFFF) ? 0 : HeaderArray[ParentIdx].Depth + 1;

    InsertAtIndex(childIdx, Type, depth, Size);
    if (Data && Size > 0) memcpy(Array + HeaderArray[childIdx].Pointer, Data, Size);
    
    return childIdx;
}
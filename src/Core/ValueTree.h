// Align to multiple of 4
#define Align(x) (((x) + 3) & ~3)
#define INVALID_HEADER 0xFFFF
#define INVALID_POINTER 0xFFFF

// Bit masks for 2-bit flag logic
#define MASK_DEPTH 0x3F     // 00111111 (Max depth 63)
#define FLAG_READONLY 0x40  // 01000000 (Bit 6)
#define FLAG_SETUPCALL 0x80 // 10000000 (Bit 7)

enum class Tri : uint8_t
{
    Reset = 0, // Force False
    Set = 1,   // Force True
    Keep = 2   // Leave as-is
};

struct Header
{
    uint16_t Skip = 0;                  // In Headers (8-byte)
    uint16_t Pointer = INVALID_POINTER; // In 4-Bytes (Aligned)
    uint16_t Length = 0;                // In Bytes
    Types Type = Types::Undefined;
    uint8_t Depth = 0; // [Setup][Read-Only][Depth: 6-bits]

    // --- Depth Logic ---

    // Returns depth 0-63, ignoring flags
    uint8_t GetDepth() const
    {
        return Depth & MASK_DEPTH;
    }

    // Sets depth while preserving current flags
    void SetDepth(uint8_t d)
    {
        Depth = (Depth & ~MASK_DEPTH) | (d & MASK_DEPTH);
    }

    // --- Read-Only Flag ---

    bool IsReadOnly() const
    {
        return (Depth & FLAG_READONLY) != 0;
    }

    // --- Setup-Call Flag ---

    bool IsSetupCall() const
    {
        return (Depth & FLAG_SETUPCALL) != 0;
    }

    // Internal helper to apply Tri-bool logic to a specific bit
    void ApplyFlag(uint8_t mask, Tri action)
    {
        if (action == Tri::Set)
            Depth |= mask;
        else if (action == Tri::Reset)
            Depth &= ~mask;
        // if Tri::Keep, do nothing
    }

    void SetReadOnly(Tri action) { ApplyFlag(FLAG_READONLY, action); }
    void SetSetupCall(Tri action) { ApplyFlag(FLAG_SETUPCALL, action); }
};

// Layer 1: The Handle (The "Where")
struct Result
{
    void *Value = nullptr;
    uint16_t Length = 0;
    Types Type = Types::Undefined;
};

struct Bookmark
{
    ValueTree *Map = nullptr;
    uint16_t Index = INVALID_HEADER;

    Result Get() const;
    Result GetThis() const;
    Bookmark Next() const;
    Bookmark Child() const;
    Bookmark This() const;

    void SetCurrent(const void *Data, size_t Size, Types Type, Tri ReadOnly = Tri::Keep, Tri SetupCall = Tri::Keep) const;
    Bookmark SetNext(const void *Data, uint16_t Size, Types Type, Tri ReadOnly = Tri::Keep, Tri SetupCall = Tri::Keep);
    Bookmark SetChild(const void *Data, uint16_t Size, Types Type, Tri ReadOnly = Tri::Keep, Tri SetupCall = Tri::Keep);
};

class ValueTree
{
private:
    BaseClass *Context = nullptr; // The stored callback
    // Memory

    void EnsureCapacity(uint16_t RequiredWords);
    void EnsureHeaderCapacity(uint16_t Required);
    void Clear();
    // Moving
    void ResizeData(uint16_t HIdx, uint16_t NewSize); // Resizes for that header
    void UpdateSkip();                                // Updates skipping based on current depth and ordering
    // Adding
    uint16_t InsertAfter(uint16_t Idx, uint8_t Depth); // Raw insert after header
    uint16_t InsertChild(uint16_t Idx);                // If first true, fail if it exists
    uint16_t InsertNext(uint16_t Idx);                 // Inserts at the same depth after index, skips children
    uint16_t InsertAt(const Reference &Location);      // Fills in
public:
    Header *HeaderArray = nullptr; // Ordered with structure
    char *DataArray = nullptr;     // Ordered by headers
    uint16_t HeaderAllocated = 0;  // In headers (8-bytes)
    uint16_t HeaderLength = 0;     // In headers (8-bytes)
    uint16_t DataAllocated = 0;    // In 4-bytes (Aligned)
    uint16_t DataLength = 0;       // In 4-bytes (Aligned)
    ValueTree(BaseClass *Context) : Context(Context) {}
    ~ValueTree()
    {
        free(HeaderArray);
        free(DataArray);
        HeaderArray = nullptr;
        DataArray = nullptr;
    };
    // Core Navigation
    // Never evaluates references
    Result Get(uint16_t Index) const;
    uint16_t Next(uint16_t Sibling) const;
    uint16_t Child(uint16_t Parent) const;
    // Always evaluates references
    Bookmark This(uint16_t Index) const;
    Result GetThis(uint16_t Index) const;
    // Search
    Bookmark Find(const Reference &Location, bool StopAtReferences = false) const;

    // Data input
    bool SetExisting(const void *Data, size_t Size, Types Type, uint16_t Index, Tri ReadOnly = Tri::Keep, Tri SetupCall = Tri::Keep);        // Set an existing, fails if does not exist
    void Set(const void *Data, size_t Size, Types Type, uint16_t Index, uint8_t Depth, Tri ReadOnly = Tri::Keep, Tri SetupCall = Tri::Keep); // Overwrites/creates
    uint16_t Set(const void *Data, size_t Size, Types Type, const Reference &Location, Tri ReadOnly = Tri::Keep, Tri SetupCall = Tri::Keep); // Overwrites/creates
    uint16_t SetNext(const void *Data, uint16_t Size, Types Type, uint16_t CurrentIdx, Tri ReadOnly = Tri::Keep, Tri SetupCall = Tri::Keep); // Creates if (sibling) does not exist, returns the index of the entry
    uint16_t SetChild(const void *Data, uint16_t Size, Types Type, uint16_t ParentIdx, Tri ReadOnly = Tri::Keep, Tri SetupCall = Tri::Keep); // Creates if (first) does not exist, returns the index of the entry
    void Delete(uint16_t Index);                                                                                                             // Deletes header and data, moves next back to fill in

    // --- Protocol Logic ---
    FlexArray Serialize(bool StorageCompression = false) const;
    bool Deserialize(const FlexArray &in, uint16_t startIndex, bool StorageCompression = false);
    void TriggerSetup(uint16_t index);
    void MarkDirty();
};

void ValueTree::EnsureCapacity(uint16_t RequiredWords)
{
    if (RequiredWords <= DataAllocated)
        return;

    // Growth strategy: add 4 words (16 bytes)
    uint16_t newAllocWords = RequiredWords + 4;
    size_t totalBytes = (size_t)newAllocWords * 4;

    void *newArray = realloc(DataArray, totalBytes);
    if (newArray)
    {
        // Calculate the starting point of the new memory and the size of the gap
        char *startOfNewSpace = (char *)newArray + (DataAllocated * 4);
        size_t newBytesAdded = (newAllocWords - DataAllocated) * 4;

        // Reset the fresh space to zero
        memset(startOfNewSpace, 0, newBytesAdded);

        DataArray = (char *)newArray;
        DataAllocated = newAllocWords;
    }
}

void ValueTree::EnsureHeaderCapacity(uint16_t requiredCount)
{
    if (requiredCount <= HeaderAllocated)
        return;

    uint16_t newAllocCount = requiredCount + 2;
    size_t totalBytes = (size_t)newAllocCount * sizeof(Header);

    void *newHeaders = realloc(HeaderArray, totalBytes);
    if (newHeaders)
    {
        HeaderArray = (Header *)newHeaders;

        // Initialize only the NEWLY allocated headers
        for (uint16_t i = HeaderAllocated; i < newAllocCount; i++)
        {
            HeaderArray[i].Skip = 0;
            HeaderArray[i].Pointer = INVALID_POINTER; // Use the new define
            HeaderArray[i].Length = 0;
            HeaderArray[i].Type = Types::Undefined;
            HeaderArray[i].Depth = 0;
        }

        HeaderAllocated = newAllocCount;
    }
}

void ValueTree::Clear()
{
    HeaderLength = 0;
    DataLength = 0;

    // Reset headers to a safe "empty" state
    if (HeaderArray && HeaderAllocated > 0)
    {
        for (uint16_t i = 0; i < HeaderAllocated; i++)
        {
            HeaderArray[i].Skip = 0;
            HeaderArray[i].Pointer = INVALID_POINTER;
            HeaderArray[i].Length = 0;
            HeaderArray[i].Type = Types::Undefined;
            HeaderArray[i].Depth = 0;
        }
    }

    // Zero out the actual data pool
    if (DataArray && DataAllocated > 0)
    {
        memset(DataArray, 0, DataAllocated * 4);
    }
}

void ValueTree::ResizeData(uint16_t HIdx, uint16_t NewSize)
{
    if (HIdx >= HeaderLength)
        return;

    Header &h = HeaderArray[HIdx];

    // 1. Calculate current word count and target word count
    uint16_t oldWords = (h.Pointer == INVALID_POINTER) ? 0 : Align(h.Length) / 4;
    uint16_t newWords = Align(NewSize) / 4;
    int16_t diff = newWords - oldWords;

    // 2. Resolve Pointer for uninitialized headers
    if (h.Pointer == INVALID_POINTER && NewSize > 0)
    {
        h.Pointer = DataLength; // Default: append to end

        // Search for the first valid pointer following this header to maintain order
        for (uint16_t i = HIdx + 1; i < HeaderLength; i++)
        {
            if (HeaderArray[i].Pointer != INVALID_POINTER)
            {
                h.Pointer = HeaderArray[i].Pointer;
                break;
            }
        }
    }

    // 3. Physical Shift
    if (diff != 0)
    {
        uint16_t moveStart = h.Pointer + oldWords;

        if (diff > 0)
            EnsureCapacity(DataLength + diff);

        uint16_t wordsToMove = DataLength - moveStart;
        if (wordsToMove > 0)
        {
            char *src = DataArray + (moveStart * 4);
            char *dest = DataArray + ((moveStart + diff) * 4);
            memmove(dest, src, wordsToMove * 4);
        }

        // 4. Update logically following headers
        for (uint16_t i = HIdx + 1; i < HeaderLength; i++)
        {
            if (HeaderArray[i].Pointer != INVALID_POINTER)
            {
                HeaderArray[i].Pointer += diff;
            }
        }

        DataLength += diff;
    }

    // 5. Finalize
    h.Length = NewSize;
    if (NewSize == 0)
        h.Pointer = INVALID_POINTER;
}

void ValueTree::UpdateSkip()
{
    for (uint16_t i = 0; i < HeaderLength; i++)
    {
        uint8_t currentDepth = HeaderArray[i].GetDepth();
        uint16_t descendantCount = 0;
        bool foundBoundary = false;

        for (uint16_t j = i + 1; j < HeaderLength; j++)
        {
            // If we hit a node at the same depth or shallower,
            // we've found the start of the NEXT subtree/sibling.
            if (HeaderArray[j].GetDepth() <= currentDepth)
            {
                // The number of descendants is the distance between
                // this node and the next sibling, MINUS 1.
                descendantCount = (j - i) - 1;
                foundBoundary = true;
                break;
            }
        }

        if (!foundBoundary)
        {
            // If no boundary was found, every node remaining
            // in the array after 'i' is a descendant.
            descendantCount = (HeaderLength - 1) - i;
        }

        HeaderArray[i].Skip = descendantCount;
    }
}

uint16_t ValueTree::InsertAfter(uint16_t Idx, uint8_t Depth)
{
    // 1. Calculate the physical insertion index in the HeaderArray.
    // If Idx is INVALID_HEADER, we are inserting at the absolute start (Index 0).
    uint16_t insertPos = (Idx == INVALID_HEADER) ? 0 : Idx + 1;

    // 2. Ensure we have enough allocated RAM for the HeaderArray
    EnsureHeaderCapacity(HeaderLength + 1);

    // 3. Create a gap for the new header
    uint16_t headersToMove = HeaderLength - insertPos;
    if (headersToMove > 0)
    {
        // Move subsequent headers down by one slot
        memmove(&HeaderArray[insertPos + 1], &HeaderArray[insertPos], headersToMove * sizeof(Header));
    }

    // 4. Initialize the new header
    Header &h = HeaderArray[insertPos];
    h.Skip = 0;                  // Will be calculated by UpdateSkip()
    h.Pointer = INVALID_POINTER; // No data in DataArray yet
    h.Length = 0;                // Initial length is 0
    h.Type = Types::Undefined;
    h.SetDepth(Depth); // Set the 6-bit depth while zeroing flags

    // 5. Update tree state
    HeaderLength++;

    // 6. Maintenance: Recalculate traversal skips
    UpdateSkip();

    // We do NOT call ResizeData here because this function only
    // creates the structural "shell." The caller will likely
    // call Set() or ResizeData() next.

    return insertPos;
}

uint16_t ValueTree::InsertChild(uint16_t Idx)
{
    if (Idx >= HeaderLength)
        return INVALID_HEADER;

    uint8_t childDepth = HeaderArray[Idx].GetDepth() + 1;
    if (childDepth > 63)
        return INVALID_HEADER;

    // This pushes everything down and calls UpdateSkip()
    // which will increment the parent's Skip by 1.
    return InsertAfter(Idx, childDepth);
}

uint16_t ValueTree::InsertNext(uint16_t Idx)
{
    if (Idx >= HeaderLength)
        return INVALID_HEADER;

    uint8_t targetDepth = HeaderArray[Idx].GetDepth();

    // With your Skip definition:
    // Idx + Skip is the last header of the subtree.
    // InsertAfter(Idx + Skip) puts the new sibling at Idx + Skip + 1.
    return InsertAfter(Idx + HeaderArray[Idx].Skip, targetDepth);
}

uint16_t ValueTree::InsertAt(const Reference &Location)
{
    // Start at the first root node
    uint16_t currentIdx = 0;
    uint8_t totalDepth = Location.PathLen();

    for (uint8_t layer = 0; layer < totalDepth; layer++)
    {
        uint8_t targetSibling = Location.Path[layer];
        uint8_t currentSiblingCount = 0;

        // 1. If the tree is completely empty, bootstrap the first root
        if (HeaderLength == 0)
        {
            currentIdx = InsertAfter(INVALID_HEADER, 0);
        }

        // 2. Horizontal Navigation: Find the N-th sibling at the current depth
        while (currentSiblingCount < targetSibling)
        {
            // If the current node has a next sibling at this depth, jump to it
            // We know a sibling exists if the node after the current subtree is at the same depth
            uint16_t nextPotential = currentIdx + HeaderArray[currentIdx].Skip + 1;

            if (nextPotential < HeaderLength && HeaderArray[nextPotential].GetDepth() == layer)
            {
                currentIdx = nextPotential;
            }
            else
            {
                // No sibling exists, so create one
                currentIdx = InsertNext(currentIdx);
            }
            currentSiblingCount++;
        }

        // 3. Vertical Navigation: Descend to the next layer
        if (layer < totalDepth - 1)
        {
            uint16_t firstChildIdx = currentIdx + 1;

            // Check if a child exists at the next depth
            if (firstChildIdx < HeaderLength && HeaderArray[firstChildIdx].GetDepth() == (layer + 1))
            {
                currentIdx = firstChildIdx;
            }
            else
            {
                // No child exists, create the first one
                currentIdx = InsertChild(currentIdx);
            }
        }
    }

    return currentIdx;
}

// Public functions

Result ValueTree::Get(uint16_t Index) const
{
    // Use HeaderLength to ensure we are within the active tree
    if (Index >= HeaderLength)
        return {};

    const Header &h = HeaderArray[Index];

    // If there is no data associated with this header, return empty
    if (h.Pointer == INVALID_POINTER)
        return {nullptr, 0, h.Type};

    // DataArray is char*, Pointer is in 4-byte increments
    return {(void *)(DataArray + (h.Pointer * 4)), h.Length, h.Type};
}

Result ValueTree::GetThis(uint16_t Index) const
{
    // This(Index) resolves the Reference if the node contains one.
    // If it's a normal value, it just returns a Bookmark to Index.
    Bookmark target = This(Index);

    if (target.Index == INVALID_HEADER || target.Map == nullptr)
        return {};

    // Access the resolved map's data
    const Header &h = target.Map->HeaderArray[target.Index];

    if (h.Pointer == INVALID_POINTER)
        return {nullptr, 0, h.Type};

    // Use the resolved map's DataArray and the 4-byte offset
    return {(void *)(target.Map->DataArray + (h.Pointer * 4)), h.Length, h.Type};
}

uint16_t ValueTree::Next(uint16_t SiblingIdx) const
{
    if (SiblingIdx >= HeaderLength || HeaderArray == nullptr)
        return INVALID_HEADER;

    // Skip covers all descendants; +1 moves to the next potential sibling/uncle
    if (HeaderArray[SiblingIdx].Skip >= (HeaderLength - SiblingIdx - 1))
    {
        return INVALID_HEADER;
    }

    uint16_t nextIdx = SiblingIdx + HeaderArray[SiblingIdx].Skip + 1;

    // 3. Depth Check (The "Uncle" Check)
    // Now it's safe to read HeaderArray[nextIdx] because we verified the bound above
    if (HeaderArray[nextIdx].GetDepth() != HeaderArray[SiblingIdx].GetDepth())
    {
        return INVALID_HEADER;
    }

    return nextIdx;
}

uint16_t ValueTree::Child(uint16_t ParentIdx) const
{
    // A node with Skip == 0 has no descendants, therefore no children
    if (ParentIdx >= HeaderLength || HeaderArray[ParentIdx].Skip == 0)
        return INVALID_HEADER;

    uint16_t childIdx = ParentIdx + 1;

    // Verify boundary and depth (Child must be ParentDepth + 1)
    if (childIdx >= HeaderLength ||
        HeaderArray[childIdx].GetDepth() != (HeaderArray[ParentIdx].GetDepth() + 1))
    {
        return INVALID_HEADER;
    }

    return childIdx;
}

// Setters

bool ValueTree::SetExisting(const void *Data, size_t Size, Types Type, uint16_t Index, Tri ReadOnly, Tri SetupCall)
{
    // 1. Boundary Check: If it doesn't exist, we fail immediately.
    if (Index >= HeaderLength)
    {
        return false;
    }

    Header &h = HeaderArray[Index];

    // 2. Update Metadata
    // We use the Tri-bool logic to potentially keep existing flags.
    h.SetReadOnly(ReadOnly);
    h.SetSetupCall(SetupCall);
    h.Type = Type;

    // 3. Resize and Copy
    // ResizeData handles the physical memory shift in DataArray.
    ResizeData(Index, (uint16_t)Size);

    if (Data && Size > 0)
    {
        // h.Pointer is guaranteed valid (or adjusted) by ResizeData
        memcpy(DataArray + (h.Pointer * 4), Data, Size);
    }

    // 5. Success
    if (h.IsSetupCall())
        TriggerSetup(Index);

    if (h.IsReadOnly() == false)
        MarkDirty();
    return true;
}

void ValueTree::Set(const void *Data, size_t Size, Types Type, uint16_t Index, uint8_t Depth, Tri ReadOnly, Tri SetupCall)
{
    // 1. Fill the gap: If Index is beyond the current length,
    // we must insert "filler" nodes until we reach the target Index.
    while (Index >= HeaderLength)
    {
        // InsertAfter(HeaderLength - 1) always appends to the physical end.
        // These fillers are created with the requested Depth.
        InsertAfter(HeaderLength - 1, Depth);
    }

    // 2. We are now guaranteed that Index exists.
    Header &h = HeaderArray[Index];

    // 3. Update the depth (in case the existing node was at a different level)
    h.SetDepth(Depth);

    // 4. Delegate to SetExisting to handle flags, resizing, data copy,
    // TriggerSetup, and MarkDirty.
    SetExisting(Data, Size, Type, Index, ReadOnly, SetupCall);
}

uint16_t ValueTree::Set(const void *Data, size_t Size, Types Type, const Reference &Location, Tri ReadOnly, Tri SetupCall)
{
    // 1. Ensure path exists (InsertAt returns existing or creates fillers)
    uint16_t index = InsertAt(Location);

    // 2. Use our existing logic to apply data and metadata
    SetExisting(Data, Size, Type, index, ReadOnly, SetupCall);
    return index;
}

uint16_t ValueTree::SetNext(const void *Data, uint16_t Size, Types Type, uint16_t CurrentIdx, Tri ReadOnly, Tri SetupCall)
{
    if (CurrentIdx >= HeaderLength)
        return INVALID_HEADER;

    uint8_t depth = HeaderArray[CurrentIdx].GetDepth();

    // The next sibling is physically located after the current node + all its descendants
    uint16_t targetIdx = CurrentIdx + HeaderArray[CurrentIdx].Skip + 1;

    // Check if the node at that position is already a sibling
    bool exists = (targetIdx < HeaderLength && HeaderArray[targetIdx].GetDepth() == depth);

    if (!exists)
    {
        targetIdx = InsertNext(CurrentIdx);
    }

    // Apply data and metadata
    SetExisting(Data, Size, Type, targetIdx, ReadOnly, SetupCall);

    return targetIdx;
}

uint16_t ValueTree::SetChild(const void *Data, uint16_t Size, Types Type, uint16_t ParentIdx, Tri ReadOnly, Tri SetupCall)
{
    if (ParentIdx >= HeaderLength)
        return INVALID_HEADER;

    uint8_t childDepth = HeaderArray[ParentIdx].GetDepth() + 1;
    uint16_t targetIdx = ParentIdx + 1;

    // Check if a node already exists at the first child position with the right depth
    bool exists = (targetIdx < HeaderLength && HeaderArray[targetIdx].GetDepth() == childDepth);

    if (!exists)
    {
        targetIdx = InsertChild(ParentIdx);
    }

    // Apply data and metadata
    // Note: SetExisting handles MarkDirty and TriggerSetup
    SetExisting(Data, Size, Type, targetIdx, ReadOnly, SetupCall);

    return targetIdx;
}

void ValueTree::Delete(uint16_t Index)
{
    if (Index >= HeaderLength)
        return;

    // 1. Identify the range to delete
    // Skip is the number of descendants.
    // Total headers to remove = 1 (the node) + Skip (the descendants).
    uint16_t countToRemove = 1 + HeaderArray[Index].Skip;

    // 2. Data Cleanup: We must remove the data blocks for all nodes in this range
    // To keep it simple and preserve the "Double Shift" logic:
    for (uint16_t i = 0; i < countToRemove; i++)
    {
        // ResizeData(Index, 0) effectively removes the data block and
        // shifts all following data in the DataArray left.
        ResizeData(Index, 0);

        // Note: We don't increment Index here because after ResizeData,
        // the "next" node's header has shifted into the current Index position.
    }

    // 3. Header Cleanup: Shift the HeaderArray left
    uint16_t moveStart = Index + countToRemove;
    uint16_t headersToShift = HeaderLength - moveStart;

    if (headersToShift > 0)
    {
        memmove(&HeaderArray[Index], &HeaderArray[moveStart], headersToShift * sizeof(Header));
    }

    // 4. Update Tree State
    HeaderLength -= countToRemove;

    // 5. Maintenance
    UpdateSkip();
    MarkDirty();
}

// Interfacing

FlexArray ValueTree::Serialize(bool StorageCompression) const
{
    uint16_t packedDataSize = 0;

    // 1. First pass: Calculate required buffer size
    for (uint16_t i = 0; i < HeaderLength; i++)
    {
        // If StorageCompression is on, we skip the data payload for Read-Only nodes
        // (Assuming the receiver already has this static data or doesn't need it)
        if (StorageCompression && HeaderArray[i].IsReadOnly())
            continue;

        packedDataSize += HeaderArray[i].Length;
    }

    // headerAreaSize = 4 bytes per active header
    uint16_t headerAreaSize = HeaderLength * 4;
    // 4 bytes for global header [TotalSize(2)][HeaderCount(2)]
    uint16_t totalSize = 4 + headerAreaSize + packedDataSize;

    FlexArray result(totalSize);
    if (!result.Array)
        return {}; // Allocation failure check

    result.Length = totalSize;
    char *base = (char *)result.Array;

    // 2. Write Global Header
    memcpy(base, &totalSize, 2);
    memcpy(base + 2, &HeaderLength, 2);

    char *hWirePtr = base + 4;
    char *dWirePtr = base + 4 + headerAreaSize;

    // 3. Second pass: Pack Headers and Data
    for (uint16_t i = 0; i < HeaderLength; i++)
    {
        const Header &h = HeaderArray[i];
        bool skipData = StorageCompression && h.IsReadOnly();

        // Write Wire Header (4 bytes)
        // We preserve h.Depth because it contains our Depth, ReadOnly, and Setup bits
        hWirePtr[0] = skipData ? (uint8_t)Types::Undefined : (uint8_t)h.Type;
        hWirePtr[1] = h.Depth;

        uint16_t wireLen = skipData ? 0 : h.Length;
        memcpy(hWirePtr + 2, &wireLen, 2);

        hWirePtr += 4;

        // Write Data Payload
        if (wireLen > 0 && h.Pointer != INVALID_POINTER)
        {
            // Remember: DataArray is aligned via (h.Pointer * 4)
            memcpy(dWirePtr, DataArray + (h.Pointer * 4), wireLen);
            dWirePtr += wireLen;
        }
    }

    return result;
}

bool ValueTree::Deserialize(const FlexArray &in, uint16_t startIndex, bool ExtraCompression)
{
    // 1. Basic Safety Check
    if (startIndex + 4 > in.Length)
        return false;

    const char *InputSource = (const char *)in.Array + startIndex;
    uint16_t TotalInSize, InHeaderCount;
    memcpy(&TotalInSize, InputSource, 2);
    memcpy(&InHeaderCount, InputSource + 2, 2);

    if (startIndex + TotalInSize > in.Length)
        return false;

    // 2. Adjust Header capacity and count
    // If the incoming tree is larger, we grow.
    EnsureHeaderCapacity(InHeaderCount);

    // If the incoming tree is smaller, we'll truncate at the end.
    // If we want a perfect sync, we should probably Clear() or handle truncation.
    // For now, let's assume the wire is the "Truth" for the first N headers.
    uint16_t oldHeaderLength = HeaderLength;
    HeaderLength = InHeaderCount;

    const char *HeaderInRead = InputSource + 4;
    const char *DataInRead = InputSource + 4 + (InHeaderCount * 4);
    const char *currentDataPtr = DataInRead;

    for (uint16_t i = 0; i < InHeaderCount; i++)
    {
        Types wireType = (Types)HeaderInRead[0];
        uint8_t wireDepth = HeaderInRead[1]; // Includes Depth, ReadOnly, and Setup bits
        uint16_t wireLen;
        memcpy(&wireLen, HeaderInRead + 2, 2);
        HeaderInRead += 4;

        Header &h = HeaderArray[i];

        // --- 1. COMPRESSION SKIP ---
        // If ExtraCompression is on, the wire Type for Read-Only nodes is Undefined
        // and wireLen is 0. We leave the local data untouched.
        if (ExtraCompression && h.IsReadOnly())
        {
            // Even if we don't update data, we might need to skip the currentDataPtr
            // if the wire actually sent data (though Serialize doesn't).
            continue;
        }

        // --- 2. SURGICAL RESIZE ---
        // ResizeData handles the memmove in DataArray and pointer updates for i+1..N
        if (h.Length != wireLen || i >= oldHeaderLength)
        {
            ResizeData(i, wireLen);
        }

        // --- 3. METADATA UPDATE ---
        h.Type = wireType;
        h.Depth = wireDepth;

        // --- 4. DATA COPY ---
        if (wireLen > 0)
        {
            // Use aligned pointer math: DataArray + (h.Pointer * 4)
            memcpy(DataArray + (h.Pointer * 4), currentDataPtr, wireLen);
            currentDataPtr += wireLen;
        }

        // --- 5. SETUP TRIGGER ---
        // Only trigger if specifically requested and NOT a read-only skip
        if (h.IsSetupCall() && !h.IsReadOnly())
        {
            TriggerSetup(i);
        }
    }

    // 6. Tree Integrity
    UpdateSkip();

    // We don't MarkDirty() during Deserialize because we are
    // receiving the data, not originating a change.
    return true;
}

// Bookmark
// --- Bookmark Implementation ---

inline Result Bookmark::Get() const
{
    if (!Map || Index == INVALID_HEADER)
        return {};
    return Map->Get(Index);
}

inline Result Bookmark::GetThis() const
{
    if (!Map || Index == INVALID_HEADER)
        return {};
    return Map->GetThis(Index);
}

inline Bookmark Bookmark::Next() const
{
    if (!Map || Index == INVALID_HEADER)
        return {Map, INVALID_HEADER};
    return {Map, Map->Next(Index)};
}

inline Bookmark Bookmark::Child() const
{
    if (!Map || Index == INVALID_HEADER)
        return {Map, INVALID_HEADER};
    return {Map, Map->Child(Index)};
}

inline Bookmark Bookmark::This() const
{
    if (!Map || Index == INVALID_HEADER)
        return {Map, INVALID_HEADER};
    return Map->This(Index); // ValueTree::This returns a Bookmark
}

// --- Bookmark Setters (aligned with Tri logic) ---

inline void Bookmark::SetCurrent(const void *Data, size_t Size, Types Type, Tri ReadOnly, Tri SetupCall) const
{
    if (Map && Index != INVALID_HEADER)
    {
        // We use SetExisting because the Bookmark already points to a specific slot
        Map->SetExisting(Data, Size, Type, Index, ReadOnly, SetupCall);
    }
}

inline Bookmark Bookmark::SetNext(const void *Data, uint16_t Size, Types Type, Tri ReadOnly, Tri SetupCall)
{
    if (!Map || Index == INVALID_HEADER)
        return {Map, INVALID_HEADER};

    uint16_t nextIdx = Map->SetNext(Data, Size, Type, Index, ReadOnly, SetupCall);
    return {Map, nextIdx};
}

inline Bookmark Bookmark::SetChild(const void *Data, uint16_t Size, Types Type, Tri ReadOnly, Tri SetupCall)
{
    if (!Map || Index == INVALID_HEADER)
        return {Map, INVALID_HEADER};

    uint16_t childIdx = Map->SetChild(Data, Size, Type, Index, ReadOnly, SetupCall);
    return {Map, childIdx};
}
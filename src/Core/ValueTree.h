// Align to multiple of 4
#define Align(x) (((x) + 3) & ~3)
#define INVALID_HEADER 0xFFFF

// Bit masks for 2-bit flag logic
#define MASK_DEPTH 0x3F     // 00111111 (Max depth 63)
#define FLAG_READONLY 0x40  // 01000000 (Bit 6)
#define FLAG_SETUPCALL 0x80 // 10000000 (Bit 7)

struct Header
{
    Types Type = Types::Undefined;
    uint8_t Depth = 0; // [Setup][Read-Only][Depth: 6-bits]
    uint16_t Length = 0;
    uint16_t Pointer = 0;
    uint16_t Skip = 0;

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

    void SetReadOnly(bool enabled)
    {
        if (enabled)
            Depth |= FLAG_READONLY;
        else
            Depth &= ~FLAG_READONLY;
    }

    // --- Setup-Call Flag ---

    bool IsSetupCall() const
    {
        return (Depth & FLAG_SETUPCALL) != 0;
    }

    void SetSetupCall(bool enabled)
    {
        if (enabled)
            Depth |= FLAG_SETUPCALL;
        else
            Depth &= ~FLAG_SETUPCALL;
    }
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
};

class ValueTree
{
private:
    void EnsureCapacity(uint16_t required);
    void EnsureHeaderCapacity(uint16_t count);

    BaseClass *Context = nullptr; // The stored callback
public:
    Header *HeaderArray = nullptr;
    uint16_t HeaderAllocated = 0;
    char *Array = nullptr;
    uint16_t Length = 0;
    uint16_t Allocated = 0;

    ValueTree() : Context(nullptr) {};
    ValueTree(BaseClass *Context) : Context(Context) {}
    ValueTree(const ValueTree &Copied);
    ~ValueTree();
    ValueTree(const void *data, uint16_t size, Types Type);

    void operator=(const ValueTree &Other);
    ValueTree &operator<<(const ValueTree &Data);

    Result Get(uint16_t Index) const;
    Result Get(const Bookmark &Sibling) const;
    Result GetThis(uint16_t Index) const;
    Result GetThis(const Bookmark &Sibling) const;

    // Core Navigation
    uint16_t Next(uint16_t Sibling) const; // Never evaluates references
    uint16_t Child(uint16_t Parent) const; // Never evaluates references
    Bookmark Next(const Bookmark &Sibling) const;
    Bookmark Child(const Bookmark &Parent) const;

    Bookmark This(uint16_t Index) const; // Always evaluates references
    Bookmark This(const Bookmark &Parent) const;
    Bookmark Find(const Reference &Location, bool StopAtReferences = false) const;

    // --- Data Manipulation ---
    void UpdateSkip();
    void ResizeData(uint16_t HIdx, uint16_t NewSize);
    void Delete(uint16_t Index);
    void Delete(const Reference &Location);
    void InsertAtIndex(uint16_t Idx, Types Type, uint8_t Depth, uint16_t Size);
    uint16_t Insert(const Reference &Location, int16_t NewDataSize);

    // Setters
    void Set(const void *Data, size_t Size, Types Type, uint16_t Index, bool ReadOnly = false, bool SetupCall = false);
    void Set(const void *Data, size_t Size, Types Type, const Bookmark &Point, bool ReadOnly = false, bool SetupCall = false);
    void Set(const void *Data, size_t Size, Types Type, const Reference &Location, bool ReadOnly = false, bool SetupCall = false);
    uint16_t InsertNext(const void *Data, uint16_t Size, Types Type, uint16_t CurrentIdx, bool ReadOnly = false, bool SetupCall = false);
    uint16_t InsertChild(const void *Data, uint16_t Size, Types Type, uint16_t ParentIdx, bool ReadOnly = false, bool SetupCall = false);
    Bookmark InsertNext(const void *Data, uint16_t Size, Types Type, const Bookmark &CurrentIdx, bool ReadOnly = false, bool SetupCall = false);
    Bookmark InsertChild(const void *Data, uint16_t Size, Types Type, const Bookmark &ParentIdx, bool ReadOnly = false, bool SetupCall = false);

    // --- Protocol Logic ---
    FlexArray Serialize(bool ExtraCompression = false) const;
    bool Deserialize(const FlexArray &in, uint16_t startIndex, bool ExtraCompression = false);
    void TriggerSetup(uint16_t index);
    void MarkDirty();
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
        if (HeaderArray[i].GetDepth() == 0)
            continue;

        uint8_t targetParentDepth = HeaderArray[i].GetDepth() - 1;
        for (int p = i - 1; p >= 0; p--)
        {
            if (HeaderArray[p].GetDepth() == targetParentDepth)
            {
                HeaderArray[p].Skip += (HeaderArray[i].Skip + 1);
                break;
            }
            if (HeaderArray[p].GetDepth() < targetParentDepth)
                break;
        }
    }
}

void ValueTree::Delete(const Reference &Location)
{
    // The Reference version now just resolves the index and hands off to the core
    Bookmark Found = Find(Location, true);
    if (Found.Index != INVALID_HEADER)
    {
        Delete(Found.Index);
    }
}

void ValueTree::Delete(uint16_t Index)
{
    if (Index >= HeaderAllocated)
        return;

    // 1. Identify the entire range of nodes (Parent + all children)
    uint16_t NodesToRemove = HeaderArray[Index].Skip + 1;
    uint16_t RangeEndIdx = Index + NodesToRemove;

    // 2. Identify the data block to remove
    // The data for this tree segment starts at the first node's pointer
    // and ends at the end of the last descendant's data.
    uint16_t DataStart = HeaderArray[Index].Pointer;

    // To find the total data size of this branch, we find the
    // pointer of the node immediately AFTER this branch.
    uint16_t DataEnd;
    if (RangeEndIdx < HeaderAllocated)
        DataEnd = HeaderArray[RangeEndIdx].Pointer;
    else
        DataEnd = Length;

    uint16_t DataBytesToRemove = DataEnd - DataStart;

    // 3. Shift the Data Array (Close the gap)
    if (DataBytesToRemove > 0 && DataEnd < Length)
    {
        memmove(Array + DataStart, Array + DataEnd, Length - DataEnd);
    }
    Length -= DataBytesToRemove;

    // 4. Update all remaining Headers' pointers
    for (uint16_t i = RangeEndIdx; i < HeaderAllocated; i++)
    {
        HeaderArray[i].Pointer -= DataBytesToRemove;
    }

    // 5. Shift the Header Array
    if (RangeEndIdx < HeaderAllocated)
    {
        memmove(HeaderArray + Index,
                HeaderArray + RangeEndIdx,
                (HeaderAllocated - RangeEndIdx) * sizeof(Header));
    }

    HeaderAllocated -= NodesToRemove;

    // 6. Finalize
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
            if (CurrentIdx >= HeaderAllocated || HeaderArray[CurrentIdx].GetDepth() < Layer)
            {
                InsertAtIndex(CurrentIdx, Types::Undefined, Layer, 0);
            }

            // If the node at CurrentIdx is at a HIGHER depth (Parent level),
            // it means there are no siblings here at all. Insert one.
            if (HeaderArray[CurrentIdx].GetDepth() < Layer)
            {
                InsertAtIndex(CurrentIdx, Types::Undefined, Layer, 0);
            }

            // Skip the current sibling and all its children to find the next sibling
            CurrentIdx += HeaderArray[CurrentIdx].Skip + 1;
            currentSiblingCount++;
        }

        // 2. We are now at the correct Sibling index for this Layer.
        // Check if we need to create this node because it doesn't exist yet.
        if (CurrentIdx >= HeaderAllocated || HeaderArray[CurrentIdx].GetDepth() != Layer)
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

FlexArray ValueTree::Serialize(bool ExtraCompression) const
{
    uint16_t packedDataSize = 0;
    for (uint16_t i = 0; i < HeaderAllocated; i++)
    {
        // If compressing, ReadOnly data is omitted from the total size
        if (ExtraCompression && HeaderArray[i].IsReadOnly())
            continue;
        packedDataSize += HeaderArray[i].Length;
    }

    uint16_t headerAreaSize = HeaderAllocated * 4;
    uint16_t totalSize = 4 + headerAreaSize + packedDataSize;

    FlexArray result(totalSize);
    result.Length = totalSize;

    char *base = result.Array;
    memcpy(base, &totalSize, 2);
    memcpy(base + 2, &HeaderAllocated, 2);

    char *hPtr = base + 4;
    char *dPtr = base + 4 + headerAreaSize;

    for (uint16_t i = 0; i < HeaderAllocated; i++)
    {
        const Header &h = HeaderArray[i];
        bool maskAsUndefined = ExtraCompression && h.IsReadOnly();

        hPtr[0] = maskAsUndefined ? (uint8_t)Types::Undefined : (uint8_t)h.Type;
        hPtr[1] = h.Depth;

        uint16_t wireLen = maskAsUndefined ? 0 : h.Length;
        memcpy(hPtr + 2, &wireLen, 2);
        hPtr += 4;

        if (wireLen > 0)
        {
            memcpy(dPtr, Array + h.Pointer, wireLen);
            dPtr += wireLen;
        }
    }

    return result;
}

bool ValueTree::Deserialize(const FlexArray &in, uint16_t startIndex, bool ExtraCompression)
{
    if (startIndex + 4 > in.Length)
        return false;

    const char *src = in.Array + startIndex;
    uint16_t totalWireSize, wireCount;
    memcpy(&totalWireSize, src, 2);
    memcpy(&wireCount, src + 2, 2);

    if (startIndex + totalWireSize > in.Length)
        return false;

    // Surgical mode usually requires the skeleton to match.
    if (ExtraCompression && wireCount != HeaderAllocated)
        return false;

    const char *hRead = src + 4;
    const char *dRead = src + 4 + (wireCount * 4);
    const char *currentDataPtr = dRead;

    for (uint16_t i = 0; i < wireCount; i++)
    {
        Types wireType = (Types)hRead[0];
        uint8_t wireDepth = hRead[1];
        uint16_t wireLen;
        memcpy(&wireLen, hRead + 2, 2);
        hRead += 4;

        Header &h = this->HeaderArray[i];

        // --- 1. COMPRESSION SKIP ---
        if (ExtraCompression && wireType == Types::Undefined)
        {
            // If it's a "Keep Local" signal, we skip data copy but
            // check if the local node ALREADY has a setup flag that needs firing.
            // (Optional: usually you only trigger on change, but this is safer)
            if (h.IsSetupCall())
                TriggerSetup(i);
            continue;
        }

        // --- 2. SURGICAL RESIZE ---
        if (h.Length != wireLen)
        {
            this->ResizeData(i, wireLen);
        }

        // --- 3. METADATA UPDATE ---
        h.Type = wireType;
        h.Depth = wireDepth;

        // --- 4. DATA COPY ---
        if (wireLen > 0)
        {
            memcpy(this->Array + h.Pointer, currentDataPtr, wireLen);
            currentDataPtr += wireLen;
        }

        // --- 5. SETUP TRIGGER ---
        // If the node (either from wire or local flags) is a Setup Call, fire it now.
        if (h.IsSetupCall() && h.IsReadOnly() == false)
            this->TriggerSetup(i);
        if (h.IsReadOnly() == false && ExtraCompression == false)
            MarkDirty();
    }

    this->UpdateSkip();
    return true;
}

// Thin calls

inline Result ValueTree::Get(uint16_t Index) const
{
    if (Index >= HeaderAllocated)
        return {};
    const Header &h = HeaderArray[Index];
    return {Array + h.Pointer, h.Length, h.Type};
}

inline Result ValueTree::Get(const Bookmark &Point) const
{
    if (!Point.Map)
        return {};
    return Point.Map->Get(Point.Index);
}

inline Result ValueTree::GetThis(uint16_t Index) const
{
    Bookmark Value = This(Index);
    if (Value.Index == INVALID_HEADER)
        return {};
    const Header &h = Value.Map->HeaderArray[Value.Index];

    return {Value.Map->Array + h.Pointer, h.Length, h.Type};
}

inline Result ValueTree::GetThis(const Bookmark &Point) const
{
    if (!Point.Map)
        return {};
    return Point.Map->GetThis(Point.Index);
}

inline Bookmark ValueTree::Next(const Bookmark &Sibling) const
{
    if (!Sibling.Map)
        return {};
    return {Sibling.Map, Sibling.Map->Next(Sibling.Index)};
}

inline Bookmark ValueTree::Child(const Bookmark &Parent) const
{
    if (!Parent.Map)
        return {};
    return {Parent.Map, Parent.Map->Child(Parent.Index)};
}

inline Bookmark ValueTree::This(const Bookmark &Point) const
{
    if (!Point.Map)
        return {};
    return Point.Map->This(Point.Index);
}

inline void ValueTree::Set(const void *Data, size_t Size, Types Type, const Bookmark &Point, bool ReadOnly, bool SetupCall)
{
    if (Point.Map)
        Point.Map->Set(Data, Size, Type, Point.Index, ReadOnly, SetupCall);
}

inline void ValueTree::Set(const void *Data, size_t Size, Types Type, const Reference &Location, bool ReadOnly, bool SetupCall)
{
    Bookmark target = Find(Location, true);
    if (target.Index == INVALID_HEADER)
    {
        Insert(Location, (int16_t)Size); // Use your existing path-builder
        // After insert, we have to find it again or have Insert return the index
        target = Find(Location, true);
    }
    Set(Data, Size, Type, target.Index, ReadOnly, SetupCall);
}

inline Bookmark ValueTree::InsertNext(const void *Data, uint16_t Size, Types Type, const Bookmark &Point, bool ReadOnly, bool SetupCall)
{
    if (!Point.Map)
        return {};
    return {Point.Map, Point.Map->InsertNext(Data, Size, Type, Point.Index, ReadOnly, SetupCall)};
}

inline Bookmark ValueTree::InsertChild(const void *Data, uint16_t Size, Types Type, const Bookmark &Point, bool ReadOnly, bool SetupCall)
{
    if (!Point.Map)
        return {};
    return {Point.Map, Point.Map->InsertChild(Data, Size, Type, Point.Index, ReadOnly, SetupCall)};
}

// Functions themselves
// --- Navigation (Raw uint16_t) ---

uint16_t ValueTree::Next(uint16_t Sibling) const
{
    if (Sibling >= HeaderAllocated)
        return INVALID_HEADER;

    uint16_t nextIdx = Sibling + HeaderArray[Sibling].Skip + 1;

    // Boundary and Depth check: If depth changes, it's not a sibling
    if (nextIdx >= HeaderAllocated || HeaderArray[nextIdx].GetDepth() != HeaderArray[Sibling].GetDepth())
        return INVALID_HEADER;

    return nextIdx;
}

uint16_t ValueTree::Child(uint16_t Parent) const
{
    if (Parent >= HeaderAllocated || HeaderArray[Parent].Skip == 0)
        return INVALID_HEADER;

    uint16_t childIdx = Parent + 1;

    // A child must be exactly one level deeper than the parent
    if (childIdx >= HeaderAllocated || HeaderArray[childIdx].GetDepth() != HeaderArray[Parent].GetDepth() + 1)
        return INVALID_HEADER;

    return childIdx;
}

void ValueTree::Set(const void *Data, size_t Size, Types Type, uint16_t Index, bool ReadOnly, bool SetupCall)
{
    if (Index >= HeaderAllocated)
    {
        if (Index == 0 && Size > 0 && Data != nullptr)
        {
            InsertAtIndex(0, Type, 0, Size);
            memcpy(Array + HeaderArray[Index].Pointer, Data, Size);
        }
        else
            return;
    }

    // 1. Check if we need to resize the existing data slot
    if (HeaderArray[Index].Length != (uint16_t)Size)
    {
        ResizeData(Index, (uint16_t)Size);
    }

    // 2. Update Metadata
    HeaderArray[Index].Type = Type;
    HeaderArray[Index].SetReadOnly(ReadOnly);
    HeaderArray[Index].SetSetupCall(SetupCall);

    // 3. Copy Data
    if (Data && Size > 0)
    {
        memcpy(Array + HeaderArray[Index].Pointer, Data, Size);
    }

    if (SetupCall)
        TriggerSetup(Index);

    if (ReadOnly == false)
        MarkDirty();
}

uint16_t ValueTree::InsertNext(const void *Data, uint16_t Size, Types Type, uint16_t CurrentIdx, bool ReadOnly, bool SetupCall)
{
    if (CurrentIdx >= HeaderAllocated)
        return INVALID_HEADER;

    // Calculate where the next sibling should live
    uint16_t nextIdx = CurrentIdx + HeaderArray[CurrentIdx].Skip + 1;
    uint8_t depth = HeaderArray[CurrentIdx].GetDepth();

    InsertAtIndex(nextIdx, Type, depth, Size);

    if (Data && Size > 0)
        memcpy(Array + HeaderArray[nextIdx].Pointer, Data, Size);

    HeaderArray[nextIdx].SetReadOnly(ReadOnly);
    HeaderArray[nextIdx].SetSetupCall(SetupCall);

    if (SetupCall)
        TriggerSetup(nextIdx);

    if (ReadOnly == false)
        MarkDirty();

    return nextIdx;
}

uint16_t ValueTree::InsertChild(const void *Data, uint16_t Size, Types Type, uint16_t ParentIdx, bool ReadOnly, bool SetupCall)
{
    uint16_t childIdx;
    uint8_t depth;

    if (ParentIdx == INVALID_HEADER) // Root Case
    {
        childIdx = 0;
        depth = 0;
    }
    else
    {
        if (ParentIdx >= HeaderAllocated)
            return INVALID_HEADER;
        childIdx = ParentIdx + 1;
        depth = HeaderArray[ParentIdx].GetDepth() + 1;
    }

    InsertAtIndex(childIdx, Type, depth, Size);

    if (Data && Size > 0)
        memcpy(Array + HeaderArray[childIdx].Pointer, Data, Size);

    HeaderArray[childIdx].SetReadOnly(ReadOnly);
    HeaderArray[childIdx].SetSetupCall(SetupCall);

    if (SetupCall)
        TriggerSetup(childIdx);

    if (ReadOnly == false)
        MarkDirty();

    return childIdx;
}
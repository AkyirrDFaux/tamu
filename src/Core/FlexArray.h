class FlexArray
{
public:
    char *Array = nullptr;
    uint16_t Length = 0;    // Logical size used
    uint16_t Allocated = 0; // Physical capacity

    FlexArray();
    FlexArray(uint16_t Preallocate);
    FlexArray(const char *source, uint16_t size);
    ~FlexArray();

    // Copy Constructor
    FlexArray(const FlexArray &other);
    // Copy Assignment Operator
    FlexArray &operator=(const FlexArray &other);
    // Move Constructor (High Performance)
    FlexArray(FlexArray &&other) noexcept;

    void Append(const char *source, uint16_t length);
    // The Concatenation Operator
    FlexArray &operator+=(const FlexArray &other);
    void Consume(uint16_t n);

    // Helper to ensure capacity
    bool Reserve(uint16_t totalRequired);
    FlexArray PrependLength() const;
    FlexArray ExtractByLength();
};

FlexArray::FlexArray() : Array(nullptr), Length(0), Allocated(0)
{
}

FlexArray::FlexArray(const FlexArray &other)
{
    if (other.Array && other.Length > 0)
    {
        Array = (char *)malloc(other.Length);
        if (Array)
        {
            memcpy(Array, other.Array, other.Length);
            Length = other.Length;
            Allocated = other.Length;
        }
    }
    else
    {
        Array = nullptr;
        Length = 0;
        Allocated = 0;
    }
}

FlexArray::FlexArray(FlexArray &&other) noexcept
{
    // Steal the pointers
    Array = other.Array;
    Length = other.Length;
    Allocated = other.Allocated;

    // Neutralize the other object so its destructor doesn't free our new memory
    other.Array = nullptr;
    other.Length = 0;
    other.Allocated = 0;
}

FlexArray &FlexArray::operator=(const FlexArray &other)
{
    if (this == &other)
        return *this; // Self-assignment check

    // 1. Clean up existing memory
    if (Array)
        free(Array);

    // 2. Duplicate the other array
    if (other.Array && other.Length > 0)
    {
        Array = (char *)malloc(other.Length);
        if (Array)
        {
            memcpy(Array, other.Array, other.Length);
            Length = other.Length;
            Allocated = other.Length;
        }
    }
    else
    {
        Array = nullptr;
        Length = 0;
        Allocated = 0;
    }
    return *this;
}

FlexArray::FlexArray(uint16_t Preallocate)
{
    Array = (char *)malloc(Preallocate);
    if (Array)
    {
        Allocated = Preallocate;
        Length = 0;
        memset(Array, 0, Allocated);
    }
    else
    {
        Allocated = 0;
        Length = 0;
    }
}

FlexArray::FlexArray(const char *source, uint16_t size) : Array(nullptr), Length(0), Allocated(0)
{
    Append(source, size);
}

FlexArray::~FlexArray()
{
    if (Array != nullptr)
    {
        free(Array);
        Array = nullptr;
    }
}

void FlexArray::Append(const char *source, uint16_t length)
{
    // 1. Safety check: Nothing to add
    if (source == nullptr || length == 0)
    {
        return;
    }

    // 2. Calculate the new total logical length
    uint16_t newLength = Length + length;

    // 3. Ensure the physical buffer is large enough
    if (Reserve(newLength))
    {
        // 4. Copy new data starting at the current end (Length)
        memcpy(Array + Length, source, length);

        // 5. Update the logical length tracker
        Length = newLength;
    }
}

FlexArray &FlexArray::operator+=(const FlexArray &other)
{
    Append(other.Array, other.Length);
    return *this;
}

void FlexArray::Consume(uint16_t n)
{
    // 1. If we are consuming more than or equal to what we have, just reset
    if (n >= Length)
    {
        Length = 0;
        return;
    }

    // 2. If n is 0, do nothing
    if (n == 0) return;

    // 3. Calculate remaining bytes
    uint16_t remaining = Length - n;

    // 4. Shift the data: [Current Data + n] moves to [Current Data]
    // memmove is mandatory here because source and destination overlap
    memmove(Array, Array + n, remaining);

    // 5. Update logical length
    Length = remaining;
}

bool FlexArray::Reserve(uint16_t totalRequired)
{
    if (totalRequired <= Allocated)
        return true;

    // Realloc handles the expansion:
    // It tries to grow in-place, otherwise it moves the block and frees the old one.
    char *newPtr = (char *)realloc(Array, totalRequired);

    if (newPtr != nullptr)
    {
        Array = newPtr;
        Allocated = totalRequired;
        return true;
    }

    return false; // Out of Memory
}

FlexArray FlexArray::PrependLength() const
{
    if (Length == 0)
        return FlexArray();

    // 1. Create a new array with space for header + data
    uint16_t totalSize = Length + 2;
    FlexArray result(totalSize);
    result.Length = totalSize;

    // 2. Write the 2-byte Length header (original data length)
    uint16_t originalLen = Length;
    memcpy(result.Array, &originalLen, 2);

    // 3. Copy the original data after the header
    memcpy(result.Array + 2, Array, originalLen);

    return result;
}

FlexArray FlexArray::ExtractByLength()
{
    if (Length < 2) return FlexArray();

    uint16_t payloadLen = *(uint16_t *)Array;

    if (Length < (payloadLen + 2)) return FlexArray();

    // Create the result from the payload (skipping the 2-byte header)
    FlexArray result(Array + 2, payloadLen);

    // Use our new method to clean up the front of the array
    Consume(payloadLen + 2);

    return result;
}
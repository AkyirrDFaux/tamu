// 1. The Local Navigator (Used by ByteArray)
#define MAX_REF_LENGTH 12
struct Reference
{
    uint8_t Metadata; // Bit 7: 1=Global, 0=Local | Bits 0-6: Path Length, 255: invalid
    union
    {
        uint8_t Data[MAX_REF_LENGTH + 3];
        struct
        {
            uint8_t Net, Group, Device;
            uint8_t Path[MAX_REF_LENGTH];
        } __attribute__((packed));
    };

    // --- Constructors ---

    // Default: Empty/Invalid
    Reference() : Metadata(255)
    {
        for (uint8_t i = 0; i < (MAX_REF_LENGTH + 3); i++)
            Data[i] = 0;
    }

    // Implicit List Constructor: Defaults to Local
    // This allows: Values.Set(val, {0, 1});
    Reference(std::initializer_list<uint8_t> p) : Metadata(0)
    {
        for (uint8_t i = 0; i < (MAX_REF_LENGTH + 3); i++)
            Data[i] = 0;
        uint8_t len = (uint8_t)p.size();
        if (len > MAX_REF_LENGTH)
            len = MAX_REF_LENGTH;

        Metadata = len; // Global bit 7 stays 0
        uint8_t i = 0;
        for (uint8_t val : p)
            Path[i++] = val;
    }

    // --- Identification ---
    inline bool IsGlobal() const { return Metadata & 0x80; }
    inline uint8_t PathLen() const { return Metadata & 0x7F; }
    inline bool IsValid() const {return Metadata != 255;}

    // --- Factories ---
    // Keep Local() for explicit clarity if needed
    static Reference Local(std::initializer_list<uint8_t> p)
    {
        return Reference(p);
    }

    static Reference Global(uint8_t n, uint8_t g, uint8_t d, std::initializer_list<uint8_t> p = {})
    {
        Reference r;
        r.Net = n;
        r.Group = g;
        r.Device = d;
        uint8_t len = (uint8_t)p.size();
        if (len > MAX_REF_LENGTH)
            len = MAX_REF_LENGTH;

        r.Metadata = len | 0x80;
        for (uint8_t i = 0; i < len; i++)
            r.Path[i] = p.begin()[i];
        return r;
    }

    // --- Equality ---
    bool operator==(const Reference &other) const
    {
        if (PathLen() != other.PathLen())
            return false;

        if (IsGlobal() && other.IsGlobal()) // Both global, compare full
        {
            uint8_t limit = PathLen() + 3;
            for (uint8_t i = 0; i < limit; i++)
            {
                if (Data[i] != other.Data[i])
                    return false;
            }
        }
        else // At least one local, compare only paths
        {
            uint8_t limit = PathLen();
            for (uint8_t i = 0; i < limit; i++)
            {
                if (Path[i] != other.Path[i])
                    return false;
            }
        }
        return true;
    }

    bool operator!=(const Reference &other) const { return !(*this == other); }

    Reference Append(uint8_t part) const
    {
        // 1. Create a copy of the current reference
        Reference r = *this;

        // 2. Check for validity and bounds
        uint8_t currentLen = PathLen();
        if (!IsValid() || currentLen >= MAX_REF_LENGTH)
            return r; // Return unchanged if invalid or full

        // 3. Add the new part to the path
        r.Path[currentLen] = part;

        // 4. Increment the path length bit (bits 0-6 of Metadata)
        // Bit 7 (Global/Local) is preserved automatically
        r.Metadata++;

        return r;
    }
};
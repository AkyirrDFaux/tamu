// 1. The Local Navigator (Used by ByteArray)
struct Path
{
    uint8_t Length;
    uint8_t *Indexing;

    Path() : Length(0), Indexing(nullptr) {}

    // 1. Fix: Initializer list constructor (Deep Copy)
    Path(std::initializer_list<uint8_t> list) : Length((uint8_t)list.size())
    {
        if (Length > 0)
        {
            Indexing = new uint8_t[Length];
            // Cast the start of the list to a raw pointer and copy the bytes
            memcpy(Indexing, list.begin(), Length);
        }
        else
        {
            Indexing = nullptr;
        }
    }

    // 2. Data constructor (Deep Copy)
    Path(const uint8_t *data, uint8_t len) : Length(len)
    {
        if (len > 0 && data != nullptr)
        {
            Indexing = new uint8_t[len];
            memcpy(Indexing, data, len);
        }
        else
        {
            Indexing = nullptr;
        }
    }

    // 3. Copy Constructor (Deep Copy)
    Path(const Path &other) : Length(other.Length)
    {
        if (other.Length > 0 && other.Indexing != nullptr)
        {
            Indexing = new uint8_t[other.Length];
            memcpy(Indexing, other.Indexing, other.Length);
        }
        else
        {
            Indexing = nullptr;
        }
    }

    // 4. Copy Assignment Operator
    Path &operator=(const Path &other)
    {
        if (this != &other)
        {
            uint8_t *new_data = nullptr;
            if (other.Length > 0 && other.Indexing != nullptr)
            {
                new_data = new uint8_t[other.Length];
                memcpy(new_data, other.Indexing, other.Length);
            }

            delete[] Indexing; // Delete old AFTER successful allocation
            Indexing = new_data;
            Length = other.Length;
        }
        return *this;
    }

    // 5. Destructor
    ~Path()
    {
        delete[] Indexing;
    }

    bool operator==(const Path &other) const
    {
        if (Length != other.Length || Indexing == nullptr || other.Indexing == nullptr)
            return false;
        if (Length == 0)
            return true; // Both are empty, they are equal
        for (uint8_t i = 0; i < Length; i++)
        {
            if (Indexing[i] != other.Indexing[i])
                return false;
        }
        return true;
    }

    bool operator!=(const Path &other) const { return !(*this == other); }
};

// 2. The Global Address (Used for Routing/Network)
struct Reference
{
    uint8_t Net, Group, Device;
    Path Location;

    // Helper to quickly get the Path part for local functions
    Reference(uint8_t n, uint8_t d, uint8_t c, Path p = Path())
        : Net(n), Group(d), Device(c), Location(p) {}

    bool operator==(const Reference &other) const
    {
        return (Net == other.Net &&
                Group == other.Group &&
                Device == other.Device &&
                Location == other.Location);
    }

    // Inequality operator
    bool operator!=(const Reference &other) const
    {
        return !(*this == other);
    }
};
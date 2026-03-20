// 1. The Local Navigator (Used by ByteArray)
struct Path
{
    uint8_t Length;
    const uint8_t *Indexing;

    Path(std::initializer_list<uint8_t> list)
        : Length((uint8_t)list.size()), Indexing(list.begin()) {}
    
    Path(const uint8_t* data, uint8_t len) 
        : Length(len), Indexing(data) {}

    Path() : Length(0), Indexing(nullptr) {}

    bool operator==(const Path& other) const {
        // 1. Quick length check
        if (Length != other.Length) 
            return false;

        // 2. Compare relevant indices
        for (uint8_t i = 0; i < Length; i++) {
            if (Indexing[i] != other.Indexing[i]) 
                return false;
        }

        return true;
    }

    bool operator!=(const Path& other) const {
        return !(*this == other);
    }
};

// 2. The Global Address (Used for Routing/Network)
struct Reference
{
    uint8_t Net, Group, Device;
    Path Location;

    // Helper to quickly get the Path part for local functions
    Reference(uint8_t n, uint8_t d, uint8_t c, Path p = Path()) 
        : Net(n), Group(d), Device(c), Location(p) {}

    bool operator==(const Reference& other) const {
        return (Net == other.Net && 
                Group == other.Group && 
                Device == other.Device && 
                Location == other.Location);
    }

    // Inequality operator
    bool operator!=(const Reference& other) const {
        return !(*this == other);
    }
};
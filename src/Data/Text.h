class Text
{
public:
    char *Data;
    uint16_t Length;

    // Add this to your Text class
    Text(const char* Buffer, uint16_t Len) : Data(nullptr), Length(0) 
    {
        if (Buffer && Len > 0) 
        {
            Length = Len;
            Data = new char[Length + 1]; // +1 for safety/null termination
            memcpy(Data, Buffer, Length);
            Data[Length] = '\0'; 
        }
    }

    Text(const char *String) : Data(nullptr), Length(0) { *this = String; }

    ~Text()
    {
        if (Data)
            delete[] Data;
    }

    Text &operator=(const char *String)
{
    if (Data) {
        delete[] Data;
        Data = nullptr;
    }

    if (String)
    {
        // 1. Get length
        uint16_t rawLen = strlen(String);
        Length = rawLen; 
        
        // 2. Allocate space for the chars AND the null terminator
        // Even if you don't send the \0, having it here prevents 
        // string functions from overreading into other memory.
        Data = new char[Length + 1]; 
        memcpy(Data, String, Length);
        Data[Length] = '\0'; // Manually terminate
    }
    else {
        Length = 0;
    }
    return *this;
}

    Text(const Text &Other) : Data(nullptr), Length(Other.Length) // Copy Constructor
    {
        if (Other.Data)
        {
            Data = new char[Other.Length];
            memcpy(Data, Other.Data, Other.Length);
        }
    }

    Text &operator=(const Text &Other) // Assignment Operator
    {
        if (this == &Other) return *this;
        if (Data) delete[] Data;

        Length = Other.Length;
        if (Other.Data)
        {
            Data = new char[Length];
            memcpy(Data, Other.Data, Length);
        }
        else Data = nullptr;

        return *this;
    }
};
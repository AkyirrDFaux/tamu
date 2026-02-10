class Text
{
public:
    char *Data;
    uint8_t Length;

    Text() : Data(nullptr), Length(0) {}

    Text(const char *s) : Data(nullptr), Length(0) { *this = s; }

    ~Text()
    {
        if (Data)
            delete[] Data;
    }

    // Copying a string (The "=" Operator)
    Text &operator=(const char *String)
    {
        // 1. Clean up old data
        if (Data)
        {
            delete[] Data;
            Data = nullptr;
        }

        if (String)
        {
            // 2. Calculate and clamp length
            size_t NewLength = strlen(String);
            Length = (NewLength > 255) ? 255 : (uint8_t)NewLength;

            // 3. Allocate new RAM and copy
            Data = new char[Length];
            memcpy(Data, String, Length);
        }
        else
            Length = 0;
        return *this;
    }

        Text(const Text &Other) : Data(nullptr), Length(Other.Length) // Copy Constructor
    {
        if (Other.Data){
            Data = new char[Other.Length];
            memcpy(Data, Other.Data, Other.Length);
        }
    }
};
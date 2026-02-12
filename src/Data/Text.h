class Text
{
public:
    char *Data;
    uint8_t Length;

    Text() : Data(nullptr), Length(0) {}

    Text(const char *String) : Data(nullptr), Length(0) { *this = String; }

    ~Text()
    {
        if (Data)
            delete[] Data;
    }

    Text &operator=(const char *String)
    {
        if (Data)
        {
            delete[] Data;
            Data = nullptr;
        }

        if (String)
        {
            size_t NewLength = strlen(String);
            if (NewLength > 255)
                Length = 255;
            else
                Length = (uint8_t)NewLength;
            Data = new char[Length];
            memcpy(Data, String, Length);
        }
        else
            Length = 0;
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
};
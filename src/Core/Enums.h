enum class ObjectTypes : uint8_t
{
    Undefined = 0,
    Shape2D,
    Board,
    Port,
    Fan,
    LEDStrip,
    LEDSegment,
    Texture1D,
    Display,
    Geometry2D,
    Texture2D,
    AccGyr, 
    Servo, 
    Input, 
    Operation, 
    Program
};

enum class Functions : uint8_t
{
    None = 0,
    CreateObject, // Create new
    DeleteObject, // Delete object
    SaveObject,   // Save to file
    SaveAll,
    LoadObject, // Create from ByteArray
    ReadObject, // Send to app
    ReadType,
    ReadName,
    WriteName,
    SetFlags, // Swapped
    ReadModules,
    SetModules,
    WriteValue,
    ReadValue,
    ReadDatabase, // Debug, Serial only now
    ReadFile,
    RunFile,
    Refresh
};

void Run(ByteArray &Input);
void ReadDatabase(ByteArray &Input);
void ReadValue(ByteArray &Input);
void WriteValue(ByteArray &Input);
void CreateObject(ByteArray &Input);
void DeleteObject(ByteArray &Input);
void SaveObject(ByteArray &Input);
void WriteName(ByteArray &Input);
void ReadName(ByteArray &Input);
void ReadFile(ByteArray &Input);
void SaveAll(ByteArray &Input);
void ReadObject(ByteArray &Input);
void LoadObject(ByteArray &Input);
void RunFile(ByteArray &Input);
void Refresh(ByteArray &Input);
void SetModules(ByteArray &Input);
void SetFlags(ByteArray &Input);

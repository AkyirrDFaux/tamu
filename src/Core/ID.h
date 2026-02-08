#define RandomID (1 << 8)
#define NoID 0

class IDClass
{
public:
    uint32_t ID = NoID;

    IDClass(uint32_t Number = NoID) { ID = Number; };
    IDClass(uint32_t Main, uint8_t Sub) { ID = (Main << 8) + Sub; };

    void operator=(uint32_t Other) { ID = Other; };
    void operator=(IDClass Other) { ID = Other.ID; };
    bool operator==(uint32_t Other) { return ID == Other; };
    bool operator==(IDClass Other) { return ID == Other.ID; };
    operator uint32_t() { return ID; };

    uint32_t Base() { return (ID >> 8); };
    uint8_t Sub() { return (uint8_t)ID; };
    IDClass Main() { return IDClass(Base(), 0); };
    uint8_t ValueIndex() { return Sub() - 1; };

    //String ToString() { return String(Base()) + "." + String(Sub()); }
};
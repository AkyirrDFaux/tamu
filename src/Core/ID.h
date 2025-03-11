#define RandomID 1
#define NoID 0

class IDClass
{
public:
    uint32_t ID = NoID;

    IDClass(uint32_t Number = NoID) { ID = Number; };

    void operator=(uint32_t Other) { ID = Other; };
    void operator=(IDClass Other) { ID = Other.ID; };
    bool operator==(uint32_t Other) { return ID == Other; };
    bool operator==(IDClass Other) { return ID == Other.ID; };
    operator uint32_t() { return ID; };
};
/*
template <>
ByteArray::ByteArray(IDClass Data)
{
    *this = ByteArray(Data.ID);
};

template <>
IDClass ByteArray::As()
{
    IDClass New;
    New.ID = SubArray(0);
    return New;
};*/
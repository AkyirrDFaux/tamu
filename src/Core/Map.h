template <class C, class D>
class Map // Immutable
{
public:
    C *SetA = nullptr;
    D *SetB = nullptr;
    int32_t Length = 0;

    void Add(C ObjectA, D ObjectB);
    int32_t Find(C Object);
    int32_t Find(D Object);
    C FindOther(D Key);
    D FindOther(C Key);
};

template <class C, class D>
void Map<C, D>::Add(C ObjectA, D ObjectB)
{
    C *NewSetA = new C[Length + 1];
    D *NewSetB = new D[Length + 1];

    for (int32_t Index = 0; Index < Length; Index++) // Copy existing
    {
        NewSetA[Index] = SetA[Index];
        NewSetB[Index] = SetB[Index];
    }

    if (Length != 0)
    {
        delete[] SetA;
        delete[] SetB;
    }

    SetA = NewSetA;
    SetB = NewSetB;

    SetA[Length] = ObjectA;
    SetB[Length] = ObjectB;

    Length = Length + 1;
};
template <class C, class D>
int32_t Map<C, D>::Find(C Object)
{
    for (int32_t Index = 0; Index < Length; Index++) // Copy existing
    {
        if (SetA[Index] == Object)
            return Index;
    }
    return -1;
};
template <class C, class D>
int32_t Map<C, D>::Find(D Object)
{
    for (int32_t Index = 0; Index < Length; Index++) // Copy existing
    {
        if (SetB[Index] == Object)
            return Index;
    }
    return -1;
};
template <class C, class D>
C Map<C, D>::FindOther(D Key) {
    int32_t Index = Find(Key);
    if (Index == -1)
        Serial.println("Nonexistent key!");
    
    return SetA[Index];
};
template <class C, class D>
D Map<C, D>::FindOther(C Key) {
    int32_t Index = Find(Key);
    if (Index == -1)
        Serial.println("Nonexistent key!");
    
    return SetB[Index];
};
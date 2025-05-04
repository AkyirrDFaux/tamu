template <class C = BaseClass>
class ObjectList
{
public:
    C **Object = nullptr;
    int32_t Length = 0;

    ~ObjectList() { RemoveAll(); };

    C *operator[](int32_t Index) const;
    C *At(int32_t Index) const;
    bool IsValid(int32_t Index) const;
    void Iterate(int32_t *Index) const;

    void Expand(int32_t NewLength);
    void Shorten();

    bool Add(C *AddObject, int32_t Index = -1);
    bool Remove(int32_t Index);
    bool Remove(C *RemovedObject);
    void RemoveAll();
};

template <class C>
C *ObjectList<C>::operator[](int32_t Index) const
{
    return At(Index);
};

template <class C>
C *ObjectList<C>::At(int32_t Index) const // Returns address or nullptr if invalid
{
    if (Index >= Length || -Index > Length)
        return nullptr;

    if (Index < 0) // From end
        Index = Length + Index;

    return Object[Index];
};

template <class C>
bool ObjectList<C>::IsValid(int32_t Index) const // Returns if object at index is valid
{
    return (At(Index) != nullptr);
};

template <class C>
void ObjectList<C>::Iterate(int32_t *Index) const
{
    (*Index)++;
    while (!IsValid(*Index) && *Index <= Length)
        (*Index)++;
};

template <class C>
void ObjectList<C>::Expand(int32_t NewLength) // Expands list to new length
{
    if (NewLength <= Length)
        return;

    C **NewObject = new C *[NewLength]; // Make larger array

    for (int32_t Index = 0; Index < Length; Index++) // Copy existing
        NewObject[Index] = Object[Index];
    for (int32_t Index = Length; Index < NewLength; Index++) // Add empty to new
        NewObject[Index] = nullptr;

    if (Length != 0) // Replace
        delete[] Object;

    Object = NewObject;
    Length = NewLength;
};

template <class C>
void ObjectList<C>::Shorten()
{
    int32_t NewLength = Length;
    while (At(NewLength - 1) == nullptr && NewLength > 0)
        NewLength--;

    if (NewLength == Length)
        return;

    C **NewObject = new C *[NewLength]; // New Array

    for (int32_t Index = 0; Index < NewLength; Index++) // Copy existing
        NewObject[Index] = Object[Index];

    if (Length != 0) // Replace
        delete[] Object;

    Object = NewObject;
    Length = NewLength;
}

template <class C>
bool ObjectList<C>::Add(C *AddObject, int32_t Index) // Add an item to list, supports -1, fails if occupied
{
    if (Index == -1) // Add to end
        Index = Length;

    if (Index >= Length) // Expand if Index is too far
        Expand(Index + 1);
    else if (At(Index) != nullptr) // Occupied
        return false;

    Object[Index] = AddObject; // Write
    return true;
};

template <class C>
bool ObjectList<C>::Remove(int32_t Index) // Removes object
{
    if (Index < Length && At(Index) != nullptr && Index >= 0)
    {
        Object[Index] = nullptr;
        Shorten();
        return true;
    }
    return false;
};

template <class C>
bool ObjectList<C>::Remove(C *RemovedObject) // Removes object
{
    for (int32_t Index = 0; Index < Length; Index++)
    {
        if (At(Index) == RemovedObject)
            Remove(Index);
    }
    return true;
};

template <class C>
void ObjectList<C>::RemoveAll()
{
    delete[] Object;
    Object = nullptr;
    Length = 0;
};
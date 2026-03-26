template <>
ByteArray::ByteArray(const BaseClass &Data)
{
    int32_t Index = Objects.Search(&Data);
    if (Index == -1) return;

    // 1. Initialize with the Global ID of the object
    // Extract Group and Device from the packed 16-bit ID in the registry
    uint8_t G = (uint8_t)(Objects.Object[Index].ID >> 8);
    uint8_t D = (uint8_t)(Objects.Object[Index].ID & 0xFF);
    
    *this = ByteArray(Reference::Global(0, G, D));

    // 2. Chain the object metadata and its internal Value tree
    // This creates a contiguous "Blob" representing the entire Object state
    *this = *this << ByteArray(Data.Type)
                  << ByteArray(Objects.Object[Index].Info)
                  << ByteArray(Data.Name)
                  << Data.Values;
}

template <class C>
Getter<C> BaseClass::ValueGet(const Reference &Location) const
{
    // Direct passthrough to the internal bit-tagged ByteArray
    return Values.Get<C>(Location);
}

template <class C>
bool BaseClass::ValueSetup(C Value, const Reference &Location)
{
    // 1. Update the internal data tree (Handles Resize/Insert automatically)
    Values.Set(Value, Location);

    // 2. Trigger the VTable Setup to react to this change
    // Useful for updating hardware pins or internal state based on the new value
    Setup(Location);

    return true;
}
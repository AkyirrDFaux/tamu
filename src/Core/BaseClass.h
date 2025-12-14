BaseClass *CreateObject(ObjectTypes Type, bool New = true, IDClass ID = RandomID, FlagClass Flags = Flags::None);

void BaseClass::AddModule(BaseClass *Object, int32_t Index)
{
    Modules.Add(Object, Index);
    Object->ReferencesCount++;
};

template <>
ByteArray::ByteArray(const BaseClass &Data)
{
    // Type, ID, Flags, Name, Modules,(+ values)
    *this = ByteArray(Data.Type) << ByteArray(Data.ID) << ByteArray(Data.Flags) << ByteArray(Data.Name) << ByteArray(Data.Modules) << Data.GetValue();
};

String BaseClass::ContentDebug(int32_t Level)
{
    String Text = "";
    for (int32_t Index = 0; Index < Level; Index++)
        Text += "-..";

    Text += "ID: ";
    Text += ID.ToString();
    Text += ", Type: ";
    Text += String((uint8_t)Type);
    Text += " , Name: ";
    Text += Name + '\n';

    for (int32_t Index = Modules.FirstValid(); Index < Modules.Length; Modules.Iterate(&Index))
        Text += Modules[Index]->ContentDebug(Level + 1);

    return Text;
};
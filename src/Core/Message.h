class Message
{
public:
    ObjectList<ByteArray> Segments;

    template <class C>
    void Add(const C &Input);

    Message();
    template <class C>
    Message(const C &Input1);
    template <class C, class D>
    Message(const C &Input1, const D &Input2);
    template <class C, class D, class E>
    Message(const C &Input1, const D &Input2, const E &Input3);
    template <class C, class D, class E, class F>
    Message(const C &Input1, const D &Input2, const E &Input3, const F &Input4);

    uint32_t Length() const;
    String AsText() const;
    Types Type(int32_t Index) const;
    template <class C>
    C Data(int32_t Index) const;

    Message *Run();
    ~Message();
};

Message::Message() {};

template <class C>
Message::Message(const C &Input1)
{
    Add(Input1);
};
template <class C, class D>
Message::Message(const C &Input1, const D &Input2)
{
    Add(Input1);
    Add(Input2);
};
template <class C, class D, class E>
Message::Message(const C &Input1, const D &Input2, const E &Input3)
{
    Add(Input1);
    Add(Input2);
    Add(Input3);
};

template <class C, class D, class E, class F>
Message::Message(const C &Input1, const D &Input2, const E &Input3, const F &Input4)
{
    Add(Input1);
    Add(Input2);
    Add(Input3);
    Add(Input4);
};

Message::~Message()
{
    for (int32_t Index = 0; Index < Segments.Length; Segments.Iterate(&Index))
        delete Segments[Index];
    Segments.RemoveAll();
};

uint32_t Message::Length() const
{
    uint32_t Length = 0;
    for (int32_t Index = 0; Index < Segments.Length; Segments.Iterate(&Index))
        Length += Segments[Index]->Length;
    return Length;
};

template <class C>
void Message::Add(const C &Input)
{
    ByteArray *Data = new ByteArray();
    *Data = *Data << ByteArray(GetType<C>()) << ByteArray(Input);
    Segments.Add(Data);
};

template <>
void Message::Add<ByteArray>(const ByteArray &Input) // Take address, will double free otherwise
{
    // Segmenting, if data is packed
    int32_t Index = 0;
    while (Index < Input.Length - 1)
    {
        ByteArray *Data = new ByteArray();
        int32_t DataLength = Input.SubArray(Index).SizeOfData();

        *Data = Input.SubArray(Index, sizeof(Types) + DataLength);
        Segments.Add(Data);
        Index = Index + sizeof(Types) + DataLength;
    }
};

template <>
void Message::Add<Message>(const Message &Input) // Take address, will double free otherwise
{
    for (int32_t Index = 0; Index < Input.Segments.Length; Input.Segments.Iterate(&Index))
    {
        ByteArray *Data = new ByteArray();
        *Data = *Input.Segments[Index];
        Segments.Add(Data);
    }
};

Types Message::Type(int32_t Index) const
{
    if (!Segments.IsValid(Index))
        return Types::Undefined;

    return Segments[Index]->Type();
};

template <class C>
C Message::Data(int32_t Index) const
{
    // Assume the data is there
    return Segments[Index]->Data<C>();
};

String Message::AsText() const
{
    String Text = "";
    for (int32_t Index = 0; Index < Segments.Length; Segments.Iterate(&Index))
    {
        switch (Type(Index))
        {
        case Types::ID:
            Text += "ID: " + String(Data<IDClass>(Index).ID);
            break;
        case Types::Type:
            Text += "TY: " + String((u_int8_t)Data<Types>(Index));
            break;
        case Types::Text:
            Text += "TX: " + Data<String>(Index);
            break;
        case Types::Function:
            Text += "FN: " + String((u_int8_t)Data<Functions>(Index)); // Write the codes instead later
            break;
        case Types::Status:
            Text += "ST: " + String((u_int8_t)Data<Status>(Index)); // Write out for clearer understanding
            break;
        case Types::Integer:
            Text += "IN: " + String(Data<int32_t>(Index));
            break;
        case Types::Number:
            Text += "NU: " + String(Data<float>(Index));
            break;
        case Types::Vector2D:
            Text += "V2: " + Data<Vector2D>(Index).AsString();
            break;
        case Types::Coord2D:
            Text += "C2: " + Data<Coord2D>(Index).AsString();
            break;
        case Types::Colour:
            Text += "CO: " + Data<ColourClass>(Index).AsString();
            break;
        case Types::Flags:
            Text += "FL: " + String(Data<uint8_t>(Index), 2);
            break;
        case Types::IDList:
            Text += "IL: " + Data<IDList>(Index).AsString();
            break;
        default:
            Text += String((u_int8_t)Type(Index)) + ": ";
            if (GetValueSize(Type(Index)) == 1)
                Text += String(Data<uint8_t>(Index));
            else
                Text += "...";

            break;
        }
        if (Index != Segments.Length - 1)
            Text += ", ";
    }
    return Text;
};

float *Extract(String Text, int32_t Number)
{
    int32_t Index = 0;
    float *Array = new float[Number];

    for (int32_t Counter = 0; Counter < Number; Counter++)
    {
        Array[Counter] = 0;
        while (Text[Index] == ' ') // Skip initial spaces
            Index++;
        int32_t Stop = Text.indexOf(" ", Index); // Find next space
        if (Stop == -1)
            Stop = Text.length();

        Array[Counter] = Text.substring(Index, Stop).toFloat();
        Index = Stop;
    };
    return Array;
};

Message *Deserialize(String Input)
{
    Message *M = new Message();
    int32_t Index = 0;
    while (Index < Input.length())
    {
        while (Input[Index] == ' ') // Skip initial spaces
            Index++;
        int32_t End = Input.indexOf(",", Index + 3); // Find end ","
        if (End == -1)
            End = Input.length(); // If there is no "," it is end of string

        String Type = Input.substring(Index, Index + 2); // First 2 letters (and :)
        String Data = Input.substring(Index + 3, End);   // Everything else
        Data.trim();

        if (Type == "FN")
        {
            if (Data == "DB")
                M->Add(Functions::ReadDatabase);
            else if (Data == "RV")
                M->Add(Functions::ReadValue);
            else if (Data == "WV")
                M->Add(Functions::WriteValue);
            else if (Data == "CO")
                M->Add(Functions::CreateObject);
            else if (Data == "DO")
                M->Add(Functions::DeleteObject);
            else if (Data == "RN")
                M->Add(Functions::ReadName);
            else if (Data == "WN")
                M->Add(Functions::WriteName);
            else if (Data == "RF")
                M->Add(Functions::ReadFile);
            else if (Data == "SV")
                M->Add(Functions::SaveObject);
            else if (Data == "SA")
                M->Add(Functions::SaveAll);
            else if (Data == "RO")
                M->Add(Functions::ReadObject);
            else if (Data == "FR")
                M->Add(Functions::RunFile);
            else
                M->Add(Functions::None);
        }
        else if (Type == "ID")
            M->Add(IDClass(Data.toInt()));
        else if (Type == "NU")
            M->Add(Data.toFloat());
        else if (Type == "IN")
            M->Add((int32_t)Data.toInt());
        else if (Type == "TX")
            M->Add(Data);
        else if (Type == "TY")
            M->Add((Types)Data.toInt());
        else if (Type == "V2")
        {
            float *Array = Extract(Data, 2);
            M->Add(Vector2D(Array[0], Array[1]));
            delete Array;
        }
        else if (Type == "C2")
        {
            float *Array = Extract(Data, 3);
            M->Add(Coord2D(Array[0], Array[1], Array[2]));
            delete Array;
        }
        else if (Type == "CO")
        {
            float *Array = Extract(Data, 4);
            M->Add(ColourClass((byte)Array[0], (byte)Array[1], (byte)Array[2], (byte)Array[3]));
            delete Array;
        }
        else
            M->Add(Status::InvalidType);

        Index = End + 1;
    }
    return M;
};
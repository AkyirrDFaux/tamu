void PortClass::Setup()
{
    if (Attached == nullptr || Attached->Key() == nullptr)
    {
        StopDriver();
        return;
    }
    BaseClass *Key = Attached->Key();
    
    if (Key->Type == Types::Display && *Data == Ports::GPIO)
    {
        DisplayClass *Display = Key->As<DisplayClass>();
        if (!Display->Modules.IsValid(Display->Length))
        {
            ReportError(Status::MissingModule);
            return;
        }

        Driver = BeginLED(*Display->Modules.GetValue<int32_t>(Display->Length), Pin);
        DriverType = Drivers::LED;
    }
    else if (Key->Type == Types::LEDStrip && *Data == Ports::GPIO)
    {
        LEDStripClass *Strip = Key->As<LEDStripClass>();
        if (!Strip->Modules.IsValid(Strip->Length))
        {
            ReportError(Status::MissingModule);
            return;
        }

        Driver = BeginLED(*Strip->Modules.GetValue<int32_t>(Strip->Length), Pin);
        DriverType = Drivers::LED;
    }
    else if (Key->Type == Types::Fan && *Data == Ports::TOut)
    {
        FanClass *Fan = Key->As<FanClass>();

        analogWriteFrequency((uint32_t)25000);
        DriverType = Drivers::FanPWM;
    }
    else
        StopDriver();
}

void PortAttachClass::Setup() // Attach to port
{
    if (Port != nullptr)
        Port->Detach();

    if (!Board.Port.Modules.IsValid(*Data))
        return;

    Port = Board.Port.Modules[*Data]->As<PortClass>();
    if (Port == nullptr || !Port->Attach(this))
        ReportError(Status::PortError, "Failed to attach");
};

bool PortAttachClass::Check(Drivers RequiredDriver)
{
    if (Port == nullptr)
    {
        if (*Data < 0)
            return false;
        if (Board.Port.Modules.IsValid(*Data))
            Setup();
        if (Port == nullptr)
            return false;
    }
    if (Port->DriverType != RequiredDriver)
    {
        Port->Setup();
        if (Port->DriverType != RequiredDriver)
            return false;
    }
    return true;
};

int32_t IDList::FirstValid(Types Filter, int32_t Start)
{
    int32_t Index = Start;
    while (Index <= Length && (!IsValid(Index) || (Filter != Types::Undefined && ObjectAt(Index)->Type != Filter)))
        Index++;
    return Index;
};

void IDList::Iterate(int32_t *Index, Types Filter)
{
    (*Index)++;
    // Skip to next if: A)Is invalid; B)Filter is not undefined and Type of object isn't one filtered
    while (*Index <= Length && (!IsValid(*Index) || (Filter != Types::Undefined && ObjectAt(*Index)->Type != Filter)))
        (*Index)++;
};

bool IDList::Remove(BaseClass *RemovedObject) // Removes object, even if it is contained more times
{
    for (int32_t Index = 0; Index < Length; Index++)
    {
        if (ObjectAt(Index) == RemovedObject || IDs[Index] == RemovedObject->ID)
            Remove(Index);
    }
    return true;
};

bool IDList::Update(int32_t Index)
{
    if (IsValidID(Index) == false)
        return false;

    Object[Index] = Objects.Find(IDs[Index]);

    return Object[Index] != nullptr;
};

bool IDList::Add(BaseClass *AddObject, int32_t Index)
{
    if (Index == -1) // Add to end
        Index = Length;

    if (Index >= Length) // Expand if Index is too far
        Expand(Index + 1);
    else if (IsValidID(Index)) // Occupied
        return false;

    IDs[Index] = AddObject->ID; // Write
    Object[Index] = AddObject;
    return true;
};

template <class C>
C *IDList::Get(int32_t Index)
{
    if (!IsValid(Index))
        return nullptr;

    return ObjectAt(Index)->As<C>();
}

template <class C>
C *IDList::GetValue(int32_t Index)
{
    if (!IsValid(Index))
        return nullptr;

    return ObjectAt(Index)->ValueAs<C>();
};

void ReportError(Status ErrorCode, String Detail)
{
    if (Detail.length() > 0)
        Chirp.SendNow(Message(ErrorCode, Detail));
    else
        Chirp.SendNow(Message(ErrorCode));
}

void DefaultSetup()
{
    *Board.Data = Boards::Tamu_v2_0;
    Board.Setup();
    Board.BTName = "Unnamed - Default";

    DisplayClass *D = new DisplayClass();
    *D->Data = Displays::Vysi_v1_0;
    D->Setup();
    PortAttachClass *DP = D->Modules.Get<PortAttachClass>(D->DisplayPort);
    *DP->Data = 0;
    DP->Setup();
    Folder *DS = D->Modules.Get<Folder>(D->Parts);

    Shape2DClass *S = new Shape2DClass();
    DS->AddModule(S);
    Texture2D *T = S->Modules.Get<Texture2D>(S->Texture);
    *T->Data = Textures2D::Full;
    T->Setup();
    *T->Modules.GetValue<ColourClass>(T->ColourA) = ColourClass(255, 255, 255);
    Geometry2DClass *G = new Geometry2DClass();
    S->AddModule(G);
    *G->Data = Geometries::Fill;
    G->Setup();

    Shape2DClass *S1 = new Shape2DClass();
    DS->AddModule(S1);
    Texture2D *T1 = S1->Modules.Get<Texture2D>(S1->Texture);
    *T1->Data = Textures2D::BlendLinear;
    T1->Setup();
    *T1->Modules.GetValue<ColourClass>(T1->ColourA) = ColourClass(0, 255, 0);
    *T1->Modules.GetValue<ColourClass>(T1->ColourB) = ColourClass(255, 0, 0);
    *T1->Modules.GetValue<Coord2D>(T1->Position) = Coord2D(1, 1, 0);
    *T1->Modules.GetValue<float>(T1->Width) = 5.0F;
    Geometry2DClass *G1 = new Geometry2DClass();
    S1->AddModule(G1);
    *G1->Data = Geometries::Elipse;
    G1->Setup();
    *G1->Modules.GetValue<Vector2D>(G1->Size) = Vector2D(4, 4);
    *G1->Modules.GetValue<float>(G1->Fade) = 0.5F;
    *G1->Modules.GetValue<Coord2D>(G1->Position) = Coord2D(1, 1, 0);

    Shape2DClass *S2 = new Shape2DClass();
    DS->AddModule(S2);
    Texture2D *T2 = S2->Modules.Get<Texture2D>(S2->Texture);
    *T2->Data = Textures2D::Full;
    T2->Setup();
    *T2->Modules.GetValue<ColourClass>(T2->ColourA) = ColourClass(0, 0, 0);
    Geometry2DClass *G2 = new Geometry2DClass();
    S2->AddModule(G2);
    *G2->ValueAs<Geometries>() = Geometries::DoubleParabola;
    G2->Setup();
    *G2->Modules.GetValue<Vector2D>(G2->Size) = Vector2D(2, 4);
    *G2->Modules.GetValue<Coord2D>(G2->Position) = Coord2D(1, 1, 0);

    //Width animation
    AnimationFloat *A = new AnimationFloat();
    *A->ValueAs<FloatAnimations>() = FloatAnimations::MoveTo;
    A->Setup();
    A->AddModule(T1->Modules[T1->Width]);
    //T1->Modules.Get<BaseClass>(T1->Width)->AddModule(A);

    Program *P = new Program();
    *P->ValueAs<ProgramTypes>() = ProgramTypes::Sequence;
    P->Flags += Flags::RunLoop;

    // Add equal for value
    Operation *O1 = new Operation();
    *O1->ValueAs<Operations>() = Operations::Equal;
    Variable<float> *V1 = new Variable<float>(0);
    O1->AddModule(V1, 0);
    O1->AddModule(A->Modules[A->TargetA]);
    //A->Modules.Get<BaseClass>(A->TargetA)->AddModule(O1);
    P->AddModule(O1, 0);

    // Add delay for time
    Operation *O2 = new Operation();
    *O2->ValueAs<Operations>() = Operations::AddDelay;
    Variable<uint32_t> *V2 = new Variable<uint32_t>(2000);
    O2->AddModule(V2, 0);
    O2->AddModule(A->Modules[A->Time]);
    //A->Modules.Get<BaseClass>(A->Time)->AddModule(O2);
    P->AddModule(O2, 1);

    // Add animation
    P->AddModule(A, 2);

    // Add equal for value
    Operation *O3 = new Operation();
    *O3->ValueAs<Operations>() = Operations::Equal;
    Variable<float> *V3 = new Variable<float>(15);
    O3->AddModule(V3, 0);
    O3->AddModule(A->Modules[A->TargetA]);
    //A->Modules.Get<BaseClass>(A->TargetA)->AddModule(O3);
    P->AddModule(O3, 3);

    // Add delay for time again
    P->AddModule(O2, 4);

    // Animate again
    P->AddModule(A, 5);

    // EYE LID
    Shape2DClass *SL = new Shape2DClass();
    DS->AddModule(SL);
    Texture2D *TL = SL->Modules.Get<Texture2D>(SL->Texture);
    *TL->Data = Textures2D::Full;
    TL->Setup();
    *TL->Modules.GetValue<ColourClass>(TL->ColourA) = ColourClass(0, 0, 0);
    Geometry2DClass *GL = new Geometry2DClass();
    SL->AddModule(GL);
    *GL->Data = Geometries::HalfFill;
    GL->Setup();
    *GL->Modules.GetValue<Coord2D>(GL->Position) = Coord2D(0, 3, 0);

    // LID MOVEMENT
    AnimationCoord *A2 = new AnimationCoord();
    *A2->ValueAs<CoordAnimations>() = CoordAnimations::MoveTo;
    A2->Setup();
    A2->AddModule(GL->Modules[GL->Position]);
    //GL->Modules.Get<BaseClass>(GL->Position)->AddModule(A2);

    Program *P2 = new Program();
    *P2->ValueAs<ProgramTypes>() = ProgramTypes::Sequence;
    P2->Flags += Flags::RunLoop;

    // Add equal for value
    Operation *O21 = new Operation();
    *O21->ValueAs<Operations>() = Operations::Equal;
    Variable<Coord2D> *V21 = new Variable<Coord2D>(Coord2D(0, -5, 0));
    O21->AddModule(V21, 0);
    O21->AddModule(A2->Modules[A2->TargetA]);
    //A2->Modules.Get<BaseClass>(A2->TargetA)->AddModule(O21);
    P2->AddModule(O21, 0);

    // Add delay for time
    Operation *O22 = new Operation();
    *O22->ValueAs<Operations>() = Operations::AddDelay;
    Variable<uint32_t> *V22 = new Variable<uint32_t>(500);
    O22->AddModule(V22, 0);
    O22->AddModule(A2->Modules[A2->Time]);
    //A2->Modules.Get<BaseClass>(A2->Time)->AddModule(O22);
    P2->AddModule(O22, 1);

    // Add animation
    P2->AddModule(A2, 2);

    // Add equal for value
    Operation *O23 = new Operation();
    *O23->ValueAs<Operations>() = Operations::Equal;
    Variable<Coord2D> *V23 = new Variable<Coord2D>(Coord2D(0, 5, 0));
    O23->AddModule(V23, 0);
    O23->AddModule(A2->Modules[A2->TargetA]);
    //A2->Modules.Get<BaseClass>(A2->TargetA)->AddModule(O23);
    P2->AddModule(O23, 3);

    // Add delay for time again
    P2->AddModule(O22, 4);

    // Animate again
    P2->AddModule(A2, 5);

    // Delay 2
    Operation *O24 = new Operation();
    *O24->ValueAs<Operations>() = Operations::AddDelay;
    Variable<uint32_t> *V24 = new Variable<uint32_t>(5000);
    O24->AddModule(V24, 0);
    O24->AddModule(A2->Modules[A2->Time]);
    //A2->Modules.Get<BaseClass>(A2->Time)->AddModule(O24);
    P2->AddModule(O24, 6);

    // "Animate"
    P2->AddModule(A2, 7);

    // FAN
    FanClass *F = new FanClass();
    *F->Modules.Get<PortAttachClass>(F->PortAttach)->Data = 4;
    *F->ValueAs<uint8_t>() = 0;

    // LED STRIP
    LEDStripClass *L = new LEDStripClass();
    *L->Modules.Get<PortAttachClass>(L->PortAttach)->Data = 1;
    *L->Modules.GetValue<int32_t>(L->Length) = 60;

    LEDSegmentClass *LS = new LEDSegmentClass();
    L->Modules.Get<Folder>(L->Parts)->AddModule(LS);
    *LS->Modules.GetValue<int32_t>(LS->End) = 59;

    Texture1D *LT = LS->Modules.Get<Texture1D>(LS->Texture);
    *LT->ValueAs<Textures1D>() = Textures1D::Full;
    LT->Setup();
    *LT->Modules.GetValue<ColourClass>(LT->ColourA) = ColourClass(255, 0, 0);
};
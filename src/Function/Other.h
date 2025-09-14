void ReportError(Status ErrorCode, String Detail)
{
    if (Detail.length() > 0)
        Chirp.Send("E" + String((uint8_t)ErrorCode) + " : " + Detail);
    else
        Chirp.Send("E" + String((uint8_t)ErrorCode));
}

void DefaultSetup()
{
    *Board.Data = Boards::Tamu_v2_0;
    Board.Setup();
    Board.BTName = "Unnamed - Default";

    DisplayClass *D = new DisplayClass();
    *D->Data = Displays::Vysi_v1_0;
    D->Setup();
    D->Modules.Add(Board.Devices.Modules[0],D->Port);
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
    Variable<float> *A = T1->Modules.Get<Variable<float>>(T1->Width);
    Variable<Operations> *AO = new Variable<Operations>(Operations::MoveTo);
    Variable<float> *ATGT = new Variable<float>(0);
    Variable<uint32_t> *AT = new Variable<uint32_t>(0);
    A->AddModule(AO);
    A->AddModule(ATGT);
    A->AddModule(AT);
    
    Program *P = new Program();
    *P->ValueAs<ProgramTypes>() = ProgramTypes::Sequence;
    P->Flags += Flags::RunLoop;

    // Add equal for value
    Variable<float> *V1 = new Variable<float>(0);
    Variable<Operations> *O1 = new Variable<Operations>(Operations::Equal);
    V1->AddModule(O1, 0);
    V1->AddModule(ATGT);
    P->AddModule(V1, 0);

    // Add delay for time
    Variable<Operations> *O2 = new Variable<Operations>(Operations::AddDelay);
    Variable<uint32_t> *V2 = new Variable<uint32_t>(2000);
    AT->AddModule(O2, 0);
    AT->AddModule(V2);
    P->AddModule(AT, 1);

    // Add animation
    P->AddModule(A, 2);

    // Add equal for value
    Variable<float> *V3 = new Variable<float>(15);
    Variable<Operations> *O3 = new Variable<Operations>(Operations::Equal);
    V3->AddModule(O3, 0);
    V3->AddModule(ATGT);
    P->AddModule(V3, 3);

    // Add delay for time again
    P->AddModule(AT, 4);

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
    Variable<Coord2D> *A2 = GL->Modules.Get<Variable<Coord2D>>(GL->Position);
    Variable<Operations> *A2O = new Variable<Operations>(Operations::MoveTo);
    Variable<Coord2D> *A2TGT = new Variable<Coord2D>(Coord2D());
    Variable<uint32_t> *A2T = new Variable<uint32_t>(0);
    A2->AddModule(A2O);
    A2->AddModule(A2TGT);
    A2->AddModule(A2T);

    Program *P2 = new Program();
    *P2->ValueAs<ProgramTypes>() = ProgramTypes::Sequence;
    P2->Flags += Flags::RunLoop;

    // Add equal for value - down position
    Variable<Coord2D> *V21 = new Variable<Coord2D>(Coord2D(0, -5, 0));
    Variable<Operations> *O21 = new Variable<Operations>(Operations::Equal);
    V21->AddModule(O21, 0);
    V21->AddModule(A2TGT);
    P2->AddModule(V21);

    // Add delay for time
    Variable<uint32_t> *V22 = new Variable<uint32_t>(500); //Delay
    Variable<Operations> *O23 = new Variable<Operations>(Operations::AddDelay);
    A2T->AddModule(O23, 0);
    A2T->AddModule(V22);
    P2->AddModule(A2T);

    // Add animation - down
    P2->AddModule(A2);

    // Add equal for value - up position
    Variable<Coord2D> *V24 = new Variable<Coord2D>(Coord2D(0, 5, 0));
    Variable<Operations> *O24 = new Variable<Operations>(Operations::Equal);
    V24->AddModule(O24, 0);
    V24->AddModule(A2TGT);
    P2->AddModule(V24);

    // Add delay for time again
    P2->AddModule(A2T);

    // Animate again - up
    P2->AddModule(A2);

    //Delay
    Variable<uint32_t> *V25 = new Variable<uint32_t>(0);
    Variable<Operations> *O25 = new Variable<Operations>(Operations::Delay);
    Variable<uint32_t> *V26 = new Variable<uint32_t>(5000);
    V25->AddModule(O25,0);
    V25->AddModule(V26);
    P2->AddModule(V25);

    // FAN
    FanClass *F = new FanClass();
    F->Modules.Add(Board.Devices.Modules[4],F->Port);
    
    // LED STRIP
    LEDStripClass *L = new LEDStripClass();
    L->Modules.Add(Board.Devices.Modules[1],L->Port);
    //*L->Modules.Get<PortAttachClass>(L->PortAttach)->Data = 1;
    *L->Modules.GetValue<int32_t>(L->Length) = 60;

    LEDSegmentClass *LS = new LEDSegmentClass();
    L->Modules.Get<Folder>(L->Parts)->AddModule(LS);
    *LS->Modules.GetValue<int32_t>(LS->End) = 59;

    Texture1D *LT = LS->Modules.Get<Texture1D>(LS->Texture);
    *LT->ValueAs<Textures1D>() = Textures1D::Full;
    LT->Setup();
    *LT->Modules.GetValue<ColourClass>(LT->ColourA) = ColourClass(255, 0, 0);

    //SERVO
    ServoClass *Ser = new ServoClass();
    Ser->Modules.Add(Board.Devices.Modules[5],Ser->Port);
};
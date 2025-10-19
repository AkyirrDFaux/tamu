void ReportError(Status ErrorCode, String Detail)
{
    if (Detail.length() > 0)
        Chirp.Send("E" + String((uint8_t)ErrorCode) + " : " + Detail);
    else
        Chirp.Send("E" + String((uint8_t)ErrorCode));
}

void DefaultSetup()
{
    *Board.Values.At<Boards>(Board.BoardType) = Boards::Tamu_v2_0;
    Board.Setup();
    *Board.Values.At<String>(Board.BTName) = "Unnamed - Default";

    DisplayClass *D = new DisplayClass();
    *D->Values.At<Displays>(D->DisplayType) = Displays::Vysi_v1_0;
    D->Setup();
    D->AddModule(Board.Modules[0], D->Port);

    Shape2DClass *S = new Shape2DClass();
    D->AddModule(S);
    Texture2D *T = new Texture2D();
    S->AddModule(T, S->Texture);
    *T->Values.At<Textures2D>(T->Texture) = Textures2D::Full;
    T->Setup();
    *T->Values.At<ColourClass>(T->ColourA) = ColourClass(255, 255, 255);
    Geometry2DClass *G = new Geometry2DClass();
    S->AddModule(G);
    *G->Values.At<Geometries>(G->Geometry) = Geometries::Fill;
    G->Setup();

    Shape2DClass *S1 = new Shape2DClass();
    D->AddModule(S1);
    Texture2D *T1 = new Texture2D();
    S1->AddModule(T1, S1->Texture);
    *T1->Values.At<Textures2D>(T1->Texture) = Textures2D::BlendLinear;
    T1->Setup();
    *T1->Values.At<ColourClass>(T1->ColourA) = ColourClass(0, 255, 0);
    *T1->Values.At<ColourClass>(T1->ColourB) = ColourClass(255, 0, 0);
    *T1->Values.At<Coord2D>(T1->Position) = Coord2D(1, 1, 0);
    *T1->Values.At<float>(T1->Width) = 5.0F;
    Geometry2DClass *G1 = new Geometry2DClass();
    S1->AddModule(G1);
    *G1->Values.At<Geometries>(G1->Geometry) = Geometries::Elipse;
    G1->Setup();
    *G1->Values.At<Vector2D>(G1->Size) = Vector2D(4, 4);
    *G1->Values.At<float>(G1->Fade) = 0.5F;
    *G1->Values.At<Coord2D>(G1->Position) = Coord2D(1, 1, 0);

    Shape2DClass *S2 = new Shape2DClass();
    D->AddModule(S2);
    Texture2D *T2 = new Texture2D();
    S2->AddModule(T2, S2->Texture);
    *T2->Values.At<Textures2D>(T2->Texture) = Textures2D::Full;
    T2->Setup();
    *T2->Values.At<ColourClass>(T2->ColourA) = ColourClass(0, 0, 0);
    Geometry2DClass *G2 = new Geometry2DClass();
    S2->AddModule(G2);
    *G2->Values.At<Geometries>(G2->Geometry) = Geometries::DoubleParabola;
    G2->Setup();
    *G2->Values.At<Vector2D>(G2->Size) = Vector2D(2, 4);
    *G2->Values.At<Coord2D>(G2->Position) = Coord2D(1, 1, 0);

    // Width animation
    Program *P = new Program();
    *P->Values.At<ProgramTypes>(P->Mode) = ProgramTypes::Sequence;
    P->Flags += Flags::RunLoop;

    Operation *AO = new Operation();
    *AO->Values.At<Operations>(0) = Operations::MoveTo;
    AO->Values.Add<float>(0, 1);
    AO->Values.Add<uint32_t>(0, 2);
    AO->Modules.Add(IDClass(T1->ID.Main(), T1->Width + 1));

    // Add equal for value
    Operation *O1 = new Operation();
    *O1->Values.At<Operations>(0) = Operations::Equal;
    O1->Values.Add<float>(0, 1);
    O1->Modules.Add(IDClass(AO->ID.Main(), 1 + 1));
    P->AddModule(O1, 0);

    // Add delay for time
    Operation *O2 = new Operation();
    *O2->Values.At<Operations>(0) = Operations::AddDelay;
    O2->Values.Add<uint32_t>(2000, 1);
    O2->Modules.Add(IDClass(AO->ID.Main(), 2 + 1));
    P->AddModule(O2, 1);

    // Add animation
    P->AddModule(AO, 2);

    // Add equal for value
    Operation *O3 = new Operation();
    *O3->Values.At<Operations>(0) = Operations::Equal;
    O3->Values.Add<float>(15, 1);
    O3->Modules.Add(IDClass(AO->ID.Main(), 1 + 1));
    P->AddModule(O3, 3);

    // Add delay for time again
    P->AddModule(O2, 4);

    // Animate again
    P->AddModule(AO, 5);

    // EYE LID
    Shape2DClass *SL = new Shape2DClass();
    D->AddModule(SL);
    Texture2D *TL = new Texture2D();
    SL->AddModule(TL, SL->Texture);
    *TL->Values.At<Textures2D>(TL->Texture) = Textures2D::Full;
    TL->Setup();
    *TL->Values.At<ColourClass>(TL->ColourA) = ColourClass(0, 0, 0);
    Geometry2DClass *GL = new Geometry2DClass();
    SL->AddModule(GL);
    *GL->Values.At<Geometries>(GL->Geometry) = Geometries::HalfFill;
    GL->Setup();
    *GL->Values.At<Coord2D>(GL->Position) = Coord2D(0, 3, 0);

    // LID MOVEMENT
    Program *P2 = new Program();
    *P2->Values.At<ProgramTypes>(P2->Mode) = ProgramTypes::Sequence;
    P2->Flags += Flags::RunLoop;

    Operation *AL = new Operation();
    *AL->Values.At<Operations>(0) = Operations::MoveTo;
    AL->Values.Add(Coord2D(), 1);
    AL->Values.Add<uint32_t>(0, 2);
    AL->Modules.Add(IDClass(GL->ID.Main(), GL->Position + 1));

    // Add equal for value - down position
    Operation *OL1 = new Operation();
    *OL1->Values.At<Operations>(0) = Operations::Equal;
    OL1->Values.Add(Coord2D(0, -5, 0), 1);
    OL1->Modules.Add(IDClass(AL->ID.Main(), 1 + 1));
    P2->AddModule(OL1, 0);

    // Add delay for time
    Operation *OL2 = new Operation();
    *OL2->Values.At<Operations>(0) = Operations::AddDelay;
    OL2->Values.Add<uint32_t>(500, 1);
    OL2->Modules.Add(IDClass(AL->ID.Main(), 2 + 1));
    P2->AddModule(OL2, 1);

    // Add animation - down
    P2->AddModule(AL, 2);

    // Add equal for value - up position
    Operation *OL3 = new Operation();
    *OL3->Values.At<Operations>(0) = Operations::Equal;
    OL3->Values.Add(Coord2D(0, 5, 0), 1);
    OL3->Modules.Add(IDClass(AL->ID.Main(), 1 + 1));
    P2->AddModule(OL3, 3);

    // Add delay for time again
    P2->AddModule(OL2, 4);

    // Animate again - up
    P2->AddModule(AL, 5);

    // Delay
    Operation *OL4 = new Operation();
    *OL4->Values.At<Operations>(0) = Operations::Delay;
    OL4->Values.Add<uint32_t>(5000, 1);
    OL4->Values.Add<uint32_t>(0, 2);
    P2->AddModule(OL4, 6);

    // FAN
    FanClass *F = new FanClass();
    F->Modules.Add(Board.Modules[4], F->Port);

    // LED STRIP
    LEDStripClass *L = new LEDStripClass();
    L->Modules.Add(Board.Modules[1], L->Port);
    *L->Values.At<int32_t>(L->Length) = 60;

    LEDSegmentClass *LS = new LEDSegmentClass();
    L->AddModule(LS);
    *LS->Values.At<int32_t>(LS->End) = 59;

    Texture1D *LT = LS->Modules.Get<Texture1D>(LS->Texture);
    *LT->Values.At<Textures1D>(LT->TextureType) = Textures1D::Full;
    LT->Setup();
    *LT->Values.At<ColourClass>(LT->ColourA) = ColourClass(255, 0, 0);

    // SERVO
    ServoClass *Ser = new ServoClass();
    Ser->Modules.Add(Board.Modules[5], Ser->Port);

    // EYE REACTION
    Program *P3 = new Program();
    *P3->Values.At<ProgramTypes>(P2->Mode) = ProgramTypes::Sequence;
    P3->Flags += Flags::RunLoop;

    //Get XY from Rot. vel.
    Operation *OR1 = new Operation();
    *OR1->Values.At<Operations>(0) = Operations::Extract;
    OR1->Values.Add(IDClass(Board.Modules[11]->ID.Main(), 1+1), 1); //GyrAcc
    OR1->Values.Add<uint8_t>(0, 2); //X
    OR1->Values.Add<uint8_t>(1, 3); //Y
    
    P3->AddModule(OR1, 0);

    //Add offset
    Operation *OR2 = new Operation();
    *OR2->Values.At<Operations>(0) = Operations::Add;

    OR2->Values.Add(Vector2D(0, 0), 1); //Variable
    OR2->Values.Add(Vector2D(1, 1), 2);

    P3->AddModule(OR2, 1);

    //Write into elements
    Operation *OR3 = new Operation();
    *OR3->Values.At<Operations>(0) = Operations::Combine;
    OR3->Values.Add(Vector2D(), 1);
    OR3->Values.Add<float>(0, 2);
    
    P3->AddModule(OR3, 2);

    //Outputs
    OR1->Modules.Add(IDClass(OR2->ID.Main(), 1 + 1));
    OR2->Modules.Add(IDClass(OR3->ID.Main(), 1 + 1));
    OR3->Modules.Add(IDClass(T1->ID.Main(), T1->Position + 1));
    OR3->Modules.Add(IDClass(G1->ID.Main(), G1->Position + 1));
    OR3->Modules.Add(IDClass(G2->ID.Main(), G2->Position + 1));
};
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

    // Display L
    DisplayClass *LD = new DisplayClass();
    *LD->Values.At<Displays>(LD->DisplayType) = Displays::Vysi_v1_0;
    LD->Setup();
    *LD->Values.At<Coord2D>(LD->Offset) = Coord2D(0, 0, 5);
    LD->AddModule(Board.Modules[0], LD->Port);

    Shape2DClass *LS = new Shape2DClass();
    LD->AddModule(LS);
    Texture2D *LT = new Texture2D();
    LS->AddModule(LT, LS->Texture);
    *LT->Values.At<Textures2D>(LT->Texture) = Textures2D::Full;
    LT->Setup();
    *LT->Values.At<ColourClass>(LT->ColourA) = ColourClass(255, 255, 255);
    Geometry2DClass *LG = new Geometry2DClass();
    LS->AddModule(LG);
    *LG->Values.At<Geometries>(LG->Geometry) = Geometries::Fill;
    LG->Setup();

    Shape2DClass *LS1 = new Shape2DClass();
    LD->AddModule(LS1);
    Texture2D *LT1 = new Texture2D();
    LS1->AddModule(LT1, LS1->Texture);
    *LT1->Values.At<Textures2D>(LT1->Texture) = Textures2D::BlendLinear;
    LT1->Setup();
    *LT1->Values.At<ColourClass>(LT1->ColourA) = ColourClass(0, 100, 0);
    *LT1->Values.At<ColourClass>(LT1->ColourB) = ColourClass(0, 150, 0);
    *LT1->Values.At<Coord2D>(LT1->Position) = Coord2D(1.5, 1, 0);
    *LT1->Values.At<float>(LT1->Width) = 1.0F;
    Geometry2DClass *LG1 = new Geometry2DClass();
    LS1->AddModule(LG1);
    *LG1->Values.At<Geometries>(LG1->Geometry) = Geometries::Elipse;
    LG1->Setup();
    *LG1->Values.At<Vector2D>(LG1->Size) = Vector2D(4.5, 4.5);
    *LG1->Values.At<float>(LG1->Fade) = 0.1F;
    *LG1->Values.At<Coord2D>(LG1->Position) = Coord2D(1.5, 1, 0);

    Shape2DClass *LS2 = new Shape2DClass();
    LD->AddModule(LS2);
    Texture2D *LT2 = new Texture2D();
    LS2->AddModule(LT2, LS2->Texture);
    *LT2->Values.At<Textures2D>(LT2->Texture) = Textures2D::Full;
    LT2->Setup();
    *LT2->Values.At<ColourClass>(LT2->ColourA) = ColourClass(0, 0, 0);
    Geometry2DClass *LG2 = new Geometry2DClass();
    LS2->AddModule(LG2);
    *LG2->Values.At<Geometries>(LG2->Geometry) = Geometries::DoubleParabola;
    LG2->Setup();
    *LG2->Values.At<float>(LG2->Fade) = 1.5F;
    *LG2->Values.At<Vector2D>(LG2->Size) = Vector2D(2.3, 4.9);
    *LG2->Values.At<Coord2D>(LG2->Position) = Coord2D(1.5, 1, 0);

    Shape2DClass *LS3 = new Shape2DClass();
    LD->AddModule(LS3);
    Texture2D *LT3 = new Texture2D();
    LS3->AddModule(LT3, LS3->Texture);
    *LT3->Values.At<Textures2D>(LT3->Texture) = Textures2D::Full;
    LT3->Setup();
    *LT3->Values.At<ColourClass>(LT3->ColourA) = ColourClass(0, 0, 0);
    Geometry2DClass *LG3 = new Geometry2DClass();
    LS3->AddModule(LG3);
    *LG3->Values.At<Geometries>(LG3->Geometry) = Geometries::HalfFill;
    LG3->Setup();
    *LG3->Values.At<Coord2D>(LG3->Position) = Coord2D(0, 3, 0);

    // Display R
    DisplayClass *RD = new DisplayClass();
    *RD->Values.At<Displays>(RD->DisplayType) = Displays::Vysi_v1_0;
    *RD->Values.At<Coord2D>(RD->Offset) = Coord2D(0, 0, 175);
    RD->Setup();
    RD->AddModule(Board.Modules[1], RD->Port);

    Shape2DClass *RS = new Shape2DClass();
    RD->AddModule(RS);
    Texture2D *RT = new Texture2D();
    RS->AddModule(RT, RS->Texture);
    *RT->Values.At<Textures2D>(RT->Texture) = Textures2D::Full;
    RT->Setup();
    *RT->Values.At<ColourClass>(RT->ColourA) = ColourClass(255, 255, 255);
    Geometry2DClass *RG = new Geometry2DClass();
    RS->AddModule(RG);
    *RG->Values.At<Geometries>(RG->Geometry) = Geometries::Fill;
    RG->Setup();

    Shape2DClass *RS1 = new Shape2DClass();
    RD->AddModule(RS1);
    Texture2D *RT1 = new Texture2D();
    RS1->AddModule(RT1, RS1->Texture);
    *RT1->Values.At<Textures2D>(RT1->Texture) = Textures2D::BlendLinear;
    RT1->Setup();
    *RT1->Values.At<ColourClass>(RT1->ColourA) = ColourClass(0, 100, 0);
    *RT1->Values.At<ColourClass>(RT1->ColourB) = ColourClass(0, 150, 0);
    *RT1->Values.At<Coord2D>(RT1->Position) = Coord2D(-1.5, 1, 0);
    *RT1->Values.At<float>(RT1->Width) = 1.0F;
    Geometry2DClass *RG1 = new Geometry2DClass();
    RS1->AddModule(RG1);
    *RG1->Values.At<Geometries>(RG1->Geometry) = Geometries::Elipse;
    RG1->Setup();
    *RG1->Values.At<Vector2D>(RG1->Size) = Vector2D(4.5, 4.5);
    ;
    *RG1->Values.At<float>(RG1->Fade) = 0.1F;
    *RG1->Values.At<Coord2D>(RG1->Position) = Coord2D(-1.5, 1, 0);

    Shape2DClass *RS2 = new Shape2DClass();
    RD->AddModule(RS2);
    Texture2D *RT2 = new Texture2D();
    RS2->AddModule(RT2, RS2->Texture);
    *RT2->Values.At<Textures2D>(RT2->Texture) = Textures2D::Full;
    RT2->Setup();
    *RT2->Values.At<ColourClass>(RT2->ColourA) = ColourClass(0, 0, 0);
    Geometry2DClass *RG2 = new Geometry2DClass();
    RS2->AddModule(RG2);
    *RG2->Values.At<Geometries>(RG2->Geometry) = Geometries::DoubleParabola;
    RG2->Setup();
    *RG2->Values.At<float>(RG2->Fade) = 1.5F;
    *RG2->Values.At<Vector2D>(RG2->Size) = Vector2D(2.3, 4.9);
    *RG2->Values.At<Coord2D>(RG2->Position) = Coord2D(-1.5, 1, 0);

    Shape2DClass *RS3 = new Shape2DClass();
    RD->AddModule(RS3);
    Texture2D *RT3 = new Texture2D();
    RS3->AddModule(RT3, RS3->Texture);
    *RT3->Values.At<Textures2D>(RT3->Texture) = Textures2D::Full;
    RT3->Setup();
    *RT3->Values.At<ColourClass>(RT3->ColourA) = ColourClass(0, 0, 0);
    Geometry2DClass *RG3 = new Geometry2DClass();
    RS3->AddModule(RG3);
    *RG3->Values.At<Geometries>(RG3->Geometry) = Geometries::HalfFill;
    RG3->Setup();
    *RG3->Values.At<Coord2D>(RG3->Position) = Coord2D(0, 3, 0);

    // LID MOVEMENT
    Program *P2 = new Program();
    *P2->Values.At<ProgramTypes>(P2->Mode) = ProgramTypes::Sequence;
    P2->Flags += Flags::RunLoop;
    P2->Name = "Blinking";

    Operation *AL = new Operation();
    *AL->Values.At<Operations>(0) = Operations::MoveTo;
    AL->Values.Add(Coord2D(), 1);
    AL->Values.Add<uint32_t>(0, 2);
    AL->Modules.Add(IDClass(LG3->ID.Base(), LG3->Position + 1));
    AL->Modules.Add(IDClass(RG3->ID.Base(), RG3->Position + 1));

    // Add equal for value - down position
    Operation *OL1 = new Operation();
    *OL1->Values.At<Operations>(0) = Operations::Equal;
    OL1->Values.Add(Coord2D(0, -5, 0), 1);
    OL1->Modules.Add(IDClass(AL->ID.Base(), 1 + 1));
    P2->AddModule(OL1, 0);

    // Add delay for time
    Operation *OL2 = new Operation();
    *OL2->Values.At<Operations>(0) = Operations::AddDelay;
    OL2->Values.Add<uint32_t>(250, 1);
    OL2->Modules.Add(IDClass(AL->ID.Base(), 2 + 1));
    P2->AddModule(OL2, 1);

    // Add animation - down
    P2->AddModule(AL, 2);

    // Add equal for value - up position
    Operation *OL3 = new Operation();
    *OL3->Values.At<Operations>(0) = Operations::Equal;
    OL3->Values.Add(Coord2D(0, 5, 0), 1);
    OL3->Modules.Add(IDClass(AL->ID.Base(), 1 + 1));
    P2->AddModule(OL3, 3);

    // Add delay for time again
    P2->AddModule(OL2, 4);

    // Animate again - up
    P2->AddModule(AL, 5);

    // Delay
    Operation *OL4 = new Operation();
    *OL4->Values.At<Operations>(0) = Operations::Delay;
    OL4->Values.Add<uint32_t>(10000, 1);
    OL4->Values.Add<uint32_t>(0, 2);
    P2->AddModule(OL4, 6);

    // FAN
    FanClass *F = new FanClass();
    F->Modules.Add(Board.Modules[3], F->Port);

    // EYE REACTION
    Program *P3 = new Program();
    *P3->Values.At<ProgramTypes>(P2->Mode) = ProgramTypes::Sequence;
    P3->Flags += Flags::RunLoop;
    P3->Name = "Eye movement";

    // Get XY from Rot. vel.
    Operation *OR1 = new Operation();
    *OR1->Values.At<Operations>(0) = Operations::Extract;
    OR1->Values.Add(IDClass(Board.Modules[11]->ID.Base(), 1 + 1), 1); // GyrAcc
    OR1->Values.Add<uint8_t>(1, 2);                                   // Y
    OR1->Values.Add<uint8_t>(0, 3);                                   // X

    P3->AddModule(OR1, 0);

    //Gain
    Operation *OR2 = new Operation();
    *OR2->Values.At<Operations>(0) = Operations::Multiply;
    OR2->Values.Add(Vector2D(0, 0), 1); // Variable
    OR2->Values.Add(Vector2D(-1, 1), 2);

    P3->AddModule(OR2, 1);

    // Add offset
    Operation *ORL3 = new Operation();
    *ORL3->Values.At<Operations>(0) = Operations::Add;

    ORL3->Values.Add(Vector2D(0, 0), 1); // Variable
    ORL3->Values.Add(Vector2D(1.5, 1), 2);

    P3->AddModule(ORL3, 2);

    Operation *ORR3 = new Operation();
    *ORR3->Values.At<Operations>(0) = Operations::Add;

    ORR3->Values.Add(Vector2D(0, 0), 1); // Variable
    ORR3->Values.Add(Vector2D(-1.5, 1), 2);

    P3->AddModule(ORR3, 3);

    // Write into elements
    Operation *ORL4 = new Operation();
    *ORL4->Values.At<Operations>(0) = Operations::Combine;
    ORL4->Values.Add(Vector2D(), 1);
    ORL4->Values.Add<float>(0, 2);

    P3->AddModule(ORL4, 4);

    Operation *ORR4 = new Operation();
    *ORR4->Values.At<Operations>(0) = Operations::Combine;
    ORR4->Values.Add(Vector2D(), 1);
    ORR4->Values.Add<float>(0, 2);

    P3->AddModule(ORR4, 5);

    // Outputs
    OR1->Modules.Add(IDClass(OR2->ID.Base(), 1 + 1));

    OR2->Modules.Add(IDClass(ORL3->ID.Base(), 1 + 1));
    OR2->Modules.Add(IDClass(ORR3->ID.Base(), 1 + 1));

    ORL3->Modules.Add(IDClass(ORL4->ID.Base(), 1 + 1));
    ORR3->Modules.Add(IDClass(ORR4->ID.Base(), 1 + 1));

    ORL4->Modules.Add(IDClass(LT1->ID.Base(), LT1->Position + 1));
    ORL4->Modules.Add(IDClass(LG1->ID.Base(), LG1->Position + 1));
    ORL4->Modules.Add(IDClass(LG2->ID.Base(), LG2->Position + 1));

    ORR4->Modules.Add(IDClass(RT1->ID.Base(), RT1->Position + 1));
    ORR4->Modules.Add(IDClass(RG1->ID.Base(), RG1->Position + 1));
    ORR4->Modules.Add(IDClass(RG2->ID.Base(), RG2->Position + 1));
};

/*
    // Width animation
    Program *P = new Program();
    *P->Values.At<ProgramTypes>(P->Mode) = ProgramTypes::Sequence;
    P->Flags += Flags::RunLoop;
    P->Name = "Width colour animation";

    Operation *AO = new Operation();
    *AO->Values.At<Operations>(0) = Operations::MoveTo;
    AO->Values.Add<float>(0, 1);
    AO->Values.Add<uint32_t>(0, 2);
    AO->Modules.Add(IDClass(T1->ID.Base(), T1->Width + 1));

    // Add equal for value
    Operation *O1 = new Operation();
    *O1->Values.At<Operations>(0) = Operations::Equal;
    O1->Values.Add<float>(0, 1);
    O1->Modules.Add(IDClass(AO->ID.Base(), 1 + 1));
    P->AddModule(O1, 0);

    // Add delay for time
    Operation *O2 = new Operation();
    *O2->Values.At<Operations>(0) = Operations::AddDelay;
    O2->Values.Add<uint32_t>(2000, 1);
    O2->Modules.Add(IDClass(AO->ID.Base(), 2 + 1));
    P->AddModule(O2, 1);

    // Add animation
    P->AddModule(AO, 2);

    // Add equal for value
    Operation *O3 = new Operation();
    *O3->Values.At<Operations>(0) = Operations::Equal;
    O3->Values.Add<float>(15, 1);
    O3->Modules.Add(IDClass(AO->ID.Base(), 1 + 1));
    P->AddModule(O3, 3);

    // Add delay for time again
    P->AddModule(O2, 4);

    // Animate again
    P->AddModule(AO, 5);



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
    */
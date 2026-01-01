void ReportError(Status ErrorCode, String Detail)
{
    if (Detail.length() > 0)
        Chirp.Send("E" + String((uint8_t)ErrorCode) + " : " + Detail);
    else
        Chirp.Send("E" + String((uint8_t)ErrorCode));
}

void DefaultSetup()
{
    Board.ValueSet<Boards>(Boards::Tamu_v2_0, Board.BoardType);
    Board.ValueSet<String>("Akyirr", Board.BTName);

    // Display L
    DisplayClass *LD = new DisplayClass();
    LD->Name = "Left Eye";
    LD->ValueSet<Displays>(Displays::Vysi_v1_0, LD->DisplayType);
    LD->ValueSet<Coord2D>(Coord2D(0, 0, 5), LD->Offset);
    LD->AddModule(Board.Modules[0], LD->Port);
    LD->ValueSet<uint8_t>(40, LD->Brightness);

    Shape2DClass *LS = new Shape2DClass();
    LS->Name = "Left Background";
    LD->AddModule(LS);
    Texture2D *LT = new Texture2D();
    LT->Name = "Left Background";
    LS->AddModule(LT, LS->Texture);
    LT->ValueSet<Textures2D>(Textures2D::Full, LT->Texture);
    LT->ValueSet<ColourClass>(ColourClass(255, 255, 255), LT->ColourA);
    Geometry2DClass *LG = new Geometry2DClass();
    LG->Name = "Left Background";
    LS->AddModule(LG);
    LG->ValueSet<Geometries>(Geometries::Fill, LG->Geometry);

    Shape2DClass *LS1 = new Shape2DClass();
    LS1->Name = "Left EyeColour";
    LD->AddModule(LS1);
    Texture2D *LT1 = new Texture2D();
    LT1->Name = "Left EyeColour";
    LS1->AddModule(LT1, LS1->Texture);
    LT1->ValueSet<Textures2D>(Textures2D::BlendLinear, LT1->Texture);
    LT1->ValueSet<ColourClass>(ColourClass(0, 100, 0), LT1->ColourA);
    LT1->ValueSet<ColourClass>(ColourClass(0, 150, 0), LT1->ColourB);
    LT1->ValueSet<Coord2D>(Coord2D(1.5, 1, 0), LT1->Position);
    LT1->ValueSet<Number>(1.0, LT1->Width);
    Geometry2DClass *LG1 = new Geometry2DClass();
    LG1->Name = "Left EyeColour";
    LS1->AddModule(LG1);
    LG1->ValueSet<Geometries>(Geometries::Elipse, LG1->Geometry);
    LG1->ValueSet<Vector2D>(Vector2D(4.5, 4.5), LG1->Size);
    LG1->ValueSet<Number>(0.1, LG1->Fade);
    LG1->ValueSet<Coord2D>(Coord2D(1.5, 1, 0), LG1->Position);

    Shape2DClass *LS2 = new Shape2DClass();
    LS2->Name = "Left Pupil";
    LD->AddModule(LS2);
    Texture2D *LT2 = new Texture2D();
    LT2->Name = "Left Pupil";
    LS2->AddModule(LT2, LS2->Texture);
    LT2->ValueSet<Textures2D>(Textures2D::Full, LT2->Texture);
    LT2->ValueSet<ColourClass>(ColourClass(0, 0, 0), LT2->ColourA);
    Geometry2DClass *LG2 = new Geometry2DClass();
    LG2->Name = "Left Pupil";
    LS2->AddModule(LG2);
    LG2->ValueSet<Geometries>(Geometries::DoubleParabola, LG2->Geometry);
    LG2->ValueSet<Vector2D>(Vector2D(2.3, 4.9), LG2->Size);
    LG2->ValueSet<Coord2D>(Coord2D(1.5, 1, 0), LG2->Position);
    LG2->ValueSet<Number>(1.5, LG2->Fade);

    Shape2DClass *LS2A = new Shape2DClass();
    LS2A->Name = "Left Pupil Arrow";
    LS2A->Flags = Flags::Inactive;
    LD->AddModule(LS2A);
    LS2A->AddModule(LT2, LS2A->Texture);
    Geometry2DClass *LG2A = new Geometry2DClass();
    LG2A->Name = "Left Pupil Arrow";
    LS2A->AddModule(LG2A);
    LG2A->ValueSet<Geometries>(Geometries::Triangle, LG2A->Geometry);
    LG2A->ValueSet<Number>(1.5, LG2A->Fade);
    LG2A->ValueSet<Vector2D>(Vector2D(8, 8), LG2A->Size);
    LG2A->ValueSet<Coord2D>(Coord2D(1.5, -1.5, 0), LG2A->Position);
    
    Geometry2DClass *LG2AC = new Geometry2DClass();
    LG2AC->Name = "Left Pupil Arrow";
    LS2A->AddModule(LG2AC);
    LG2AC->ValueSet<Geometries>(Geometries::Triangle, LG2AC->Geometry);
    LG2AC->ValueSet<GeometryOperation>(GeometryOperation::Cut, LG2AC->Operation);
    LG2AC->ValueSet<Number>(1.5F, LG2AC->Fade);
    LG2AC->ValueSet<Vector2D>(Vector2D(3, 3), LG2AC->Size);
    LG2AC->ValueSet<Coord2D>(Coord2D(1.5, -1.5, 0), LG2AC->Position);

    Shape2DClass *LS3 = new Shape2DClass();
    LS3->Name = "Left Lid";
    LD->AddModule(LS3);
    Texture2D *LT3 = new Texture2D();
    LT3->Name = "Left Lid";
    LS3->AddModule(LT3, LS3->Texture);
    LT3->ValueSet<Textures2D>(Textures2D::Full, LT3->Texture);
    LT3->ValueSet<ColourClass>(ColourClass(0, 0, 0), LT3->ColourA);
    Geometry2DClass *LG3 = new Geometry2DClass();
    LG3->Name = "Left Lid";
    LS3->AddModule(LG3);
    LG3->ValueSet<Geometries>(Geometries::HalfFill, LG3->Geometry);
    LG3->ValueSet<Coord2D>(Coord2D(0, 3, 0), LG3->Position);

    // Display R
    DisplayClass *RD = new DisplayClass();
    RD->Name = "Right Eye";
    RD->ValueSet<Displays>(Displays::Vysi_v1_0, RD->DisplayType);
    RD->ValueSet<Coord2D>(Coord2D(0, 0, 175), RD->Offset);
    RD->AddModule(Board.Modules[7], RD->Port);
    RD->ValueSet<uint8_t>(40, RD->Brightness);

    Shape2DClass *RS = new Shape2DClass();
    RS->Name = "Right Background";
    RD->AddModule(RS);
    Texture2D *RT = new Texture2D();
    RT->Name = "Right Background";
    RS->AddModule(RT, RS->Texture);
    RT->ValueSet<Textures2D>(Textures2D::Full, RT->Texture);
    RT->ValueSet<ColourClass>(ColourClass(255, 255, 255), RT->ColourA);
    Geometry2DClass *RG = new Geometry2DClass();
    RG->Name = "Right Background";
    RS->AddModule(RG);
    RG->ValueSet<Geometries>(Geometries::Fill, RG->Geometry);

    Shape2DClass *RS1 = new Shape2DClass();
    RS1->Name = "Right EyeColour";
    RD->AddModule(RS1);
    Texture2D *RT1 = new Texture2D();
    RT1->Name = "Right EyeColour";
    RS1->AddModule(RT1, RS1->Texture);
    RT1->ValueSet<Textures2D>(Textures2D::BlendLinear, RT1->Texture);
    RT1->ValueSet<ColourClass>(ColourClass(0, 100, 0), RT1->ColourA);
    RT1->ValueSet<ColourClass>(ColourClass(0, 150, 0), RT1->ColourB);
    RT1->ValueSet<Coord2D>(Coord2D(-1.5, 1, 0), RT1->Position);
    RT1->ValueSet<Number>(1.0F, RT1->Width);
    Geometry2DClass *RG1 = new Geometry2DClass();
    RG1->Name = "Right EyeColour";
    RS1->AddModule(RG1);
    RG1->ValueSet<Geometries>(Geometries::Elipse, RG1->Geometry);
    RG1->ValueSet<Vector2D>(Vector2D(4.5, 4.5), RG1->Size);
    RG1->ValueSet<Number>(0.1F, RG1->Fade);
    RG1->ValueSet<Coord2D>(Coord2D(-1.5, 1, 0), RG1->Position);

    Shape2DClass *RS2 = new Shape2DClass();
    RS2->Name = "Right Pupil";
    RD->AddModule(RS2);
    Texture2D *RT2 = new Texture2D();
    RT2->Name = "Right Pupil";
    RS2->AddModule(RT2, RS2->Texture);
    RT2->ValueSet<Textures2D>(Textures2D::Full, RT2->Texture);
    RT2->ValueSet<ColourClass>(ColourClass(0, 0, 0), RT2->ColourA);
    Geometry2DClass *RG2 = new Geometry2DClass();
    RG2->Name = "Right Pupil";
    RS2->AddModule(RG2);
    RG2->ValueSet<Geometries>(Geometries::DoubleParabola, RG2->Geometry);
    RG2->ValueSet<Number>(1.5F, RG2->Fade);
    RG2->ValueSet<Vector2D>(Vector2D(2.3, 4.9), RG2->Size);
    RG2->ValueSet<Coord2D>(Coord2D(-1.5, 1, 0), RG2->Position);

    Shape2DClass *RS2A = new Shape2DClass();
    RS2A->Name = "Right Pupil Arrow";
    RS2A->Flags = Flags::Inactive;
    RD->AddModule(RS2A);
    RS2A->AddModule(RT2, RS2A->Texture);
    Geometry2DClass *RG2A = new Geometry2DClass();
    RG2A->Name = "Right Pupil Arrow";
    RS2A->AddModule(RG2A);
    RG2A->ValueSet<Geometries>(Geometries::Triangle, RG2A->Geometry);
    RG2A->ValueSet<Number>(1.5F, RG2A->Fade);
    RG2A->ValueSet<Vector2D>(Vector2D(8, 8), RG2A->Size);
    RG2A->ValueSet<Coord2D>(Coord2D(-1.5, -1.5, 0), RG2A->Position);
    
    Geometry2DClass *RG2AC = new Geometry2DClass();
    RG2AC->Name = "Right Pupil Arrow";
    RS2A->AddModule(RG2AC);
    RG2AC->ValueSet<Geometries>(Geometries::Triangle, RG2AC->Geometry);
    RG2AC->ValueSet<GeometryOperation>(GeometryOperation::Cut, RG2AC->Operation);
    RG2AC->ValueSet<Number>(1.5F, RG2AC->Fade);
    RG2AC->ValueSet<Vector2D>(Vector2D(3, 3), RG2AC->Size);
    RG2AC->ValueSet<Coord2D>(Coord2D(-1.5, -1.5, 0), RG2AC->Position);

    Shape2DClass *RS3 = new Shape2DClass();
    RS3->Name = "Right Lid";
    RD->AddModule(RS3);
    Texture2D *RT3 = new Texture2D();
    RT3->Name = "Right Lid";
    RS3->AddModule(RT3, RS3->Texture);
    RT3->ValueSet<Textures2D>(Textures2D::Full, RT3->Texture);
    RT3->ValueSet<ColourClass>(ColourClass(0, 0, 0), RT3->ColourA);
    Geometry2DClass *RG3 = new Geometry2DClass();
    RG3->Name = "Right Lid";
    RS3->AddModule(RG3);
    RG3->ValueSet<Geometries>(Geometries::HalfFill, RG3->Geometry);
    RG3->ValueSet<Coord2D>(Coord2D(0, 3, 0), RG3->Position);

    // FAN
    FanClass *F = new FanClass();
    F->Name = "Fan";
    F->Modules.Add(Board.Modules[4], F->Port);

    // BLINKING PROGRAM
    Program *P2 = new Program();
    P2->ValueSet<ProgramTypes>(ProgramTypes::Sequence, P2->Mode);
    P2->Flags += Flags::RunLoop | Flags::Favourite;
    P2->Name = "Blinking";

    Operation *AL = new Operation();
    AL->Name = "Blink animation";
    AL->ValueSet<Operations>(Operations::MoveTo, 0);
    AL->Values.Add(Coord2D(), 1);
    AL->Values.Add<uint32_t>(0, 2);
    AL->Modules.Add(IDClass(LG3->ID.Base(), LG3->Position + 1));
    AL->Modules.Add(IDClass(RG3->ID.Base(), RG3->Position + 1));

    Operation *OL1 = new Operation();
    OL1->Name = "Blink down";
    OL1->ValueSet<Operations>(Operations::Equal, 0);
    OL1->Values.Add(Coord2D(0, -5, 0), 1);
    OL1->Modules.Add(IDClass(AL->ID.Base(), 1 + 1));
    P2->AddModule(OL1, 0);

    Operation *OL2 = new Operation();
    OL2->Name = "Blink timing";
    OL2->ValueSet<Operations>(Operations::AddDelay, 0);
    OL2->Values.Add<uint32_t>(250, 1);
    OL2->Modules.Add(IDClass(AL->ID.Base(), 2 + 1));
    P2->AddModule(OL2, 1);

    P2->AddModule(AL, 2);

    Operation *OL3 = new Operation();
    OL3->Name = "Blink up";
    OL3->ValueSet<Operations>(Operations::Equal, 0);
    OL3->Values.Add(Coord2D(0, 5, 0), 1);
    OL3->Modules.Add(IDClass(AL->ID.Base(), 1 + 1));
    P2->AddModule(OL3, 3);

    P2->AddModule(OL2, 4);
    P2->AddModule(AL, 5);

    Operation *OL4 = new Operation();
    OL4->Name = "Blink delay";
    OL4->ValueSet<Operations>(Operations::Delay, 0);
    OL4->Values.Add<uint32_t>(10000, 1);
    OL4->Values.Add<uint32_t>(0, 2);
    P2->AddModule(OL4, 6);

    // EYE MOVEMENT PROGRAM
    Program *P3 = new Program();
    P3->ValueSet<ProgramTypes>(ProgramTypes::Sequence, P3->Mode);
    P3->Flags += Flags::RunLoop | Flags::Favourite;
    P3->Name = "Eye movement";

    Operation *OR1 = new Operation();
    OR1->Name = "Gyroscope extract";
    OR1->ValueSet<Operations>(Operations::Extract, 0);
    OR1->Values.Add(IDClass(Board.Modules[11]->ID.Base(), 1 + 1), 1); 
    OR1->Values.Add<uint8_t>(0, 2);                                   
    OR1->Values.Add<uint8_t>(1, 3);                                   
    P3->AddModule(OR1, 0);

    Operation *OR2 = new Operation();
    OR2->Name = "Gyroscope gain";
    OR2->ValueSet<Operations>(Operations::Multiply, 0);
    OR2->Values.Add(Vector2D(0, 0), 1); 
    OR2->Values.Add(Vector2D(1, -0.5), 2);
    P3->AddModule(OR2, 1);

    Operation *ORL3 = new Operation();
    ORL3->Name = "Eye offset left";
    ORL3->ValueSet<Operations>(Operations::Add, 0);
    ORL3->Values.Add(Vector2D(0, 0), 1); 
    ORL3->Values.Add(Vector2D(1.5, 1), 2);
    P3->AddModule(ORL3, 2);

    Operation *ORR3 = new Operation();
    ORR3->Name = "Eye offset right";
    ORR3->ValueSet<Operations>(Operations::Add, 0);
    ORR3->Values.Add(Vector2D(0, 0), 1); 
    ORR3->Values.Add(Vector2D(-1.5, 1), 2);
    P3->AddModule(ORR3, 3);

    Operation *ORL4 = new Operation();
    ORL4->Name = "Eye combine left";
    ORL4->ValueSet<Operations>(Operations::Combine, 0);
    ORL4->Values.Add(Vector2D(), 1);
    ORL4->Values.Add<Number>(0, 2);
    P3->AddModule(ORL4, 4);

    Operation *ORR4 = new Operation();
    ORR4->Name = "Eye combine right";
    ORR4->ValueSet<Operations>(Operations::Combine, 0);
    ORR4->Values.Add(Vector2D(), 1);
    ORR4->Values.Add<Number>(0, 2);
    P3->AddModule(ORR4, 5);

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

    Operation *ORLA3 = new Operation();
    ORLA3->Name = "Eye arrow offset left";
    ORLA3->ValueSet<Operations>(Operations::Add, 0);
    ORLA3->Values.Add(Vector2D(0, 0), 1); 
    ORLA3->Values.Add(Vector2D(1.5, -1.5), 2);
    P3->AddModule(ORLA3, 6);

    Operation *ORRA3 = new Operation();
    ORRA3->Name = "Eye arrow offset right";
    ORRA3->ValueSet<Operations>(Operations::Add, 0);
    ORRA3->Values.Add(Vector2D(0, 0), 1); 
    ORRA3->Values.Add(Vector2D(-1.5, -1.5), 2);
    P3->AddModule(ORRA3, 7);

    Operation *ORLA4 = new Operation();
    ORLA4->Name = "Eye arrow combine left";
    ORLA4->ValueSet<Operations>(Operations::Combine, 0);
    ORLA4->Values.Add(Vector2D(), 1);
    ORLA4->Values.Add<Number>(0, 2);
    P3->AddModule(ORLA4, 8);

    Operation *ORRA4 = new Operation();
    ORRA4->Name = "Eye arrow combine right";
    ORRA4->ValueSet<Operations>(Operations::Combine, 0);
    ORRA4->Values.Add(Vector2D(), 1);
    ORRA4->Values.Add<Number>(0, 2);
    P3->AddModule(ORRA4, 9);

    OR2->Modules.Add(IDClass(ORLA3->ID.Base(), 1 + 1));
    OR2->Modules.Add(IDClass(ORRA3->ID.Base(), 1 + 1));
    ORLA3->Modules.Add(IDClass(ORLA4->ID.Base(), 1 + 1));
    ORRA3->Modules.Add(IDClass(ORRA4->ID.Base(), 1 + 1));
    ORLA4->Modules.Add(IDClass(LG2A->ID.Base(), LG2A->Position + 1));
    ORLA4->Modules.Add(IDClass(LG2AC->ID.Base(), LG2AC->Position + 1));
    ORRA4->Modules.Add(IDClass(RG2A->ID.Base(), RG2A->Position + 1));
    ORRA4->Modules.Add(IDClass(RG2AC->ID.Base(), RG2AC->Position + 1));

    // PUPIL SETS
    Program *PSN = new Program();
    PSN->ValueSet<ProgramTypes>(ProgramTypes::Sequence, PSN->Mode);
    PSN->Flags += Flags::Favourite;
    PSN->Name = "Normal Pupil";

    Operation *OSNE = new Operation();
    OSNE->Name = "Normal Enable";
    OSNE->ValueSet<Operations>(Operations::ResetFlags, 0);
    OSNE->Values.Add(FlagClass(Flags::Inactive), 1);
    OSNE->Modules.Add(RS2->ID);
    OSNE->Modules.Add(LS2->ID);

    Operation *OSND = new Operation();
    OSND->Name = "Normal Disable";
    OSND->ValueSet<Operations>(Operations::SetFlags, 0);
    OSND->Values.Add(FlagClass(Flags::Inactive), 1);
    OSND->Modules.Add(RS2A->ID);
    OSND->Modules.Add(LS2A->ID);

    PSN->Modules.Add(OSNE, 0);
    PSN->Modules.Add(OSND, 1);

    Program *PSA = new Program();
    PSA->ValueSet<ProgramTypes>(ProgramTypes::Sequence, PSA->Mode);
    PSA->Flags += Flags::Favourite;
    PSA->Name = "Arrow Pupil";

    Operation *OSAE = new Operation();
    OSAE->Name = "Arrow Enable";
    OSAE->ValueSet<Operations>(Operations::ResetFlags, 0);
    OSAE->Values.Add(FlagClass(Flags::Inactive), 1);
    OSAE->Modules.Add(RS2A->ID);
    OSAE->Modules.Add(LS2A->ID);

    Operation *OSAD = new Operation();
    OSAD->Name = "Arrow Disable";
    OSAD->ValueSet<Operations>(Operations::SetFlags, 0);
    OSAD->Values.Add(FlagClass(Flags::Inactive), 1);
    OSAD->Modules.Add(RS2->ID);
    OSAD->Modules.Add(LS2->ID);

    PSA->Modules.Add(OSAE, 0);
    PSA->Modules.Add(OSAD, 1);
};

    /*// LED COLLAR
    LEDStripClass *C = new LEDStripClass();
    C->Name = "LED Collar";
    C->Modules.Add(Board.Modules[1], C->Port);
    *C->Values.At<int32_t>(C->Length) = 13;
    *C->Values.At<uint8_t>(C->Brightness) = 200;

    LEDSegmentClass *CS = new LEDSegmentClass();
    CS->Name = "Collar Segment";
    C->AddModule(CS);
    *CS->Values.At<int32_t>(CS->End) = 12;
    Texture1D *CT = CS->Modules.Get<Texture1D>(CS->Texture);
    CT->Name = "Collar Texture";
    *CT->Values.At<Textures1D>(CT->TextureType) = Textures1D::Point;
    CT->Setup();
    *CT->Values.At<ColourClass>(CT->ColourA) = ColourClass(255, 0, 0);
    *CT->Values.At<ColourClass>(CT->ColourB) = ColourClass(255, 150, 0);
    *CT->Values.At<Number>(CT->Width) = 6;
    *CT->Values.At<Number>(CT->Position) = 0;*/

    /* Program *PC = new Program();
     *PC->Values.At<ProgramTypes>(PC->Mode) = ProgramTypes::Sequence;
     PC->Flags += Flags::RunLoop;
     PC->Name = "Collar program";

     // Add delay for time
     Operation *OC1 = new Operation();
     OC1->Name = "Collar timing";
     *OC1->Values.At<Operations>(0) = Operations::AddDelay;
     OC1->Values.Add<uint32_t>(0, 1);
     PC->AddModule(OC1, 0);

     // Add equal for value - down position
     Operation *OC2 = new Operation();
     OC2->Name = "Collar sine";
     *OC2->Values.At<Operations>(0) = Operations::Sine;
     OC2->Values.Add((uint32_t)0, 1);
     OC2->Values.Add((Number)0.0005, 2);
     OC2->Values.Add((Number)0, 3);
     OC1->Modules.Add(IDClass(OC2->ID.Base(), 1 + 1));
     PC->AddModule(OC2, 1);

     // Add equal for value - down position
     Operation *OC3 = new Operation();
     OC3->Name = "Collar gain";
     *OC3->Values.At<Operations>(0) = Operations::Multiply;
     OC3->Values.Add((Number)0, 1);
     OC3->Values.Add((Number)6, 2);
     OC2->Modules.Add(IDClass(OC3->ID.Base(), 1 + 1));

     OC3->Modules.Add(IDClass(CT->ID.Base(), CT->Position + 1));
     PC->AddModule(OC3, 2);*/

/*
    // Width animation
    Program *P = new Program();
    *P->Values.At<ProgramTypes>(P->Mode) = ProgramTypes::Sequence;
    P->Flags += Flags::RunLoop;
    P->Name = "Width colour animation";

    Operation *AO = new Operation();
    *AO->Values.At<Operations>(0) = Operations::MoveTo;
    AO->Values.Add<Number>(0, 1);
    AO->Values.Add<uint32_t>(0, 2);
    AO->Modules.Add(IDClass(T1->ID.Base(), T1->Width + 1));

    // Add equal for value
    Operation *O1 = new Operation();
    *O1->Values.At<Operations>(0) = Operations::Equal;
    O1->Values.Add<Number>(0, 1);
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
    O3->Values.Add<Number>(15, 1);
    O3->Modules.Add(IDClass(AO->ID.Base(), 1 + 1));
    P->AddModule(O3, 3);

    // Add delay for time again
    P->AddModule(O2, 4);

    // Animate again
    P->AddModule(AO, 5);



    // LED STRIP

    // SERVO
    ServoClass *Ser = new ServoClass();
    Ser->Modules.Add(Board.Modules[5], Ser->Port);
    */
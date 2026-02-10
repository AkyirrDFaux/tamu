void DefaultSetup() {
    Board.ValueSet(Text("Valu v2.0 Test"), Board.DisplayName);

    DisplayClass *D = new DisplayClass();
    D->Name = "Display";
    D->ValueSet<Displays>(Displays::GenericLEDMatrix, D->DisplayType);
    Board.Modules[4]->AddModule(D);
    D->ValueSet<uint8_t>(10, D->Brightness);

    Shape2DClass *S = new Shape2DClass();
    S->Name = "Background";
    D->AddModule(S);
    Texture2D *T = new Texture2D();
    T->Name = "Background";
    S->AddModule(T, S->Texture);
    T->ValueSet<Textures2D>(Textures2D::Full, T->Texture);
    T->ValueSet<ColourClass>(ColourClass(200, 0, 0), T->ColourA);
    Geometry2DClass *G = new Geometry2DClass();
    G->Name = "Background";
    S->AddModule(G);
    G->ValueSet<Geometries>(Geometries::Fill, G->Geometry);
};
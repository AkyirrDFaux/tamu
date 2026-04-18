class ColourClass
{
public:
    uint8_t R = 0;
    uint8_t G = 0;
    uint8_t B = 0;
    uint8_t A = 0;
    ColourClass(const ColourClass&) = default;
    ColourClass(uint8_t R = 0, uint8_t G = 0, uint8_t B = 0, uint8_t A = 255);
    static ColourClass FromHSV(Number H, Number S, Number V, uint8_t Alpha = 255);

    void operator=(ColourClass Colour);

    void Layer(ColourClass LayerColour, Number Overlap);
    //Text AsString();
};

ColourClass::ColourClass(uint8_t R, uint8_t G, uint8_t B, uint8_t A)
{
    this->R = R;
    this->G = G;
    this->B = B;
    this->A = A;
};

ColourClass ColourClass::FromHSV(Number H, Number S, Number V, uint8_t Alpha)
{
    // Convert inputs to 8-bit integers for high-speed bitwise math
    uint8_t h = (uint8_t)H.RoundToInt();
    uint8_t s = (uint8_t)S.RoundToInt();
    uint8_t v = (uint8_t)V.RoundToInt();

    uint8_t r, g, b;

    if (s == 0) 
    {
        // Achromatic (Grey)
        r = g = b = v;
    }
    else 
    {
        // Sector is 0-5
        uint8_t sector = h / 43; 
        // Fractional part of sector
        uint8_t remainder = (h - (sector * 43)) * 6; 

        uint8_t p = (v * (255 - s)) >> 8;
        uint8_t q = (v * (255 - ((s * remainder) >> 8))) >> 8;
        uint8_t t = (v * (255 - ((s * (255 - remainder)) >> 8))) >> 8;

        switch (sector) 
        {
            case 0: r = v; g = t; b = p; break;
            case 1: r = q; g = v; b = p; break;
            case 2: r = p; g = v; b = t; break;
            case 3: r = p; g = q; b = v; break;
            case 4: r = t; g = p; b = v; break;
            default: r = v; g = p; b = q; break;
        }
    }

    return ColourClass(r, g, b, Alpha);
}

void ColourClass::operator=(ColourClass Colour)
{
    R = Colour.R;
    G = Colour.G;
    B = Colour.B;
    A = Colour.A;
};

void ColourClass::Layer(ColourClass LayerColour, Number Overlap)
{
    Number Opacity = Overlap * ByteToPercent(LayerColour.A);
    R = (LayerColour.R * Opacity + R * (1 - Opacity)).RoundToInt(); // Opaque + Leftover
    G = (LayerColour.G * Opacity + G * (1 - Opacity)).RoundToInt();
    B = (LayerColour.B * Opacity + B * (1 - Opacity)).RoundToInt();
    A = LimitByte(A + ((255 - A) * Opacity).RoundToInt());
};

/*
String ColourClass::AsString()
{
    return (String(R) + " " + String(G) + " " + String(B) + " " + String(A));
};*/
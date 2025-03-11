class ColourClass
{
public:
    uint8_t R = 0;
    uint8_t G = 0;
    uint8_t B = 0;
    uint8_t A = 0;
    ColourClass(uint8_t R = 0, uint8_t G = 0, uint8_t B = 0, uint8_t A = 255);

    void operator=(ColourClass Colour);

    void Layer(ColourClass LayerColour, float Overlap);
    void ToDisplay(uint8_t Brightness);
    void TimeBlend(ColourClass Target, unsigned long TargetTime);

    String AsString();
    //operator String();
};

ColourClass::ColourClass(uint8_t R, uint8_t G, uint8_t B, uint8_t A)
{
    this->R = R;
    this->G = G;
    this->B = B;
    this->A = A;
};

void ColourClass::operator=(ColourClass Colour)
{
    R = Colour.R;
    G = Colour.G;
    B = Colour.B;
    A = Colour.A;
};

void ColourClass::Layer(ColourClass LayerColour, float Overlap)
{
    float Opacity = Overlap * ByteToPercent(LayerColour.A);
    R = LayerColour.R * Opacity + R * (1 - Opacity); // Opaque + Leftover
    G = LayerColour.G * Opacity + G * (1 - Opacity);
    B = LayerColour.B * Opacity + B * (1 - Opacity);
    A = LimitByte(A + (uint8_t)((255 - A) * Opacity));
};

void ColourClass::ToDisplay(uint8_t Brightness)
{
    A = MultiplyBytePercentByte(A, Brightness);
    R = MultiplyBytePercentByte(R, A);
    G = MultiplyBytePercentByte(G, A);
    B = MultiplyBytePercentByte(B, A);
};

void ColourClass::TimeBlend(ColourClass Target, unsigned long TargetTime)
{
    float Step = TimeStep(TargetTime);
    R = LimitByte(R + (Target.R - R) * Step);
    G = LimitByte(G + (Target.G - G) * Step);
    B = LimitByte(B + (Target.B - B) * Step);
    A = LimitByte(A + (Target.A - A) * Step);
};

String ColourClass::AsString()
{
    return (String(R) + " " + String(G) + " " + String(B) + " " + String(A));
};
/*
ColourClass::operator String()
{
    return (String(R) + " " + String(G) + " " + String(B) + " " + String(A));
};*/
/*
template <>
ByteArray::ByteArray(ColourClass Data)
{
    *this = ByteArray(Data.R) << ByteArray(Data.G) << ByteArray(Data.B) << ByteArray(Data.A);
};*/
/*
template <>
ColourClass ByteArray::As()
{
    ColourClass New;
    New.R = SubArray(0);
    New.G = SubArray(sizeof(uint8_t));
    New.B = SubArray(sizeof(uint8_t) * 2);
    New.A = SubArray(sizeof(uint8_t) * 3);
    return New;
};
*/
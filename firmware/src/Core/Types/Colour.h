#pragma once

class ColourClass
{
public:
    uint8_t Red = 0;
    uint8_t Green = 0;
    uint8_t Blue = 0;
    uint8_t Alpha = 0;
    // Copy constructor
    ColourClass(const ColourClass&) = default;
    // Constructs a colour from RGBA components (defaults to opaque white)
    ColourClass(uint8_t Red = 0, uint8_t Green = 0, uint8_t Blue = 0, uint8_t Alpha = 255);

    // Copies the RGBA components of `Colour` into this colour
    void operator=(ColourClass Colour);

    // Overlays `LayerColour` onto this colour by `Overlap` (0.0-1.0), blending channels and alpha
    void Layer(ColourClass LayerColour, Number Overlap);
};

// Stores the given RGBA components into the colour
inline ColourClass::ColourClass(uint8_t Red, uint8_t Green, uint8_t Blue, uint8_t Alpha)
{
    this->Red = Red;
    this->Green = Green;
    this->Blue = Blue;
    this->Alpha = Alpha;
};

// Assigns the RGBA components of `Colour` to this colour
inline void ColourClass::operator=(ColourClass Colour)
{
    Red = Colour.Red;
    Green = Colour.Green;
    Blue = Colour.Blue;
    Alpha = Colour.Alpha;
};

// Blends `LayerColour` over this colour by `Overlap` (0.0-1.0), including alpha accumulation
inline void ColourClass::Layer(ColourClass LayerColour, Number Overlap)
{
    Number Opacity = Overlap * ByteToPercent(LayerColour.Alpha);
    // Clamp the blended channels: an Out-of-range Overlap (>1) would otherwise extrapolate
    // past 0..255 and wrap in the uint8_t fields.
    Red = (uint8_t)LimitByte((LayerColour.Red * Opacity + Red * (1 - Opacity)).RoundToInt());
    Green = (uint8_t)LimitByte((LayerColour.Green * Opacity + Green * (1 - Opacity)).RoundToInt());
    Blue = (uint8_t)LimitByte((LayerColour.Blue * Opacity + Blue * (1 - Opacity)).RoundToInt());
    Alpha = LimitByte(Alpha + ((255 - Alpha) * Opacity).RoundToInt());
};

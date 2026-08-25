#pragma once

class ColourClass
{
public:
    uint8_t R = 0;
    uint8_t G = 0;
    uint8_t B = 0;
    uint8_t A = 0;
    // Copy constructor
    ColourClass(const ColourClass&) = default;
    // Constructs a colour from RGBA components (defaults to opaque white)
    ColourClass(uint8_t R = 0, uint8_t G = 0, uint8_t B = 0, uint8_t A = 255);

    // Copies the RGBA components of `Colour` into this colour
    void operator=(ColourClass Colour);

    // Overlays `LayerColour` onto this colour by `Overlap` (0.0-1.0), blending channels and alpha
    void Layer(ColourClass LayerColour, Number Overlap);
};

// Stores the given RGBA components into the colour
inline ColourClass::ColourClass(uint8_t R, uint8_t G, uint8_t B, uint8_t A)
{
    this->R = R;
    this->G = G;
    this->B = B;
    this->A = A;
};

// Assigns the RGBA components of `Colour` to this colour
inline void ColourClass::operator=(ColourClass Colour)
{
    R = Colour.R;
    G = Colour.G;
    B = Colour.B;
    A = Colour.A;
};

// Blends `LayerColour` over this colour by `Overlap` (0.0-1.0), including alpha accumulation
inline void ColourClass::Layer(ColourClass LayerColour, Number Overlap)
{
    Number Opacity = Overlap * ByteToPercent(LayerColour.A);
    // Clamp the blended channels: an Out-of-range Overlap (>1) would otherwise extrapolate
    // past 0..255 and wrap in the uint8_t fields.
    R = (uint8_t)LimitByte((LayerColour.R * Opacity + R * (1 - Opacity)).RoundToInt());
    G = (uint8_t)LimitByte((LayerColour.G * Opacity + G * (1 - Opacity)).RoundToInt());
    B = (uint8_t)LimitByte((LayerColour.B * Opacity + B * (1 - Opacity)).RoundToInt());
    A = LimitByte(A + ((255 - A) * Opacity).RoundToInt());
};

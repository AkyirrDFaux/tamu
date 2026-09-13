#pragma once

// Keyed-dictionary keys and enums for the LED display render block
// (Docs/Modules and blocks/LED display.md). The render block is a dynamic block
// whose fields are keyed dictionaries: Geometry fields (DataType::Geometry)
// build the alpha mask, Texture fields (DataType::Texture) fill it. Fields are
// processed consecutively in the block's index order.

// Keys of a Geometry dictionary (mask).
enum class GeometryKey : uint8_t
{
    Operation = 0,    // GeometryOperation
    Shape = 1,        // Geometries
    Position = 2,     // Matrix 2x3 (2D transformation)
    Size = 3,         // Vector<2> / Number
    Fade = 4,         // Number, pixels
    Alpha = 5,        // Number, 0..1 (default 1)
    Rounding = 6,     // Number, pixels
    Angles = 7,       // Number / Vector
    PointNumber = 8,  // Integer
    PointCoordinates = 9, // Matrix 2xN
    NoiseSeed = 10,   // Integer
};

// Geometry shapes (docs order). Only a subset is rendered initially; the rest
// resolve to zero alpha until implemented.
enum class Geometries : uint8_t
{
    None = 0,
    Fill = 1,
    HalfFill = 2,
    Square = 3,
    Rectangle = 4,
    Trapezoid = 5,
    Circle = 6,
    Ellipse = 7,
    DoubleParabola = 8,
    Triangle = 9,
    Polygon = 10,
    Star = 11,
    Mesh = 12,
    Noise = 13,
};

// How a geometry modifies the current mask.
enum class GeometryOperation : uint8_t
{
    Replace = 0,
    Add = 1,
    Cut = 2,
    Intersect = 3,
    XOR = 4,
};

// Keys of a Texture dictionary (fill).
enum class TextureKey : uint8_t
{
    Type = 0,       // Textures2D
    Position = 1,   // Matrix 2x3 (defines centre)
    Size = 2,       // Vector<2> / Number
    Colour1 = 3,    // Colour (RGBA)
    Colour2 = 4,    // Colour (RGBA)
    Colour3 = 5,    // Colour (RGBA)
    Amount = 6,     // Number
};

// Texture/effect types; effects continue the same enum
// (docs: "Effect list (continues texture enum)").
enum class Textures2D : uint8_t
{
    None = 0,
    Fill = 1,
    GradientLinear = 2,
    GradientCircular = 3,
    InvertColour = 4,
    HueShift = 5,
    Contrast = 6,
    Brightness = 7,
};
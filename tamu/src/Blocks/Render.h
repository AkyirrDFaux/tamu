#pragma once
enum class GeometryKey : uint8_t
{
    Invalid = 0x00,
    Shape = 0x01,          // enum
    Operation = 0x02,      // enum
    Transformation = 0x03, // Matrix 3x3
    Width = 0x04,          // Number
    Height = 0x05,         // Number
    Radius = 0x06,         // Number
    PointNumber = 0x07,    // Number
    EdgeFade = 0x08        // Number
};

enum class Geometries : uint8_t
{
    None = 0,
    Box,
    Elipse,
    Triangle,
    Polygon,
    Star,
    DoubleParabola,
    Fill,
    HalfFill
};

enum class GeometryOperation : uint8_t
{
    Add,
    Cut,
    Intersect,
    XOR
};

struct GeometryDefinition
{
    Geometries Type = Geometries::None;

    struct BasicShape
    {
        Number Width, Height, EdgeFade;
    };
    struct PolygonShape
    {
        Number Radius, PointNumber, EdgeFade;
    };
    struct HalfPlaneShape
    {
        Number EdgeFade;
    };

    union GeometryData
    {
        BasicShape Basic;
        PolygonShape Polygon;
        HalfPlaneShape HalfPlane;

        // Unions need a constructor if members are non-trivial
        // Default-constructs (zeroes) the Basic member so no geometry reads
        // indeterminate memory before ResolveGeometryDefinition fills it in.
        GeometryData() : Basic{} {}
        // Trivial destructor required by the non-trivial union members.
        ~GeometryData() {}
    } Data;

    // Provide a constructor for the parent struct
    // Initialises the geometry type to Geometries::None.
    GeometryDefinition() : Type(Geometries::None) {}
};

enum class TextureKey : uint8_t
{
    Invalid = 0x00,
    Type = 0x01,
    Transformation = 0x05,
    Colour1 = 0x02,
    Colour2 = 0x03,
    Width = 0x04,
};

enum class Textures2D : uint8_t
{
    None = 0,
    Full,
    BlendLinear,
    BlendCircular,
};

struct TextureDefinition
{
    Textures2D Type = Textures2D::None;

    struct FillDef
    {
        ColourClass Colour;
    };
    struct Blend2Def
    {
        ColourClass Colour1, Colour2;
        Number Width;
    };

    union TextureData
    {
        FillDef Fill;
        Blend2Def Blend2;

        // Unions need a constructor if members are non-trivial
        // Default-constructs (zeroes) the Fill member so no texture reads
        // indeterminate memory before ResolveTextureDefinition fills it in.
        TextureData() : Fill{} {}
        // Trivial destructor required by the non-trivial union members.
        ~TextureData() {}
    } Data;

    // Provide a constructor for the parent struct
    // Initialises the texture type to Textures2D::None.
    TextureDefinition() : Type(Textures2D::None) {}
};
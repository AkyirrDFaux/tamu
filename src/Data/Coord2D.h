class Coord2D
{
public:
    // Relative to origin, offset first, rotate second
    Vector2D Offset = Vector2D(0, 0);
    Vector2D Rotation = Vector2D(0);
    Vector2D TransformFrom(Vector2D Vector); // Vector transformed to origin
    Vector2D TransformTo(Vector2D Vector);   // Vector from origin to transformed
    Coord2D Join(Coord2D Other);             // Other is 2nd transform

    Coord2D();
    Coord2D(Vector2D NewOffset, Vector2D NewRotation);
    Coord2D(Number X, Number Y, Number Angle);

    //Text AsString();

    Coord2D TimeMove(Coord2D Target, unsigned long TargetTime);
};

inline Coord2D::Coord2D() {};

inline Coord2D::Coord2D(Vector2D NewOffset, Vector2D NewRotation)
{
    Offset = NewOffset;
    Rotation = NewRotation;
};
inline Coord2D::Coord2D(Number X, Number Y, Number Angle)
{
    Offset = Vector2D(X, Y);
    Rotation = Vector2D(Angle);
};

inline Vector2D Coord2D::TransformFrom(Vector2D Vector)
{
    return Rotation.Rotate(Vector) + Offset;
};

inline Vector2D Coord2D::TransformTo(Vector2D Vector)
{
    return Rotation.RotateInverted(Vector - Offset); // Check
};

inline Coord2D Coord2D::Join(Coord2D Other)
{
    Coord2D Joined;
    Joined.Offset = Offset + Rotation.Rotate(Other.Offset);
    Joined.Rotation = Rotation.Rotate(Other.Rotation); // AAAAAAAAAAAAAAAAAAAAAAAA
    return Joined;
}
/*
inline String Coord2D::AsString()
{
    return Offset.AsString() + " " + Rotation.AsStringAngle();
};*/

inline Coord2D Coord2D::TimeMove(Coord2D Target, unsigned long TargetTime)
{
    return Coord2D(Offset.TimeMove(Target.Offset, TargetTime), Rotation.TimeMoveAngle(Target.Rotation, TargetTime));
};
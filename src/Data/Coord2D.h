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
    Coord2D(float X, float Y, float Angle);

    String AsString();

    Coord2D TimeMove(Coord2D Target, unsigned long TargetTime);
};

Coord2D::Coord2D() {};

Coord2D::Coord2D(Vector2D NewOffset, Vector2D NewRotation)
{
    Offset = NewOffset;
    Rotation = NewRotation;
};
Coord2D::Coord2D(float X, float Y, float Angle)
{
    Offset = Vector2D(X, Y);
    Rotation = Vector2D(Angle);
};

Vector2D Coord2D::TransformFrom(Vector2D Vector)
{
    return Rotation.Rotate(Vector) + Offset;
};

Vector2D Coord2D::TransformTo(Vector2D Vector)
{
    return Rotation.RotateInverted(Vector - Offset); // Check
};

Coord2D Coord2D::Join(Coord2D Other)
{
    Coord2D Joined;
    Joined.Offset = Offset + Rotation.Rotate(Other.Offset);
    Joined.Rotation = Rotation.Rotate(Other.Rotation); // AAAAAAAAAAAAAAAAAAAAAAAA
    return Joined;
}

String Coord2D::AsString()
{
    return Offset.AsString() + " " + Rotation.AsStringAngle();
};

Coord2D Coord2D::TimeMove(Coord2D Target, unsigned long TargetTime)
{
    return Coord2D(Offset.TimeMove(Target.Offset, TargetTime), Rotation.TimeMoveAngle(Target.Rotation, TargetTime));
};

/*
template <>
ByteArray::ByteArray(Coord2D Data)
{
    *this = ByteArray(Data.Offset) << ByteArray(Data.Rotation);
};

template <>
Coord2D ByteArray::As()
{
    Coord2D New;
    New.Offset = SubArray(0);
    New.Rotation = SubArray(sizeof(float) * 2);
    return New;
};*/
class Vector3D
{
public:
    // y|_x
    float X = 0;
    float Y = 0;
    float Z = 0;

    Vector3D();
    Vector3D(float X, float Y, float Z);

    float GetByIndex(uint8_t Index);
};

Vector3D::Vector3D() {};

Vector3D::Vector3D(float X, float Y, float Z)
{
    this->X = X;
    this->Y = Y;
    this->Z = Z;
};

float Vector3D::GetByIndex(uint8_t Index)
{
    if (Index == 0)
        return X;
    else if (Index == 1)
        return Y;
    else if (Index == 2)
        return Z;
    else
        return 0;
}

/*
Vector2D Vector2D::operator+(Vector2D Other)
{
    return Vector2D(X + Other.X, Y + Other.Y);
};

Vector2D Vector2D::operator-()
{
    return Vector2D(-X, -Y);
};

Vector2D Vector2D::operator-(Vector2D Other)
{
    return Vector2D(X - Other.X, Y - Other.Y);
};

Vector2D Vector2D::operator*(Vector2D Other)
{
    return Vector2D(X * Other.X, Y * Other.Y);
};

Vector2D Vector2D::operator*(float Scale)
{
    return Vector2D(X * Scale, Y * Scale);
};

float Vector2D::Length()
{
    return sqrtf(sq(X) + sq(Y));
};

float Vector2D::Distance(Vector2D Other)
{
    return (*this - Other).Length();
};
*/
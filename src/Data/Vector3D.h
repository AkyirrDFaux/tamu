class Vector3D
{
public:
    // y|_x
    Number X = 0;
    Number Y = 0;
    Number Z = 0;

    Vector3D();
    Vector3D(Number X, Number Y, Number Z);

    Number GetByIndex(uint8_t Index);
    String AsString();
};

Vector3D::Vector3D() {};

Vector3D::Vector3D(Number X, Number Y, Number Z)
{
    this->X = X;
    this->Y = Y;
    this->Z = Z;
};

Number Vector3D::GetByIndex(uint8_t Index)
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

String Vector3D::AsString()
{
    return (String(X) + " " + String(Y) + " " + String(Z));
};

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

Vector2D Vector2D::operator*(Number Scale)
{
    return Vector2D(X * Scale, Y * Scale);
};

Number Vector2D::Length()
{
    return sqrtf(sq(X) + sq(Y));
};

Number Vector2D::Distance(Vector2D Other)
{
    return (*this - Other).Length();
};
*/
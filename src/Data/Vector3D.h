class Vector3D
{
public:
    // y|_x
    float X = 0;
    float Y = 0;
    float Z = 0;

    Vector3D();
    Vector3D(float X, float Y, float Z);
};

Vector3D::Vector3D() {};

Vector3D::Vector3D(float X, float Y, float Z)
{
    this->X = X;
    this->Y = Y;
    this->Z = Z;
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
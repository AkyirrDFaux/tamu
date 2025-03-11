class Vector2D
{
public:
    // y|_x
    float X = 0;
    float Y = 0;

    Vector2D();
    Vector2D(float X, float Y);
    Vector2D(float Angle);
    void FromAngle(float Angle);
    float ToAngle();

    void operator=(Vector2D Other);
    Vector2D operator+(Vector2D Other);
    Vector2D operator-();
    Vector2D operator-(Vector2D Other);
    Vector2D operator*(Vector2D Other);
    Vector2D operator*(float Scale);

    float Length();
    float Distance(Vector2D Other);
    float DotProduct(Vector2D Other);
    Vector2D Rotate(Vector2D Other);
    Vector2D RotateInverted(Vector2D Other);
    Vector2D Mirror(Vector2D Other);
    Vector2D Abs();

    Vector2D TimeMove(Vector2D Target, unsigned long TargetTime);
    Vector2D TimeMoveAngle(Vector2D Target, unsigned long TargetTime);

    String AsString();
    String AsStringAngle();
};

Vector2D::Vector2D() {};

Vector2D::Vector2D(float X, float Y)
{
    this->X = X;
    this->Y = Y;
};

Vector2D::Vector2D(float Angle) // In degrees
{
    FromAngle(Angle);
};

void Vector2D::FromAngle(float Angle)
{
    X = cos(Angle * PI / 180);
    Y = sin(Angle * PI / 180);
};

float Vector2D::ToAngle() //Radians
{
    return atan2(Y, X);
};

void Vector2D::operator=(Vector2D Other)
{
    this->X = Other.X;
    this->Y = Other.Y;
};

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

float Vector2D::DotProduct(Vector2D Other)
{
    return X * Other.X + Y * Other.Y; //<x,y> = x1*y1 + x2*y2
};

Vector2D Vector2D::Rotate(Vector2D Other) // Rotate other, this is angle (sin, cos)
{
    return Vector2D(X * Other.X - Y * Other.Y, Y * Other.X + X * Other.Y); // CCW
};

Vector2D Vector2D::RotateInverted(Vector2D Other)
{
    return Vector2D(X * Other.X + Y * Other.Y, -Y * Other.X + X * Other.Y); // CW
};

Vector2D Vector2D::Mirror(Vector2D Other) // Other is lateral to the mirroring axis (x)
{
    return *this - Other * 2 * this->DotProduct(Other); // x-2*<x,y>*y
};

Vector2D Vector2D::Abs()
{
    return Vector2D(abs(X), abs(Y));
};

Vector2D Vector2D::TimeMove(Vector2D Target, unsigned long TargetTime)
{
    float Step = TimeStep(TargetTime);
    return *this + (Target - *this) * Step;
};

Vector2D Vector2D::TimeMoveAngle(Vector2D Target, unsigned long TargetTime)
{
    float Step = TimeStep(TargetTime);
    return Vector2D(this->ToAngle() + LimitPi(Target.ToAngle() - this->ToAngle()) * Step);
}

String Vector2D::AsString()
{
    return (String(X) + " " + String(Y));
};

String Vector2D::AsStringAngle() // In degrees
{
    return (String(ToAngle() * 180 / PI));
};
/*
template <>
ByteArray::ByteArray(Vector2D Data)
{
    *this = ByteArray(Data.X) << ByteArray(Data.Y);
};

template <>
Vector2D ByteArray::As()
{
    Vector2D New;
    New.X = SubArray(0);
    New.Y = SubArray(sizeof(float));
    return New;
};*/
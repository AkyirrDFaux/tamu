class Vector2D
{
public:
    // y|_x
    Number X = 0;
    Number Y = 0;

    Vector2D();
    Vector2D(Number X, Number Y);
    Vector2D(Number Angle);
    void FromAngle(Number Angle);
    Number ToAngle();

    void operator=(Vector2D Other);
    Vector2D operator+(Vector2D Other);
    Vector2D operator-();
    Vector2D operator-(Vector2D Other);
    Vector2D operator*(Vector2D Other);
    Vector2D operator*(Number Scale);

    Number Length();
    Number Distance(Vector2D Other);
    Number DotProduct(Vector2D Other);
    Vector2D Rotate(Vector2D Other);
    Vector2D RotateInverted(Vector2D Other);
    Vector2D Mirror(Vector2D Other);
    Vector2D Abs();

    Vector2D TimeMove(Vector2D Target, unsigned long TargetTime);
    Vector2D TimeMoveAngle(Vector2D Target, unsigned long TargetTime);

    String AsString();
    String AsStringAngle();
};

inline Vector2D::Vector2D() {};

inline Vector2D::Vector2D(Number X, Number Y)
{
    this->X = X;
    this->Y = Y;
};

inline Vector2D::Vector2D(Number Angle) // In degrees
{
    FromAngle(Angle);
};

inline void Vector2D::FromAngle(Number Angle)
{
    X = cos(Angle * PI / 180);
    Y = sin(Angle * PI / 180);
};

inline Number Vector2D::ToAngle() //Radians
{
    return atan2(Y, X);
};

inline void Vector2D::operator=(Vector2D Other)
{
    this->X = Other.X;
    this->Y = Other.Y;
};

inline Vector2D Vector2D::operator+(Vector2D Other)
{
    return Vector2D(X + Other.X, Y + Other.Y);
};

inline Vector2D Vector2D::operator-()
{
    return Vector2D(-X, -Y);
};

inline Vector2D Vector2D::operator-(Vector2D Other)
{
    return Vector2D(X - Other.X, Y - Other.Y);
};

inline Vector2D Vector2D::operator*(Vector2D Other)
{
    return Vector2D(X * Other.X, Y * Other.Y);
};

inline Vector2D Vector2D::operator*(Number Scale)
{
    return Vector2D(X * Scale, Y * Scale);
};

inline Number Vector2D::Length()
{
    return sqrt(sq(X) + sq(Y));
};

inline Number Vector2D::Distance(Vector2D Other)
{
    return (*this - Other).Length();
};

inline Number Vector2D::DotProduct(Vector2D Other)
{
    return X * Other.X + Y * Other.Y; //<x,y> = x1*y1 + x2*y2
};

inline Vector2D Vector2D::Rotate(Vector2D Other) // Rotate other, this is angle (sin, cos)
{
    return Vector2D(X * Other.X - Y * Other.Y, Y * Other.X + X * Other.Y); // CCW
};

inline Vector2D Vector2D::RotateInverted(Vector2D Other)
{
    return Vector2D(X * Other.X + Y * Other.Y, -Y * Other.X + X * Other.Y); // CW
};

inline Vector2D Vector2D::Mirror(Vector2D Other) // Other is lateral to the mirroring axis (x)
{
    return *this - Other * 2 * this->DotProduct(Other); // x-2*<x,y>*y
};

inline Vector2D Vector2D::Abs()
{
    return Vector2D(abs(X), abs(Y));
};

inline Vector2D Vector2D::TimeMove(Vector2D Target, unsigned long TargetTime)
{
    Number Step = TimeStep(TargetTime);
    return *this + (Target - *this) * Step;
};

inline Vector2D Vector2D::TimeMoveAngle(Vector2D Target, unsigned long TargetTime)
{
    Number Step = TimeStep(TargetTime);
    return Vector2D(this->ToAngle() + LimitPi(Target.ToAngle() - this->ToAngle()) * Step);
}

inline String Vector2D::AsString()
{
    return (String(X) + " " + String(Y));
};

inline String Vector2D::AsStringAngle() // In degrees
{
    return (String(ToAngle() * 180 / PI));
};
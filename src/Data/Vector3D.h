class Vector3D
{
public:
    // y|_x
    Number X = 0;
    Number Y = 0;
    Number Z = 0;

    Vector3D();
    Vector3D(Number X, Number Y, Number Z);
};

Vector3D::Vector3D() {};

inline Vector3D::Vector3D(Number X, Number Y, Number Z)
{
    this->X = X;
    this->Y = Y;
    this->Z = Z;
};

// Basic Arithmetic Operators
inline Vector3D operator+(const Vector3D& a, const Vector3D& b) {
    return Vector3D(a.X + b.X, a.Y + b.Y, a.Z + b.Z);
}

inline Vector3D operator-(const Vector3D& a, const Vector3D& b) {
    return Vector3D(a.X - b.X, a.Y - b.Y, a.Z - b.Z);
}

inline Vector3D operator*(const Vector3D& a, const Vector3D& b) {
    return Vector3D(a.X * b.X, a.Y * b.Y, a.Z * b.Z);
}

inline Vector3D operator/(const Vector3D& a, const Vector3D& b) {
    // Component-wise division with safety checks
    return Vector3D(
        (b.X != Number(0)) ? a.X / b.X : a.X,
        (b.Y != Number(0)) ? a.Y / b.Y : a.Y,
        (b.Z != Number(0)) ? a.Z / b.Z : a.Z
    );
}

// Equality Operators (Useful for Comparison Blocks later)
inline bool operator==(const Vector3D& a, const Vector3D& b) {
    return (a.X == b.X && a.Y == b.Y && a.Z == b.Z);
}

inline bool operator!=(const Vector3D& a, const Vector3D& b) {
    return !(a == b);
}
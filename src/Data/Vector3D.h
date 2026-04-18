class Vector3D
{
public:
    // Memory Layout: Y|_X
    Number X = 0;
    Number Y = 0;
    Number Z = 0;

    // --- Constructors ---
    Vector3D();
    Vector3D(Number X, Number Y, Number Z);

    // --- Member Functions ---
    inline Number Length() { 
        return sqrt((X * X) + (Y * Y) + (Z * Z)); 
    }

    inline Number DotProduct(const Vector3D& v) const { 
        return (X * v.X) + (Y * v.Y) + (Z * v.Z); 
    }
    
    inline Vector3D CrossProduct(const Vector3D& v) const {
        return Vector3D(
            (Y * v.Z) - (Z * v.Y),
            (Z * v.X) - (X * v.Z),
            (X * v.Y) - (Y * v.X)
        );
    }

    inline Vector3D Normalize();
};

// --- Constructor Implementations ---
inline Vector3D::Vector3D() {}

inline Vector3D::Vector3D(Number X, Number Y, Number Z)
{
    this->X = X;
    this->Y = Y;
    this->Z = Z;
}

// --- Basic Vector-to-Vector Arithmetic ---
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
    return Vector3D(
        (b.X != Number(0)) ? a.X / b.X : a.X,
        (b.Y != Number(0)) ? a.Y / b.Y : a.Y,
        (b.Z != Number(0)) ? a.Z / b.Z : a.Z
    );
}

// --- Scalar Operations ---
inline Vector3D operator*(const Vector3D& a, const Number& s) {
    return Vector3D(a.X * s, a.Y * s, a.Z * s);
}

inline Vector3D operator*(const Number& s, const Vector3D& a) {
    return a * s;
}

inline Vector3D operator/(const Vector3D& a, const Number& s) {
    if (s.Value == 0) return a; 
    return Vector3D(a.X / s, a.Y / s, a.Z / s);
}

// --- Unary & Compound Assignment ---
inline Vector3D operator-(const Vector3D& a) {
    return Vector3D(-a.X, -a.Y, -a.Z);
}

inline Vector3D& operator+=(Vector3D& a, const Vector3D& b) {
    a.X += b.X; a.Y += b.Y; a.Z += b.Z;
    return a;
}

inline Vector3D& operator-=(Vector3D& a, const Vector3D& b) {
    a.X -= b.X; a.Y -= b.Y; a.Z -= b.Z;
    return a;
}

// --- Comparison Operators ---
inline bool operator==(const Vector3D& a, const Vector3D& b) {
    return (a.X == b.X && a.Y == b.Y && a.Z == b.Z);
}

inline bool operator!=(const Vector3D& a, const Vector3D& b) {
    return !(a == b);
}

// --- Deferred Member Implementations ---
inline Vector3D Vector3D::Normalize() {
    Number len = Length();
    if (len.Value == 0) return Vector3D(0, 0, 0);
    // Use scalar multiplication for efficiency
    return (*this) * (Number(1) / len);
}

// --- Utility Helpers ---
inline Number Distance(const Vector3D& a, const Vector3D& b) {
    Vector3D diff = a - b;
    return diff.Length();
}
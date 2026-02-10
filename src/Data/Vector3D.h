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
    Vector2D GetByIndex(uint8_t IndexA, uint8_t IndexB);
    //Text AsString();
};

Vector3D::Vector3D() {};

inline Vector3D::Vector3D(Number X, Number Y, Number Z)
{
    this->X = X;
    this->Y = Y;
    this->Z = Z;
};

inline Number Vector3D::GetByIndex(uint8_t Index)
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

inline Vector2D Vector3D::GetByIndex(uint8_t IndexA, uint8_t IndexB)
{
    return Vector2D(GetByIndex(IndexA), GetByIndex(IndexB));
}
/*
inline String Vector3D::AsString()
{
    return (String(X) + " " + String(Y) + " " + String(Z));
};*/
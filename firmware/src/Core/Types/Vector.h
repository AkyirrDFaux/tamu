#pragma once

// Vector/Matrix support is compiled out on SCALAR_ONLY targets (DAS).
#ifndef SCALAR_ONLY
template <size_t N>
class Vector
{
public:
    Number Data[N];
    // Mutable element access by index
    Number &operator[](size_t i) { return Data[i]; }
    // Const element access by index
    const Number &operator[](size_t i) const { return Data[i]; }

    // Vector Addition: V = V1 + V2
    Vector operator+(const Vector &Other) const
    {
        Vector Result;
        for (size_t i = 0; i < N; ++i)
        {
            Result.Data[i] = this->Data[i] + Other.Data[i];
        }
        return Result;
    }

    // Vector Subtraction: V = V1 - V2
    Vector operator-(const Vector &Other) const
    {
        Vector Result;
        for (size_t i = 0; i < N; ++i)
        {
            Result.Data[i] = this->Data[i] - Other.Data[i];
        }
        return Result;
    }

    // Scalar Multiplication: V = V1 * scalar
    // Scalar is a Number (16.16) and is multiplied as one. Passing `scalar.Value` here would
    // hand the raw fixed-point value to Number's int constructor, which scales it a second
    // time (and overflows for any scalar above ~32767).
    Vector operator*(const Number &Scalar) const
    {
        Vector Result;
        for (size_t i = 0; i < N; ++i)
            Result.Data[i] = Data[i] * Scalar;
        return Result;
    }

    // Returns the Euclidean magnitude (norm) of the vector
    Number Norm2() const
    {
        Number Sum = 0;
        for (size_t i = 0; i < N; ++i)
        {
            Sum = Sum + (Data[i] * Data[i]);
        }
        return sqrt(Sum); // Assuming Number has a sqrt() method
    }

    // Returns a NEW vector of size N-1 with the element at `pos` removed.
    // Note: Requires template specialization or conditional check for N > 0
    Vector<N - 1> Remove(size_t Pos) const
    {
        if (Pos >= N)
            Pos = N - 1; // clamp: a bad index would be a silent out-of-bounds read
        Vector<N - 1> NextVec;
        for (size_t i = 0; i < Pos; ++i)
            NextVec.Data[i] = this->Data[i];

        for (size_t i = Pos + 1; i < N; ++i)
            NextVec.Data[i - 1] = this->Data[i];

        return NextVec;
    }
};



#endif // SCALAR_ONLY

#pragma once

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
    Vector operator+(const Vector &other) const
    {
        Vector result;
        for (size_t i = 0; i < N; ++i)
        {
            result.Data[i] = this->Data[i] + other.Data[i];
        }
        return result;
    }

    // Vector Subtraction: V = V1 - V2
    Vector operator-(const Vector &other) const
    {
        Vector result;
        for (size_t i = 0; i < N; ++i)
        {
            result.Data[i] = this->Data[i] - other.Data[i];
        }
        return result;
    }

    // Scalar Multiplication: V = V1 * scalar
    // Note: Scalar is converted to Number (16.16) for fixed-point consistency
    Vector operator*(const Number &scalar) const
    {
        Vector result;
        for (size_t i = 0; i < N; ++i)
            result.Data[i] = Data[i] * scalar.Value;
        return result;
    }

    // Returns the Euclidean magnitude (norm) of the vector
    Number norm2() const
    {
        Number sum = 0;
        for (size_t i = 0; i < N; ++i)
        {
            sum = sum + (Data[i] * Data[i]);
        }
        return sqrt(sum); // Assuming Number has a sqrt() method
    }

    // Returns a NEW vector of size N+1 with `val` inserted at position `pos`
    Vector<N + 1> insert(size_t pos, Number val) const
    {
        if (pos > N)
            pos = N; // clamp: a bad index would be a silent out-of-bounds write
        Vector<N + 1> next_vec;
        for (size_t i = 0; i < pos; ++i)
            next_vec.Data[i] = this->Data[i];

        next_vec.Data[pos] = val;

        for (size_t i = pos; i < N; ++i)
            next_vec.Data[i + 1] = this->Data[i];

        return next_vec;
    }

    // Returns a NEW vector of size N-1
    // Note: Requires template specialization or conditional check for N > 0
    Vector<N - 1> remove(size_t pos) const
    {
        if (pos >= N)
            pos = N - 1; // clamp: a bad index would be a silent out-of-bounds read
        Vector<N - 1> next_vec;
        for (size_t i = 0; i < pos; ++i)
            next_vec.Data[i] = this->Data[i];

        for (size_t i = pos + 1; i < N; ++i)
            next_vec.Data[i - 1] = this->Data[i];

        return next_vec;
    }
};



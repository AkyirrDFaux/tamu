#pragma once

// Vector/Matrix support is compiled out on SCALAR_ONLY targets (DAS).
#ifndef SCALAR_ONLY
template <size_t Rows, size_t Cols>
class Matrix
{
public:
    struct
    {
        uint16_t Height;
        uint16_t Width;
        Number Data[Rows * Cols];
    } Buffer;

    // Initialises the buffer header with the matrix dimensions and zeroes all elements
    Matrix()
    {
        Buffer.Height = static_cast<uint16_t>(Rows);
        Buffer.Width = static_cast<uint16_t>(Cols);
        for (size_t i = 0; i < Rows * Cols; ++i)
        {
            Buffer.Data[i] = Number(0);
        }
    }

    // Source of truth accessors
    inline uint16_t RowCount() const { return Buffer.Height; }
    inline uint16_t ColCount() const { return Buffer.Width; }

    // Logic uses the buffer variables as the source of truth
    inline Number &operator()(uint16_t r, uint16_t c)
    {
        return Buffer.Data[r * Buffer.Width + c];
    }

    inline const Number &operator()(uint16_t r, uint16_t c) const
    {
        return Buffer.Data[r * Buffer.Width + c];
    }

    // Matrix-Vector Multiplication: Result = This * Vec (columns must match vector size)
    template <size_t N>
    Vector<Rows> operator*(const Vector<N> &Vec) const
    {
        static_assert(N == Cols, "Matrix columns must match Vector size");

        Vector<Rows> Result;
        for (uint16_t r = 0; r < Buffer.Height; ++r)
        {
            Number RowSum = 0;
            for (uint16_t c = 0; c < Buffer.Width; ++c)
            {
                // Accessing row-major data using header as source of truth
                RowSum = RowSum + ((*this)(r, c) * Vec.Data[c]);
            }
            Result.Data[r] = RowSum;
        }
        return Result;
    }

    // Matrix-Matrix Multiplication: Result = This * Other
    // Result dimensions will be [Rows x OtherCols]
    template <size_t OtherCols>
    Matrix<Rows, OtherCols> Multiply(const Matrix<Cols, OtherCols> &Other) const
    {
        Matrix<Rows, OtherCols> Result;

        for (uint16_t i = 0; i < this->RowCount(); ++i)
        {
            for (uint16_t j = 0; j < Other.ColCount(); ++j)
            {
                Number Sum = 0;
                for (uint16_t k = 0; k < this->ColCount(); ++k)
                {
                    Sum = Sum + ((*this)(i, k) * Other(k, j));
                }
                Result(i, j) = Sum;
            }
        }
        return Result;
    }

    // Builds a 3x3 homogeneous transformation matrix from a rotation angle, translation and scale
    static Matrix<3, 3> CreateTransform2D(Number Angle, const Vector<2> &Translation, const Vector<2> &Scale = {N(1.0), N(1.0)})
    {
        Matrix<3, 3> Mat = Matrix<3, 3>::Identity();

        Number c = cos(Angle);
        Number s = sin(Angle);

        // 1. Calculate Rotation/Scale components
        Number R00 = c * Scale[0];
        Number R01 = (-s) * Scale[1];
        Number R10 = s * Scale[0];
        Number R11 = c * Scale[1];

        Mat(0, 0) = R00;
        Mat(0, 1) = R01;
        Mat(1, 0) = R10;
        Mat(1, 1) = R11;

        // 2. Compensation: Pre-rotate the translation
        // In the (Local * Base) order, the Local rotation/scale is applied to
        // the translation column. To keep the translation constant, we
        // transform the desired translation vector by the rotation/scale matrix.

        Mat(0, 2) = (R00 * Translation[0]) + (R01 * Translation[1]);
        Mat(1, 2) = (R10 * Translation[0]) + (R11 * Translation[1]);

        return Mat;
    }

    // Static factory for Identity Matrix
    static Matrix<Rows, Cols> Identity()
    {
        static_assert(Rows == Cols, "Identity matrix must be square.");

        Matrix<Rows, Cols> Mat; // Constructor already zero-initialises the buffer

        for (size_t i = 0; i < Rows; ++i)
        {
            Mat(i, i) = Number(1);
        }

        return Mat;
    }
};

// Free-standing Matrix-Matrix Multiplication operator: Result = Lhs * Rhs
template <size_t R1, size_t C1, size_t C2>
Matrix<R1, C2> operator*(const Matrix<R1, C1> &Lhs, const Matrix<C1, C2> &Rhs)
{
    return Lhs.Multiply(Rhs);
}

#endif // SCALAR_ONLY

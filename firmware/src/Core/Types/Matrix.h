#pragma once

template <size_t Rows, size_t Cols>
class Matrix
{
public:
    struct
    {
        uint16_t height;
        uint16_t width;
        Number data[Rows * Cols];
    } buffer;

    // Initialises the buffer header with the matrix dimensions and zeroes all elements
    Matrix()
    {
        buffer.height = static_cast<uint16_t>(Rows);
        buffer.width = static_cast<uint16_t>(Cols);
        for (size_t i = 0; i < Rows * Cols; ++i)
        {
            buffer.data[i] = Number(0);
        }
    }

    // Source of truth accessors
    inline uint16_t rows() const { return buffer.height; }
    inline uint16_t cols() const { return buffer.width; }

    // Logic uses the buffer variables as the source of truth
    inline Number &operator()(uint16_t r, uint16_t c)
    {
        return buffer.data[r * buffer.width + c];
    }

    inline const Number &operator()(uint16_t r, uint16_t c) const
    {
        return buffer.data[r * buffer.width + c];
    }

    // Matrix-Vector Multiplication: Result = This * Vec (columns must match vector size)
    template <size_t N>
    Vector<Rows> operator*(const Vector<N> &vec) const
    {
        static_assert(N == Cols, "Matrix columns must match Vector size");

        Vector<Rows> result;
        for (uint16_t r = 0; r < buffer.height; ++r)
        {
            Number row_sum = 0;
            for (uint16_t c = 0; c < buffer.width; ++c)
            {
                // Accessing row-major data using header as source of truth
                row_sum = row_sum + ((*this)(r, c) * vec.Data[c]);
            }
            result.Data[r] = row_sum;
        }
        return result;
    }

    // Matrix-Matrix Multiplication: Result = This * Other
    // Result dimensions will be [Rows x OtherCols]
    template <size_t OtherCols>
    Matrix<Rows, OtherCols> multiply(const Matrix<Cols, OtherCols> &other) const
    {
        Matrix<Rows, OtherCols> result;

        for (uint16_t i = 0; i < this->rows(); ++i)
        {
            for (uint16_t j = 0; j < other.cols(); ++j)
            {
                Number sum = 0;
                for (uint16_t k = 0; k < this->cols(); ++k)
                {
                    sum = sum + ((*this)(i, k) * other(k, j));
                }
                result(i, j) = sum;
            }
        }
        return result;
    }

    // Builds a 3x3 homogeneous transformation matrix from a rotation angle, translation and scale
    static Matrix<3, 3> CreateTransform2D(Number angle, const Vector<2> &translation, const Vector<2> &scale = {N(1.0), N(1.0)})
    {
        Matrix<3, 3> mat = Matrix<3, 3>::Identity();

        Number c = cos(angle);
        Number s = sin(angle);

        // 1. Calculate Rotation/Scale components
        Number r00 = c * scale[0];
        Number r01 = (-s) * scale[1];
        Number r10 = s * scale[0];
        Number r11 = c * scale[1];

        mat(0, 0) = r00;
        mat(0, 1) = r01;
        mat(1, 0) = r10;
        mat(1, 1) = r11;

        // 2. Compensation: Pre-rotate the translation
        // In the (Local * Base) order, the Local rotation/scale is applied to
        // the translation column. To keep the translation constant, we
        // transform the desired translation vector by the rotation/scale matrix.

        mat(0, 2) = (r00 * translation[0]) + (r01 * translation[1]);
        mat(1, 2) = (r10 * translation[0]) + (r11 * translation[1]);

        return mat;
    }

    // Static factory for Identity Matrix
    static Matrix<Rows, Cols> Identity()
    {
        static_assert(Rows == Cols, "Identity matrix must be square.");

        Matrix<Rows, Cols> mat; // Constructor already zero-initialises the buffer

        for (size_t i = 0; i < Rows; ++i)
        {
            mat(i, i) = Number(1);
        }

        return mat;
    }
};

// Free-standing Matrix-Matrix Multiplication operator: Result = Lhs * Rhs
template <size_t R1, size_t C1, size_t C2>
Matrix<R1, C2> operator*(const Matrix<R1, C1> &lhs, const Matrix<C1, C2> &rhs)
{
    return lhs.multiply(rhs);
}

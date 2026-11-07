#pragma once

#include "Vector.h"

namespace csgo
{

struct matrix3x4 {
    constexpr auto operator[](int i) const noexcept { return mat[i]; }
    constexpr auto origin() const noexcept;
    constexpr void translate(const Vector& v) noexcept { mat[0][3] += v.x; mat[1][3] += v.y; mat[2][3] += v.z; }

    // перенос в точку без поворота
    [[nodiscard]] static constexpr matrix3x4 translation(const Vector& v) noexcept
    {
        matrix3x4 m{};
        m.mat[0][0] = m.mat[1][1] = m.mat[2][2] = 1.0f;
        m.mat[0][3] = v.x;
        m.mat[1][3] = v.y;
        m.mat[2][3] = v.z;
        return m;
    }

    // поворот кости вокруг вертикальной оси, проходящей через pivot (cos/sin угла)
    constexpr void rotateYaw(const Vector& pivot, float c, float s) noexcept
    {
        for (int column = 0; column < 4; ++column) {
            float x = mat[0][column];
            float y = mat[1][column];
            if (column == 3) {
                x -= pivot.x;
                y -= pivot.y;
            }
            float nx = c * x - s * y;
            float ny = s * x + c * y;
            if (column == 3) {
                nx += pivot.x;
                ny += pivot.y;
            }
            mat[0][column] = nx;
            mat[1][column] = ny;
        }
    }

    // поворот кости вокруг оси axis (единичный вектор), проходящей через pivot (cos/sin угла)
    constexpr void rotateAxis(const Vector& pivot, const Vector& axis, float c, float s) noexcept
    {
        const float t = 1.0f - c;
        const float x = axis.x, y = axis.y, z = axis.z;
        const float r[3][3]{
            { t * x * x + c,     t * x * y - s * z, t * x * z + s * y },
            { t * x * y + s * z, t * y * y + c,     t * y * z - s * x },
            { t * x * z - s * y, t * y * z + s * x, t * z * z + c },
        };
        for (int column = 0; column < 4; ++column) {
            float v[3]{ mat[0][column], mat[1][column], mat[2][column] };
            if (column == 3) {
                v[0] -= pivot.x;
                v[1] -= pivot.y;
                v[2] -= pivot.z;
            }
            for (int row = 0; row < 3; ++row) {
                float value = r[row][0] * v[0] + r[row][1] * v[1] + r[row][2] * v[2];
                if (column == 3)
                    value += row == 0 ? pivot.x : row == 1 ? pivot.y : pivot.z;
                mat[row][column] = value;
            }
        }
    }
private:
    float mat[3][4];
};

constexpr auto matrix3x4::origin() const noexcept
{
    return Vector{ mat[0][3], mat[1][3], mat[2][3] };
}

}

constexpr auto transform(const csgo::Vector& v, const csgo::matrix3x4& mat) noexcept
{
    return csgo::Vector{ v.dotProduct({ mat[0][0], mat[0][1], mat[0][2] }) + mat[0][3],
                    v.dotProduct({ mat[1][0], mat[1][1], mat[1][2] }) + mat[1][3],
                    v.dotProduct({ mat[2][0], mat[2][1], mat[2][2] }) + mat[2][3] };
}
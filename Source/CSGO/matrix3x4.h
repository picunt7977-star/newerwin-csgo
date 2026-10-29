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
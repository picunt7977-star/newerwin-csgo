#pragma once

#include <algorithm>
#include <array>
#include <cmath>

#include <CSGO/Vector.h>

// Сглаженная позиция чужих игроков для отрисовки: модель, чамсы и ESP берут одно и то же.
// На хвх simtime врагов скачет (DT, дефенсив, фейклаг), и интерполяция движка тащит модель
// то вперёд, то назад. Ведём свою позицию: едем по скорости и мягко подтягиваемся к движку,
// большой скачок (телепорт, респавн, выход из дорманта) — сразу. Рейдж это не трогает.
namespace hvh::smoothing
{

struct State {
    csgo::Vector shown{};
    float at = 0.0f;
};
inline std::array<State, 65> states{};

// Обновляется один раз за кадр (по realtime): модель рисуется несколько раз (чамсы, оружие), ESP — после.
[[nodiscard]] inline csgo::Vector origin(int index, const csgo::Vector& raw, const csgo::Vector& velocity, float realtime) noexcept
{
    if (index <= 0 || index >= static_cast<int>(states.size()))
        return raw;
    auto& state = states[index];
    if (realtime == state.at)
        return state.shown;
    const float dt = std::clamp(realtime - state.at, 0.0f, 0.1f);
    if (state.at <= 0.0f || realtime - state.at > 0.25f || (raw - state.shown).length() > 64.0f) {
        state.shown = raw;
    } else {
        state.shown += velocity * dt;
        state.shown += (raw - state.shown) * (1.0f - std::exp(-dt / 0.1f));
    }
    state.at = realtime;
    return state.shown;
}

}

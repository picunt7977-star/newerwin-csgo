#pragma once

class OtherInterfaces;
struct ImDrawList;

namespace csgo
{
    struct Engine;
    struct Vector;
}

// Визуалы из hysteria: aspect ratio, вьюмодель, свой прицел снайпера, индикатор урона, метки выстрелов,
// замедление, список биндов, LC-индикатор — и трештолк в чат.
namespace hvh::extras
{

// игровой поток (CreateMove): квары aspect ratio и вьюмодели
void update(const OtherInterfaces& interfaces, float frameTime) noexcept;
// рейдж: итог выстрела (точка в мире)
void onShotResult(const csgo::Vector& position, bool hit) noexcept;
// рейдж: мин. урон текущего оружия — для индикатора
void setMinDamage(int value, bool overridden) noexcept;
// player_death: трештолк
void onDeath(const csgo::Engine& engine, int attacker, int victim, int local, bool headshot) noexcept;

// поток рендера
void draw(ImDrawList* drawList) noexcept;

}

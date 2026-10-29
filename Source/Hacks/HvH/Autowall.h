#pragma once

namespace csgo
{
    struct Entity;
    struct Vector;
    struct WeaponInfo;
}

class EngineInterfaces;
class OtherInterfaces;
struct Memory;

namespace hvh::autowall
{

// Урон по цели в точке point с учётом пробития стен (penetrate) и брони.
// 0 — не попадём или не по той цели.
float getDamage(const EngineInterfaces& engineInterfaces, const OtherInterfaces& interfaces, const Memory& memory,
                const csgo::Entity& target, const csgo::Vector& point, const csgo::WeaponInfo* weaponData, bool penetrate) noexcept;

// Урон, который долетит до точки point сквозь стены, без множителя хитгруппы и брони.
// Игроков луч не замечает: для бектрека сервер на время выстрела откатывает цель в запись,
// а не туда, где её модель сейчас у нас. 0 — не пробьёт.
float getDamageToPoint(const EngineInterfaces& engineInterfaces, const OtherInterfaces& interfaces, const csgo::Vector& point,
                       const csgo::WeaponInfo* weaponData, bool penetrate) noexcept;

// множитель хитгруппы + броня/шлем цели
float scaleDamage(const csgo::Entity& target, int hitgroup, const csgo::WeaponInfo* weaponData, float damage) noexcept;

}

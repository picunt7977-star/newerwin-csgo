#include <cmath>
#include <cstring>

#include "Autowall.h"

#include <Interfaces/EngineInterfaces.h>
#include <Interfaces/OtherInterfaces.h>
#include <Memory.h>
#include <CSGO/EngineTrace.h>
#include <CSGO/Entity.h>
#include <CSGO/LocalPlayer.h>
#include <CSGO/PhysicsSurfaceProps.h>
#include <CSGO/Vector.h>
#include <CSGO/WeaponData.h>

namespace hvh::autowall
{

constexpr unsigned MASK_SHOT = 0x4600400B;
constexpr unsigned MASK_SHOT_HULL = 0x600400B;
// MASK_SHOT без хитбоксов и игроков (CONTENTS_HITBOX, CONTENTS_MONSTER): только стены и пропы
constexpr unsigned MASK_SHOT_WORLD = 0x400400B;
constexpr unsigned CONTENTS_HITBOX = 0x40000000;
constexpr unsigned short SURF_NODRAW = 0x0080;
constexpr unsigned short SURF_HITBOX = 0x8000;

static csgo::Vector planeNormal(const csgo::Trace& trace) noexcept
{
    // первые 12 байт после endpos — cplane_t::normal
    csgo::Vector normal;
    std::memcpy(&normal, trace.pad, sizeof(normal));
    return normal;
}

static bool didHit(const csgo::Trace& trace) noexcept
{
    return trace.fraction < 1.0f || trace.allSolid || trace.startSolid;
}

// Переписанный TraceToExit из движка: ищем, где пуля выходит из препятствия
static bool traceToExit(const csgo::EngineTrace& engineTrace, const csgo::Trace& enterTrace, const csgo::Vector& start, const csgo::Vector& direction, csgo::Vector& end, csgo::Trace& exitTrace) noexcept
{
    int firstContents = 0;

    for (float distance = 0.0f; distance <= 90.0f;) {
        distance += 4.0f;
        end = start + direction * distance;

        const int contents = engineTrace.getPointContents(end, MASK_SHOT_HULL | CONTENTS_HITBOX);
        if (!firstContents)
            firstContents = contents;

        if ((contents & MASK_SHOT_HULL) && (!(contents & CONTENTS_HITBOX) || contents == firstContents))
            continue;

        const auto newEnd = end - direction * 4.0f;
        engineTrace.traceRay({ end, newEnd }, MASK_SHOT_HULL | CONTENTS_HITBOX, { nullptr }, exitTrace);

        if (exitTrace.startSolid && (exitTrace.surface.flags & SURF_HITBOX)) {
            engineTrace.traceRay({ end, start }, MASK_SHOT_HULL, { exitTrace.entity }, exitTrace);
            if (didHit(exitTrace) && !exitTrace.startSolid) {
                end = exitTrace.endpos;
                return true;
            }
            continue;
        }

        if (didHit(exitTrace) && !exitTrace.startSolid) {
            if ((enterTrace.surface.flags & SURF_NODRAW) || (!(exitTrace.surface.flags & SURF_NODRAW) && planeNormal(exitTrace).dotProduct(direction) <= 1.0f)) {
                end = end - direction * (exitTrace.fraction * 4.0f);
                return true;
            }
            continue;
        }
    }
    return false;
}

static bool handleBulletPenetration(const csgo::EngineTrace& engineTrace, const OtherInterfaces& interfaces, const csgo::SurfaceData* enterSurfaceData, const csgo::Trace& enterTrace, const csgo::Vector& direction, csgo::Vector& start, float penetration, float& damage) noexcept
{
    csgo::Vector end;
    csgo::Trace exitTrace;

    if (!traceToExit(engineTrace, enterTrace, enterTrace.endpos, direction, end, exitTrace))
        return false;

    const auto exitSurfaceData = interfaces.getPhysicsSurfaceProps().getSurfaceData(exitTrace.surface.surfaceProps);
    if (!exitSurfaceData)
        return false;

    float damageModifier = 0.16f;
    float penetrationModifier = (enterSurfaceData->penetrationmodifier + exitSurfaceData->penetrationmodifier) / 2.0f;

    // 71 — стекло, 89 — решётка
    if (enterSurfaceData->material == 71 || enterSurfaceData->material == 89) {
        damageModifier = 0.05f;
        penetrationModifier = 3.0f;
    } else if ((enterTrace.contents >> 3 & 1) || (enterTrace.surface.flags >> 7 & 1)) {
        penetrationModifier = 1.0f;
    }

    if (enterSurfaceData->material == exitSurfaceData->material) {
        if (exitSurfaceData->material == 85 || exitSurfaceData->material == 87)
            penetrationModifier = 3.0f;
        else if (exitSurfaceData->material == 76)
            penetrationModifier = 2.0f;
    }

    const float thickness = (exitTrace.endpos - enterTrace.endpos).squareLength();
    damage -= 11.25f / penetration / penetrationModifier + damage * damageModifier + thickness / 24.0f / penetrationModifier;

    start = exitTrace.endpos;
    return damage > 0.0f;
}

float scaleDamage(const csgo::Entity& target, int hitgroup, const csgo::WeaponInfo* weaponData, float damage) noexcept
{
    damage *= hitgroup == HitGroup::Head ? weaponData->headshotMultiplier : HitGroup::getDamageMultiplier(hitgroup);

    if (HitGroup::isArmored(hitgroup, target.hasHelmet()) && target.armor() > 0) {
        const float armorRatio = weaponData->armorRatio / 2.0f;
        float newDamage = damage * armorRatio;
        if ((damage - newDamage) * 0.5f > target.armor())
            newDamage = damage - target.armor() * 2.0f;
        damage = newDamage;
    }
    return damage;
}

float getDamage(const EngineInterfaces& engineInterfaces, const OtherInterfaces& interfaces, const Memory& memory, const csgo::Entity& target, const csgo::Vector& point, const csgo::WeaponInfo* weaponData, bool penetrate) noexcept
{
    if (!localPlayer || !weaponData)
        return 0.0f;

    const auto& engineTrace = engineInterfaces.engineTrace();

    csgo::Vector start = localPlayer.get().getEyePosition();
    auto direction = point - start;
    const float totalDistance = direction.length();
    if (totalDistance <= 0.0f)
        return 0.0f;
    direction /= totalDistance;

    float damage = static_cast<float>(weaponData->damage);
    float traveled = 0.0f;
    int hitsLeft = penetrate ? 4 : 1;

    while (damage >= 1.0f && hitsLeft > 0) {
        const float remaining = weaponData->range - traveled;
        const auto end = start + direction * remaining;

        csgo::Trace trace;
        engineTrace.traceRay({ start, end }, MASK_SHOT, { localPlayer.get().getPOD() }, trace);

        traveled += trace.fraction * remaining;
        damage *= std::pow(weaponData->rangeModifier, traveled / 500.0f);

        if (trace.fraction == 1.0f)
            return 0.0f;

        if (trace.entity == target.getPOD() && trace.hitgroup > HitGroup::Generic && trace.hitgroup <= HitGroup::RightLeg)
            return scaleDamage(target, trace.hitgroup, weaponData, damage);

        // упёрлись в другого игрока — дальше не считаем
        if (trace.entity && trace.entity != target.getPOD() && trace.hitgroup > HitGroup::Generic)
            return 0.0f;

        if (!penetrate)
            return 0.0f;

        const auto surfaceData = interfaces.getPhysicsSurfaceProps().getSurfaceData(trace.surface.surfaceProps);
        if (!surfaceData || surfaceData->penetrationmodifier < 0.1f)
            return 0.0f;

        if (!handleBulletPenetration(engineTrace, interfaces, surfaceData, trace, direction, start, weaponData->penetration, damage))
            return 0.0f;

        --hitsLeft;
    }
    return 0.0f;
}

float getDamageToPoint(const EngineInterfaces& engineInterfaces, const OtherInterfaces& interfaces, const csgo::Vector& point, const csgo::WeaponInfo* weaponData, bool penetrate) noexcept
{
    if (!localPlayer || !weaponData)
        return 0.0f;

    const auto& engineTrace = engineInterfaces.engineTrace();

    const auto eye = localPlayer.get().getEyePosition();
    csgo::Vector start = eye;
    auto direction = point - start;
    const float totalDistance = direction.length();
    if (totalDistance <= 0.0f)
        return 0.0f;
    direction /= totalDistance;

    float damage = static_cast<float>(weaponData->damage);
    float traveled = 0.0f;
    int hitsLeft = penetrate ? 4 : 1;

    while (damage >= 1.0f && hitsLeft > 0) {
        // сколько осталось до точки по прямой (после пробития start уже за стеной)
        const float remaining = totalDistance - (start - eye).length();
        if (remaining <= 0.0f)
            return damage;

        csgo::Trace trace;
        engineTrace.traceRay({ start, start + direction * remaining }, MASK_SHOT_WORLD, { localPlayer.get().getPOD() }, trace);

        traveled += trace.fraction * remaining;
        damage *= std::pow(weaponData->rangeModifier, traveled / 500.0f);

        // долетели до точки, не упёршись в стену
        if (trace.fraction == 1.0f)
            return damage;

        if (!penetrate)
            return 0.0f;

        const auto surfaceData = interfaces.getPhysicsSurfaceProps().getSurfaceData(trace.surface.surfaceProps);
        if (!surfaceData || surfaceData->penetrationmodifier < 0.1f)
            return 0.0f;

        if (!handleBulletPenetration(engineTrace, interfaces, surfaceData, trace, direction, start, weaponData->penetration, damage))
            return 0.0f;

        --hitsLeft;
    }
    return 0.0f;
}

}

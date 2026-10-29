#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <numbers>
#include <vector>

#include <imgui/imgui.h>

#include "GrenadePrediction.h"
#include "HvhConfig.h"
#include "HvhGui.h"

#include <GameData.h>
#include <Helpers.h>
#include <Memory.h>
#include <Netvars.h>
#include <Interfaces/EngineInterfaces.h>
#include <CSGO/Entity.h>
#include <CSGO/EngineTrace.h>
#include <CSGO/GlobalVars.h>
#include <CSGO/LocalPlayer.h>
#include <CSGO/WeaponId.h>

namespace hvh::grenade
{

namespace
{

// MASK_SOLID | CONTENTS_CURRENT_90 — как у самой гранаты
constexpr unsigned MASK_GRENADE = 0x200400B | 0x40000;

struct Path {
    std::vector<csgo::Vector> points;
    std::vector<csgo::Vector> bounces;
    csgo::Vector end;
    WeaponId weapon{};
    bool valid = false;
};

std::mutex mutex;
Path shared;

[[nodiscard]] csgo::Vector angleForward(float pitch, float yaw) noexcept
{
    const float p = Helpers::deg2rad(pitch);
    const float y = Helpers::deg2rad(yaw);
    return { std::cos(p) * std::cos(y), std::cos(p) * std::sin(y), -std::sin(p) };
}

[[nodiscard]] csgo::Vector planeNormal(const csgo::Trace& trace) noexcept
{
    // cplane_t лежит сразу после endpos: normal, dist, type, signbits
    csgo::Vector normal;
    std::memcpy(&normal, trace.pad, sizeof(normal));
    return normal;
}

[[nodiscard]] bool isGrenade(WeaponId id) noexcept
{
    switch (id) {
    case WeaponId::Flashbang:
    case WeaponId::HeGrenade:
    case WeaponId::SmokeGrenade:
    case WeaponId::Molotov:
    case WeaponId::Decoy:
    case WeaponId::IncGrenade:
        return true;
    default:
        return false;
    }
}

[[nodiscard]] float dot(const csgo::Vector& a, const csgo::Vector& b) noexcept
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

void simulate(const EngineInterfaces& engineInterfaces, const Memory& memory, WeaponId weapon, float strength, Path& path) noexcept
{
    const auto engineTrace = engineInterfaces.engineTrace();
    const auto local = localPlayer.get().getPOD();

    // как в CBaseCSGrenade::ThrowGrenade
    auto angles = engineInterfaces.getEngine().getViewAngles();
    float pitch = angles.x;
    if (pitch < -90.0f)
        pitch += 360.0f;
    else if (pitch > 90.0f)
        pitch -= 360.0f;
    pitch -= (90.0f - std::abs(pitch)) * 10.0f / 90.0f;

    const auto forward = angleForward(pitch, angles.y);
    const float speed = std::clamp(750.0f * 0.9f, 15.0f, 750.0f) * (strength * 0.7f + 0.3f);

    auto src = localPlayer.get().getAbsOrigin() + localPlayer.get().viewOffset();
    src.z += strength * 12.0f - 12.0f;

    csgo::Trace trace;
    engineTrace.traceRay({ src, src + forward * 22.0f }, MASK_GRENADE, { local }, trace);
    src = trace.endpos - forward * 6.0f;

    auto velocity = localPlayer.get().velocity() * 1.25f + forward * speed;

    const float interval = memory.globalVars->intervalPerTick;
    const bool fused = weapon == WeaponId::HeGrenade || weapon == WeaponId::Flashbang;
    const bool molotov = weapon == WeaponId::Molotov || weapon == WeaponId::IncGrenade;
    const float lifetime = fused ? 1.5f : molotov ? 2.0f : 8.0f;
    const int maxTicks = static_cast<int>(lifetime / interval);
    constexpr float gravity = 800.0f * 0.4f;

    path.points.clear();
    path.bounces.clear();
    path.points.push_back(src);

    float stillTime = 0.0f;
    for (int tick = 0; tick < maxTicks; ++tick) {
        const float newZ = velocity.z - gravity * interval;
        const csgo::Vector move{ velocity.x * interval, velocity.y * interval, (velocity.z + newZ) * 0.5f * interval };
        velocity.z = newZ;

        engineTrace.traceRay({ src, src + move }, MASK_GRENADE, { local }, trace);
        src = trace.endpos;
        if (tick % 2 == 0 || trace.fraction != 1.0f)
            path.points.push_back(src);

        if (trace.fraction != 1.0f && !trace.startSolid) {
            const auto normal = planeNormal(trace);
            if (molotov && normal.z > 0.7f)
                break;  // молотов разбивается об пол

            // отражение: PhysicsClipVelocity с overbounce 2 и упругость 0.45
            const float backoff = dot(velocity, normal) * 2.0f;
            velocity = (velocity - normal * backoff) * 0.45f;
            path.bounces.push_back(src);

            if (normal.z > 0.7f && dot(velocity, velocity) < 20.0f * 20.0f)
                velocity = {};
        }

        // дым и декой срабатывают, когда граната остановилась
        if (!fused && !molotov) {
            stillTime = dot(velocity, velocity) < 0.01f ? stillTime + interval : 0.0f;
            if (stillTime >= 0.2f)
                break;
        }
    }

    path.end = src;
    path.points.push_back(src);
    path.weapon = weapon;
    path.valid = true;
}

}

void update(const EngineInterfaces& engineInterfaces, const Memory& memory) noexcept
{
    Path path;
    if (config.view.grenadePrediction && localPlayer && localPlayer.get().isAlive()) {
        const auto weapon = csgo::Entity::from(retSpoofGadgets->client, localPlayer.get().getActiveWeapon());
        if (weapon.getPOD() && isGrenade(weapon.itemDefinitionIndex())
            && Netvars::get(fnv::hash("CBasePlayer->m_vecViewOffset[0]"))) {
            // показываем всё время, пока граната в руке; сила броска — по зажатым кнопкам (ЛКМ 1, ПКМ 0, обе 0.5)
            float strength = 1.0f;
            if (Netvars::get(fnv::hash("CBaseCSGrenade->m_bPinPulled")) && weapon.pinPulled() && Netvars::get(fnv::hash("CBaseCSGrenade->m_flThrowStrength")))
                strength = std::clamp(weapon.throwStrength(), 0.0f, 1.0f);
            simulate(engineInterfaces, memory, weapon.itemDefinitionIndex(), strength, path);
        }
    }

    std::scoped_lock lock{ mutex };
    shared = std::move(path);
}

void draw(ImDrawList* drawList) noexcept
{
    Path path;
    {
        std::scoped_lock lock{ mutex };
        if (!shared.valid)
            return;
        path = shared;
    }

    // кубик в мире: 8 вершин -> 12 рёбер
    const auto cube = [drawList](const csgo::Vector& center, float half, ImU32 col) {
        ImVec2 v[8];
        for (int i = 0; i < 8; ++i) {
            const csgo::Vector corner{ center.x + ((i & 1) ? half : -half), center.y + ((i & 2) ? half : -half), center.z + ((i & 4) ? half : -half) };
            if (!Helpers::worldToScreen(corner, v[i]))
                return;
        }
        static constexpr int edges[12][2]{ { 0, 1 }, { 1, 3 }, { 3, 2 }, { 2, 0 }, { 4, 5 }, { 5, 7 }, { 7, 6 }, { 6, 4 }, { 0, 4 }, { 1, 5 }, { 2, 6 }, { 3, 7 } };
        for (const auto& [a, b] : edges)
            drawList->AddLine(v[a], v[b], col, 1.0f);
    };

    GameData::Lock lock;
    ImVec2 previous;
    bool havePrevious = false;
    for (const auto& point : path.points) {
        ImVec2 p;
        if (!Helpers::worldToScreen(point, p)) {
            havePrevious = false;
            continue;
        }
        if (havePrevious)
            drawList->AddLine(previous, p, IM_COL32(255, 255, 255, 230), 1.5f);
        previous = p;
        havePrevious = true;
    }
    for (const auto& point : path.bounces)
        cube(point, 2.0f, IM_COL32(255, 255, 255, 230));
    cube(path.end, 3.0f, ui::color::accent);
}

}

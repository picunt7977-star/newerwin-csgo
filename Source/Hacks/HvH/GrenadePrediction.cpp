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
#include <CSGO/UserCmd.h>
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

// ---- супертосс ----
// Бросок (CBaseCSGrenade::ThrowGrenade): pitch' = pitch − (90 − |pitch|)·10/90, скорость гранаты
// v = forward(pitch', yaw) · s + скорость_игрока · 1.25, где s = clamp(750·0.9) · (0.3 + 0.7·сила).
// Хотим, чтобы v смотрел туда же, куда forward прицела d: ищем единичный f и k > 0, что f·s + w = k·d.
// |k·d − w| = s  →  k = d·w + sqrt((d·w)² − |w|² + s²). Решения нет (своя скорость больше броска и
// направлена не туда — слабый бросок ПКМ на бегу/в прыжке) — тогда супертосс не трогает бросок.

[[nodiscard]] float throwPitch(float pitch) noexcept
{
    if (pitch < -90.0f)
        pitch += 360.0f;
    else if (pitch > 90.0f)
        pitch -= 360.0f;
    return pitch - (90.0f - std::abs(pitch)) * 10.0f / 90.0f;
}

// обратное к throwPitch на [-90, 90]: при pitch ≥ 0 p' = (10p − 90)/9, при pitch < 0 p' = (8p − 90)/9
[[nodiscard]] float inverseThrowPitch(float adjusted) noexcept
{
    return adjusted >= -10.0f ? (9.0f * adjusted + 90.0f) / 10.0f : (9.0f * adjusted + 90.0f) / 8.0f;
}

[[nodiscard]] float throwSpeed(float strength) noexcept
{
    return std::clamp(750.0f * 0.9f, 15.0f, 750.0f) * (strength * 0.7f + 0.3f);
}

// k — итоговая скорость гранаты вдоль d; ≤ 0 — решения нет
[[nodiscard]] float tossScale(const csgo::Vector& d, const csgo::Vector& w, float speed) noexcept
{
    const float dw = d.x * w.x + d.y * w.y + d.z * w.z;
    const float disc = dw * dw - (w.x * w.x + w.y * w.y + w.z * w.z) + speed * speed;
    return disc >= 0.0f ? dw + std::sqrt(disc) : -1.0f;
}

void simulate(const EngineInterfaces& engineInterfaces, const Memory& memory, WeaponId weapon, float strength, Path& path) noexcept
{
    const auto engineTrace = engineInterfaces.engineTrace();
    const auto local = localPlayer.get().getPOD();

    // как в CBaseCSGrenade::ThrowGrenade
    auto angles = engineInterfaces.getEngine().getViewAngles();
    const float pitch = throwPitch(angles.x);

    const auto forward = angleForward(pitch, angles.y);
    const float speed = throwSpeed(strength);

    auto src = localPlayer.get().getAbsOrigin() + localPlayer.get().viewOffset();
    src.z += strength * 12.0f - 12.0f;

    csgo::Trace trace;
    engineTrace.traceRay({ src, src + forward * 22.0f }, MASK_GRENADE, { local }, trace);
    src = trace.endpos - forward * 6.0f;

    auto velocity = localPlayer.get().velocity() * 1.25f + forward * speed;
    // супертосс: граната идёт ровно по прицелу, своя скорость меняет только дальность
    if (config.movement.superToss) {
        if (const float k = tossScale(forward, localPlayer.get().velocity() * 1.25f, speed); k > 1.0f)
            velocity = forward * k;
    }

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
                velocity = csgo::Vector{};
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

bool superToss(csgo::UserCmd* cmd, const Memory& memory) noexcept
{
    if (!config.movement.superToss || !localPlayer || !localPlayer.get().isAlive())
        return false;
    const auto weapon = csgo::Entity::from(retSpoofGadgets->client, localPlayer.get().getActiveWeapon());
    if (!weapon.getPOD() || !isGrenade(weapon.itemDefinitionIndex()))
        return false;
    if (!Netvars::get(fnv::hash("CBaseCSGrenade->m_fThrowTime")) || !Netvars::get(fnv::hash("CBaseCSGrenade->m_flThrowStrength")))
        return false;
    // бросок происходит в ItemPostFrame, когда curtime дошёл до m_fThrowTime. Тик в тик по тикбейсу
    // не угадать (предсказание могло уже прибавить тик) — подменяем угол в обоих кандидатах: граната
    // берёт угол один раз, лишний тик ни на что не влияет
    const float throwTime = weapon.throwTime();
    const float interval = memory.globalVars->intervalPerTick;
    if (throwTime <= 0.0f || memory.globalVars->serverTime() + interval < throwTime)
        return false;

    const float strength = std::clamp(weapon.throwStrength(), 0.0f, 1.0f);
    const float speed = throwSpeed(strength);
    // куда целимся — угол камеры (антиаим во время броска углы не трогает, но берём из команды до него)
    const auto d = angleForward(throwPitch(cmd->viewangles.x), cmd->viewangles.y);
    const auto w = localPlayer.get().velocity() * 1.25f;
    if (w.x * w.x + w.y * w.y + w.z * w.z < 1.0f)
        return false;   // стоим — компенсировать нечего
    const float k = tossScale(d, w, speed);
    if (k <= 1.0f)
        return false;   // решения нет — бросаем как есть
    auto f = d * k - w;
    const float length = std::sqrt(f.x * f.x + f.y * f.y + f.z * f.z);
    if (length < 0.001f)
        return false;
    f = f * (1.0f / length);

    const float adjusted = Helpers::rad2deg(-std::asin(std::clamp(f.z, -1.0f, 1.0f)));
    cmd->viewangles.x = std::clamp(inverseThrowPitch(adjusted), -89.0f, 89.0f);
    cmd->viewangles.y = Helpers::rad2deg(std::atan2(f.y, f.x));
    return true;
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

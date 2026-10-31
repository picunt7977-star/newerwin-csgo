#include <algorithm>
#include <array>
#include <atomic>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

#include <imgui/imgui.h>

#include "HvhConfig.h"
#include "HvhGui.h"
#include "HvhVisuals.h"
#include "Ragebot.h"

#include <GameData.h>
#include <Helpers.h>
#include <Memory.h>
#include <fnv.h>
#include <Netvars.h>
#include <Interfaces/ClientInterfaces.h>
#include <CSGO/ClientClass.h>
#include <CSGO/Constants/ClassId.h>
#include <CSGO/Entity.h>
#include <CSGO/EntityList.h>
#include <CSGO/GlobalVars.h>
#include <CSGO/LocalPlayer.h>
#include <CSGO/UserCmd.h>
#include <CSGO/WeaponData.h>
#include <RetSpoof/RetSpoofGadgets.h>

namespace hvh::visuals
{

namespace
{

std::atomic<float> spreadCone = 0.0f;   // inaccuracy + spread своего оружия, радианы (≈ тангенс)

struct DamageEvent {
    int index;
    int damage;
    bool headshot;
};
std::mutex eventMutex;
std::vector<DamageEvent> pendingDamage;
bool pendingKill = false;

// всплывающая цифра урона: позиция берётся у игрока в первый кадр показа
struct DamageNumber {
    csgo::Vector position;
    int damage;
    bool headshot;
    double at;
};
std::vector<DamageNumber> damageNumbers;
double killAt = -10.0;

struct Ring {
    csgo::Vector position;
    double at;
};
std::vector<Ring> rings;
std::array<double, 65> lastRing{};

// дым и огонь в мире: собираем в игровом потоке, рисуем в потоке рендера
struct GrenadeTimer {
    csgo::Vector center;
    float radius;       // зона на земле, юниты
    float endsAt;       // realtime, когда погаснет
    float duration;
    bool smoke;
};
std::mutex grenadeMutex;
std::vector<GrenadeTimer> grenadeTimers;

constexpr float smokeDuration = 18.0f;    // дым держится ~18 с с момента раскрытия
constexpr float fireDuration = 7.0f;      // inferno_flame_lifetime
constexpr float smokeRadius = 144.0f;

[[nodiscard]] ImU32 withAlpha(ImU32 color, float alpha) noexcept
{
    const auto a = static_cast<unsigned>(std::clamp(alpha, 0.0f, 1.0f) * static_cast<float>((color >> IM_COL32_A_SHIFT) & 0xFF));
    return (color & ~IM_COL32_A_MASK) | (a << IM_COL32_A_SHIFT);
}

// текст с чёрной тенью — читается на любом фоне
void shadowText(ImDrawList* drawList, ImVec2 position, ImU32 color, const char* text) noexcept
{
    ImFont* font = ui::fonts.menu;
    const float size = font ? font->FontSize : ImGui::GetFontSize();
    drawList->AddText(font, size, { position.x + 1.0f, position.y + 1.0f }, withAlpha(IM_COL32(0, 0, 0, 255), ((color >> IM_COL32_A_SHIFT) & 0xFF) / 255.0f), text);
    drawList->AddText(font, size, position, color, text);
}

[[nodiscard]] ImVec2 textSize(const char* text) noexcept
{
    ImFont* font = ui::fonts.menu;
    if (!font)
        return ImGui::CalcTextSize(text);
    return font->CalcTextSizeA(font->FontSize, FLT_MAX, 0.0f, text);
}

// экранная рамка игрока по его коробке (как у ESP)
[[nodiscard]] bool screenBox(const PlayerData& player, ImVec2& min, ImVec2& max) noexcept
{
    min = { FLT_MAX, FLT_MAX };
    max = { -FLT_MAX, -FLT_MAX };
    for (int i = 0; i < 8; ++i) {
        const csgo::Vector corner{ i & 1 ? player.obbMaxs.x : player.obbMins.x, i & 2 ? player.obbMaxs.y : player.obbMins.y, i & 4 ? player.obbMaxs.z : player.obbMins.z };
        ImVec2 screen;
        if (!Helpers::worldToScreen(corner + player.origin, screen))
            return false;
        min = ImMin(min, screen);
        max = ImMax(max, screen);
    }
    return true;
}

void drawFlags(ImDrawList* drawList, const PlayerData& player, float realtime) noexcept
{
    const int flags = config.view.espFlags;
    if (!flags || !player.enemy || !player.alive || player.dormant)
        return;
    ImVec2 min, max;
    if (!screenBox(player, min, max))
        return;

    const auto view = ragebot::targetView(player.index);
    const bool fresh = realtime - view.updatedAt < 0.5f;
    float y = min.y;
    const auto flag = [&](ImU32 color, const char* text) {
        shadowText(drawList, { max.x + 4.0f, y }, color, text);
        y += textSize(text).y;
    };
    char buffer[32];
    if ((flags & FlagArmor) && player.armor > 0)
        flag(IM_COL32(230, 230, 230, 255), player.helmet ? "HK" : "K");
    if (flags & FlagMoney) {
        std::snprintf(buffer, sizeof(buffer), "$%d", player.money);
        flag(IM_COL32(150, 210, 90, 255), buffer);
    }
    if ((flags & FlagKit) && player.defuser)
        flag(IM_COL32(90, 170, 255, 255), player.defusing ? "ДЕФЬЮЗ" : "KIT");
    if ((flags & FlagScoped) && player.scoped)
        flag(IM_COL32(120, 200, 255, 255), "ZOOM");
    if ((flags & FlagReload) && player.reloading)
        flag(IM_COL32(255, 200, 90, 255), "RELOAD");
    if ((flags & FlagFlashed) && player.flashDuration > 0.5f)
        flag(IM_COL32(255, 255, 140, 255), "FLASH");
    if ((flags & FlagBomb) && player.hasBomb)
        flag(IM_COL32(255, 90, 90, 255), "C4");
    if (fresh && (flags & FlagDoubleTap) && view.charged)
        flag(IM_COL32(255, 120, 200, 255), "DT");
    if (fresh && (flags & FlagLagComp) && view.breakingLagComp)
        flag(IM_COL32(255, 80, 80, 255), "LC");
    if (fresh && (flags & FlagResolver) && view.resolver && view.resolver[0]) {
        std::snprintf(buffer, sizeof(buffer), "R: %s", view.resolver);
        flag(IM_COL32(200, 200, 200, 220), buffer);
    }
    if (fresh && (flags & FlagLethal) && realtime - view.aimAt < 0.3f && view.aimLethal)
        flag(IM_COL32(255, 60, 60, 255), "LETHAL");
}

// Флаги в предпросмотре меню: пример со всеми включёнными флагами (значения выдуманные)
void drawPreviewFlagsImpl(ImDrawList* drawList, ImVec2 position) noexcept
{
    const int flags = config.view.espFlags;
    const auto flag = [&](int bit, ImU32 color, const char* text) {
        if (!(flags & bit))
            return;
        shadowText(drawList, position, color, text);
        position.y += textSize(text).y;
    };
    flag(FlagArmor, IM_COL32(230, 230, 230, 255), "HK");
    flag(FlagMoney, IM_COL32(150, 210, 90, 255), "$4750");
    flag(FlagKit, IM_COL32(90, 170, 255, 255), "KIT");
    flag(FlagScoped, IM_COL32(120, 200, 255, 255), "ZOOM");
    flag(FlagReload, IM_COL32(255, 200, 90, 255), "RELOAD");
    flag(FlagFlashed, IM_COL32(255, 255, 140, 255), "FLASH");
    flag(FlagBomb, IM_COL32(255, 90, 90, 255), "C4");
    flag(FlagDoubleTap, IM_COL32(255, 120, 200, 255), "DT");
    flag(FlagLagComp, IM_COL32(255, 80, 80, 255), "LC");
    flag(FlagResolver, IM_COL32(200, 200, 200, 220), "R: слои");
    flag(FlagLethal, IM_COL32(255, 60, 60, 255), "LETHAL");
}

void drawAimDot(ImDrawList* drawList, const PlayerData& player, float realtime) noexcept
{
    if (!config.view.aimDot || !player.enemy || !player.alive || player.dormant)
        return;
    const auto view = ragebot::targetView(player.index);

    // Точки мультипоинта, куда рейдж может выстрелить: зелёные убивают, жёлтые — урон не ниже мин.
    if (const float age = realtime - view.pointsAt; age >= 0.0f && age < 0.3f) {
        const float alpha = 1.0f - age / 0.3f * 0.5f;
        for (int k = 0; k < view.pointCount; ++k) {
            const int state = view.pointState[k];
            if (state == 0)
                continue;
            ImVec2 screen;
            if (!Helpers::worldToScreen({ view.points[k][0], view.points[k][1], view.points[k][2] }, screen))
                continue;
            const ImU32 color = state == 2 ? IM_COL32(90, 255, 120, 255) : IM_COL32(255, 220, 80, 255);
            drawList->AddCircleFilled(screen, 6.0f, withAlpha(color, alpha * 0.18f));   // свечение
            drawList->AddCircleFilled(screen, 3.4f, withAlpha(IM_COL32(0, 0, 0, 255), alpha));
            drawList->AddCircleFilled(screen, 2.5f, withAlpha(color, alpha));
        }
    }

    // выбранная рейджем точка — кольцо; красное — сюда выстрелили
    const float age = realtime - view.aimAt;
    if (age > 0.25f)
        return;
    ImVec2 screen;
    if (!Helpers::worldToScreen({ view.aimPoint[0], view.aimPoint[1], view.aimPoint[2] }, screen))
        return;
    const float alpha = 1.0f - age / 0.25f * 0.6f;
    const ImU32 color = view.aimedNow ? IM_COL32(255, 70, 70, 255) : view.aimLethal ? IM_COL32(90, 255, 120, 255) : IM_COL32(255, 220, 80, 255);
    drawList->AddCircle(screen, 7.0f, withAlpha(IM_COL32(0, 0, 0, 255), alpha * 0.8f), 16, 3.0f);
    drawList->AddCircle(screen, 7.0f, withAlpha(color, alpha), 16, 1.5f);
}

void drawShotSkeleton(ImDrawList* drawList, float realtime) noexcept
{
    if (!config.view.shotCapsules)
        return;
    const auto shot = ragebot::shotView();
    const float age = realtime - shot.at;
    if (shot.at <= 0.0f || age > 2.5f || age < 0.0f)
        return;
    const float alpha = 1.0f - age / 2.5f;
    const ImU32 color = shot.result > 0 ? IM_COL32(90, 255, 120, 255) : shot.result < 0 ? IM_COL32(255, 80, 80, 255) : IM_COL32(230, 230, 230, 255);
    for (int k = 0; k < shot.count; ++k) {
        ImVec2 sa, sb;
        if (!Helpers::worldToScreen({ shot.a[k][0], shot.a[k][1], shot.a[k][2] }, sa)
            || !Helpers::worldToScreen({ shot.b[k][0], shot.b[k][1], shot.b[k][2] }, sb))
            continue;
        drawList->AddLine(sa, sb, withAlpha(IM_COL32(0, 0, 0, 255), alpha * 0.8f), 3.0f);
        drawList->AddLine(sa, sb, withAlpha(color, alpha), 1.5f);
    }
}

void drawDamageNumbers(ImDrawList* drawList) noexcept
{
    const double now = ImGui::GetTime();
    {
        std::scoped_lock lock{ eventMutex };
        for (const auto& event : pendingDamage) {
            for (const auto& player : GameData::players()) {
                if (player.index != event.index)
                    continue;
                const csgo::Vector position{ player.origin.x, player.origin.y, player.origin.z + (event.headshot ? 72.0f : 50.0f) };
                damageNumbers.push_back({ position, event.damage, event.headshot, now });
                break;
            }
        }
        pendingDamage.clear();
        if (pendingKill) {
            pendingKill = false;
            killAt = now;
        }
    }
    std::erase_if(damageNumbers, [now](const DamageNumber& number) { return now - number.at > 1.2; });
    if (!config.view.damageNumbers)
        return;
    for (const auto& number : damageNumbers) {
        const float t = static_cast<float>(now - number.at) / 1.2f;
        ImVec2 screen;
        if (!Helpers::worldToScreen(number.position, screen))
            continue;
        screen.y -= t * 40.0f;   // всплывает вверх и гаснет
        char text[16];
        std::snprintf(text, sizeof(text), "-%d", number.damage);
        const ImU32 color = number.headshot ? IM_COL32(255, 70, 70, 255) : IM_COL32(255, 230, 120, 255);
        const auto size = textSize(text);
        shadowText(drawList, { screen.x - size.x * 0.5f, screen.y }, withAlpha(color, 1.0f - t * t), text);
    }
}

void drawKillFlash(ImDrawList* drawList) noexcept
{
    if (!config.view.killFlash)
        return;
    const float t = static_cast<float>(ImGui::GetTime() - killAt) / 0.45f;
    if (t < 0.0f || t > 1.0f)
        return;
    const auto size = ImGui::GetIO().DisplaySize;
    const float edge = std::min(size.x, size.y) * 0.12f;
    const ImU32 inner = withAlpha(ui::color::accent, 0.0f);
    const ImU32 outer = withAlpha(ui::color::accent, (1.0f - t) * 0.55f);
    drawList->AddRectFilledMultiColor({ 0.0f, 0.0f }, { size.x, edge }, outer, outer, inner, inner);
    drawList->AddRectFilledMultiColor({ 0.0f, size.y - edge }, size, inner, inner, outer, outer);
    drawList->AddRectFilledMultiColor({ 0.0f, 0.0f }, { edge, size.y }, outer, inner, inner, outer);
    drawList->AddRectFilledMultiColor({ size.x - edge, 0.0f }, size, inner, outer, outer, inner);
}

void drawSpreadCircle(ImDrawList* drawList) noexcept
{
    if (!config.view.spreadCircle || !GameData::local().exists || !GameData::local().alive)
        return;
    const float cone = spreadCone.load();
    if (cone <= 0.0f)
        return;
    // fov в CS:GO задан для 4:3: половина высоты экрана = tan(fov/2) * 0.75
    const int fov = GameData::local().fov > 0 ? GameData::local().fov : 90;
    const auto size = ImGui::GetIO().DisplaySize;
    const float radius = std::tan(cone) / (std::tan(Helpers::deg2rad(static_cast<float>(fov) * 0.5f)) * 0.75f) * size.y * 0.5f;
    if (radius < 1.0f || radius > size.y)
        return;
    const ImVec2 center{ size.x * 0.5f, size.y * 0.5f };
    drawList->AddCircleFilled(center, radius, withAlpha(ui::color::accent, 0.08f), 48);
    drawList->AddCircle(center, radius, withAlpha(ui::color::accent, 0.7f), 48, 1.0f);
}

void drawFootsteps(ImDrawList* drawList) noexcept
{
    const double now = ImGui::GetTime();
    if (config.view.footsteps) {
        for (const auto& player : GameData::players()) {
            if (!player.enemy || !player.alive || !player.audible || player.index <= 0 || player.index >= 65)
                continue;
            if (now - lastRing[player.index] < 0.45)
                continue;
            lastRing[player.index] = now;
            rings.push_back({ player.origin, now });
        }
    }
    std::erase_if(rings, [now](const Ring& ring) { return now - ring.at > 0.9; });
    if (!config.view.footsteps)
        return;
    constexpr int segments = 32;
    for (const auto& ring : rings) {
        const float t = static_cast<float>(now - ring.at) / 0.9f;
        const float radius = 8.0f + t * 40.0f;
        ImVec2 points[segments];
        int count = 0;
        for (int i = 0; i < segments; ++i) {
            const float angle = 2.0f * 3.14159265f * static_cast<float>(i) / segments;
            if (!Helpers::worldToScreen({ ring.position.x + std::cos(angle) * radius, ring.position.y + std::sin(angle) * radius, ring.position.z + 2.0f }, points[count]))
                break;
            ++count;
        }
        if (count == segments)
            drawList->AddPolyline(points, count, withAlpha(IM_COL32(255, 120, 120, 255), (1.0f - t) * 0.9f), ImDrawFlags_Closed, 1.5f);
    }
}

void drawGrenadeTimers(ImDrawList* drawList, float realtime) noexcept
{
    if (!config.view.grenadeTimers)
        return;
    std::scoped_lock lock{ grenadeMutex };
    constexpr int segments = 48;
    for (const auto& timer : grenadeTimers) {
        const float left = timer.endsAt - realtime;
        if (left <= 0.0f)
            continue;
        const float fraction = std::clamp(left / timer.duration, 0.0f, 1.0f);
        const ImU32 color = timer.smoke ? IM_COL32(150, 190, 255, 255) : IM_COL32(255, 130, 40, 255);
        // последние 0.5 с всё плавно гаснет
        const float fade = std::clamp(left / 0.5f, 0.0f, 1.0f);

        // зона на земле: тонкий контур + заливка, дуга по контуру показывает остаток
        ImVec2 ground[segments];
        int count = 0;
        for (int i = 0; i < segments; ++i) {
            const float angle = 2.0f * 3.14159265f * static_cast<float>(i) / segments;
            if (!Helpers::worldToScreen({ timer.center.x + std::cos(angle) * timer.radius, timer.center.y + std::sin(angle) * timer.radius, timer.center.z + 2.0f }, ground[count]))
                break;
            ++count;
        }
        if (count == segments) {
            drawList->AddConvexPolyFilled(ground, count, withAlpha(color, 0.06f * fade));
            drawList->AddPolyline(ground, count, withAlpha(color, 0.25f * fade), ImDrawFlags_Closed, 1.0f);
            const int lit = std::max(static_cast<int>(std::ceil(fraction * segments)), 1);
            ImVec2 arc[segments + 1];
            for (int i = 0; i <= lit; ++i)
                arc[i] = ground[i % segments];
            drawList->AddPolyline(arc, lit + 1, withAlpha(color, 0.85f * fade), ImDrawFlags_None, 2.0f);
        }

        // значок над центром: тёмный круг, дорожка, дуга остатка и секунды внутри
        ImVec2 screen;
        if (!Helpers::worldToScreen({ timer.center.x, timer.center.y, timer.center.z + (timer.smoke ? 60.0f : 30.0f) }, screen))
            continue;
        constexpr float radius = 17.0f;
        constexpr float start = -3.14159265f * 0.5f;
        drawList->AddCircleFilled(screen, radius + 2.0f, withAlpha(IM_COL32(12, 12, 16, 255), 0.75f * fade), 32);
        drawList->AddCircle(screen, radius, withAlpha(IM_COL32(255, 255, 255, 255), 0.12f * fade), 32, 3.0f);
        drawList->PathArcTo(screen, radius, start, start + 2.0f * 3.14159265f * fraction, 32);
        drawList->PathStroke(withAlpha(color, fade), ImDrawFlags_None, 3.0f);

        char seconds[16];
        std::snprintf(seconds, sizeof(seconds), left < 10.0f ? "%.1f" : "%.0f", left);
        const auto secondsSize = textSize(seconds);
        shadowText(drawList, { screen.x - secondsSize.x * 0.5f, screen.y - secondsSize.y * 0.5f }, withAlpha(IM_COL32(255, 255, 255, 255), fade), seconds);
        const char* label = timer.smoke ? "ДЫМ" : "ОГОНЬ";
        const auto labelSize = textSize(label);
        shadowText(drawList, { screen.x - labelSize.x * 0.5f, screen.y + radius + 4.0f }, withAlpha(color, fade), label);
    }
}

}

void collectGrenades(const ClientInterfaces& clientInterfaces, const Memory& memory) noexcept
{
    static const bool haveSmoke = Netvars::get(fnv::hash("CSmokeGrenadeProjectile->m_nSmokeEffectTickBegin")) && Netvars::get(fnv::hash("CSmokeGrenadeProjectile->m_bDidSmokeEffect"));
    static const bool haveFire = Netvars::get(fnv::hash("CInferno->m_nFireEffectTickBegin")) != 0;
    std::vector<GrenadeTimer> found;
    if (config.view.grenadeTimers && localPlayer) {
        const auto& globals = *memory.globalVars;
        const auto& entityList = clientInterfaces.getEntityList();
        const int highest = entityList.getHighestEntityIndex();
        for (int i = 65; i <= highest; ++i) {
            const auto entity = csgo::Entity::from(retSpoofGadgets->client, entityList.getEntity(i));
            if (!entity.getPOD() || entity.getNetworkable().isDormant())
                continue;
            const auto clientClass = entity.getNetworkable().getClientClass();
            if (!clientClass)
                continue;
            if (clientClass->classId == ClassId::SmokeGrenadeProjectile && haveSmoke) {
                if (!entity.didSmokeEffect() || entity.smokeEffectTickBegin() <= 0)
                    continue;
                const float elapsed = static_cast<float>(globals.tickCount - entity.smokeEffectTickBegin()) * globals.intervalPerTick;
                if (elapsed < smokeDuration)
                    found.push_back({ entity.getAbsOrigin(), smokeRadius, globals.realtime + smokeDuration - elapsed, smokeDuration, true });
            } else if (clientClass->classId == ClassId::Inferno && haveFire) {
                // центр и размер зоны — по горящим точкам
                const auto& origin = entity.getAbsOrigin();
                csgo::Vector sum{};
                int burning = 0;
                const int fires = std::clamp(entity.fireCount(), 0, 100);
                for (int k = 0; k < fires; ++k) {
                    if (!entity.fireIsBurning()[k])
                        continue;
                    sum += csgo::Vector{ origin.x + entity.fireXDelta()[k], origin.y + entity.fireYDelta()[k], origin.z + entity.fireZDelta()[k] };
                    ++burning;
                }
                if (burning == 0)
                    continue;
                const auto center = sum / static_cast<float>(burning);
                float radius = 0.0f;
                for (int k = 0; k < fires; ++k) {
                    if (entity.fireIsBurning()[k])
                        radius = std::max(radius, std::hypot(origin.x + entity.fireXDelta()[k] - center.x, origin.y + entity.fireYDelta()[k] - center.y));
                }
                const float begin = entity.fireEffectTickBegin() > 0 ? static_cast<float>(entity.fireEffectTickBegin()) * globals.intervalPerTick : globals.currenttime;
                const float elapsed = std::max(static_cast<float>(globals.tickCount) * globals.intervalPerTick - begin, 0.0f);
                if (elapsed < fireDuration)
                    found.push_back({ center, radius + 40.0f, globals.realtime + fireDuration - elapsed, fireDuration, false });
            }
        }
    }
    std::scoped_lock lock{ grenadeMutex };
    grenadeTimers = std::move(found);
}

void drawPreviewFlags(ImDrawList* drawList, ImVec2 position) noexcept
{
    drawPreviewFlagsImpl(drawList, position);
}

void onCreateMove(const csgo::UserCmd* cmd) noexcept
{
    float cone = 0.0f;
    if (localPlayer && localPlayer.get().isAlive()) {
        if (const auto weapon = csgo::Entity::from(retSpoofGadgets->client, localPlayer.get().getActiveWeapon()); weapon.getPOD()) {
            if (const auto data = weapon.getWeaponData(); data && weapon.getWeaponType() != WeaponType::Knife && weapon.getWeaponType() != WeaponType::Grenade) {
                const float spread = weapon.weaponMode() == 1 ? data->spreadAlt : data->spread;
                cone = std::max(weapon.getInaccuracy(), 0.0f) + (spread >= 0.0f && spread < 0.2f ? spread : 0.0f);
            }
        }
    }
    spreadCone = cone;
}

void onDamage(int victimIndex, int damage, bool headshot) noexcept
{
    std::scoped_lock lock{ eventMutex };
    if (pendingDamage.size() < 32)
        pendingDamage.push_back({ victimIndex, damage, headshot });
}

void onKill() noexcept
{
    std::scoped_lock lock{ eventMutex };
    pendingKill = true;
}

void draw(ImDrawList* drawList, float realtime) noexcept
{
    GameData::Lock lock;
    for (const auto& player : GameData::players()) {
        drawFlags(drawList, player, realtime);
        drawAimDot(drawList, player, realtime);
    }
    drawShotSkeleton(drawList, realtime);
    drawFootsteps(drawList);
    drawGrenadeTimers(drawList, realtime);
    drawDamageNumbers(drawList);
    drawSpreadCircle(drawList);
    drawKillFlash(drawList);
}

}

#include <algorithm>
#include <array>
#include <atomic>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <chrono>
#include <cstring>
#include <mutex>
#include <random>
#include <string_view>
#include <string>
#include <unordered_map>
#include <vector>

#include <imgui/imgui.h>

#include "HvhConfig.h"
#include "HvhGui.h"
#include "Hud.h"
#include "AntiAim.h"
#include "HvhVisuals.h"
#include "Ragebot.h"
#include "Defensive.h"
#include "Tickbase.h"

#include <GameData.h>
#include <Helpers.h>
#include <Memory.h>
#include <fnv.h>
#include <Netvars.h>
#include <Interfaces/ClientInterfaces.h>
#include <Interfaces/EngineInterfaces.h>
#include <CSGO/EngineTrace.h>
#include <Interfaces/OtherInterfaces.h>
#include <CSGO/ConVar.h>
#include <CSGO/Cvar.h>
#include <CSGO/Material.h>
#include <CSGO/MaterialSystem.h>
#include <CSGO/PODs/ConVar.h>
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

// искры от своих пуль: события приходят в игровом потоке, физика и отрисовка — в потоке рендера
struct PendingImpact {
    csgo::Vector point;
    csgo::Vector from;
};
struct Spark {
    csgo::Vector position;
    csgo::Vector velocity;
    float life;
    float maxLife;
};
struct ImpactFlash {
    csgo::Vector position;
    double at;
};
std::mutex impactMutex;
std::vector<PendingImpact> pendingImpacts;
std::vector<Spark> sparks;
std::vector<ImpactFlash> impactFlashes;
std::minstd_rand sparkRng{ 1337 };

// трассеры своих пуль (под impactMutex) и последнее попадание для хитмаркера (поток рендера)
struct Tracer {
    csgo::Vector from, to;
    double at;      // < 0 — ещё не показан: время ставит поток рендера
};
std::vector<Tracer> tracers;
struct HitMark {
    double at = -10.0;
    int damage = 0;
    bool headshot = false;
};
HitMark lastHit;
// след за собой
struct TrailPoint {
    csgo::Vector position;
    double at;
};
std::vector<TrailPoint> trail;
std::atomic<bool> holdingZeus = false;
std::atomic<float> zeusReach = 0.0f;

// всё рисуется общим стилем из Hud.h; короткие имена — для этого файла
using hud::withAlpha;

void shadowText(ImDrawList* drawList, ImVec2 position, ImU32 color, const char* text) noexcept
{
    hud::text(drawList, hud::smallFont(), position, color, text);
}

[[nodiscard]] ImVec2 textSize(const char* text) noexcept
{
    return hud::measure(hud::smallFont(), text);
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

// ---------- флаги справа от рамки игрока ----------
// Столбик коротких меток. Цвет — по смыслу (см. Hud.h): белое — снаряжение, голубое — что враг делает,
// жёлтое — «сейчас уязвим», красное — опасность, акцент — эксплойты, зелёное — «убиваем».

struct FlagLine {
    ImU32 color;
    char text[32];
};
using FlagList = std::array<FlagLine, 12>;

void addFlag(FlagList& list, int& count, ImU32 color, const char* text) noexcept
{
    if (count >= static_cast<int>(list.size()))
        return;
    list[count].color = color;
    std::snprintf(list[count].text, sizeof(list[count].text), "%s", text);
    ++count;
}

void drawFlagColumn(ImDrawList* drawList, ImVec2 position, const FlagList& list, int count) noexcept
{
    const float lineHeight = hud::smallFont()->FontSize + 1.0f;
    for (int i = 0; i < count; ++i) {
        shadowText(drawList, position, list[i].color, list[i].text);
        position.y += lineHeight;
    }
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
    FlagList list;
    int count = 0;
    char buffer[32];
    if ((flags & FlagArmor) && player.armor > 0)
        addFlag(list, count, hud::color::text, player.helmet ? "HK" : "K");
    if (flags & FlagMoney) {
        std::snprintf(buffer, sizeof(buffer), "$%d", player.money);
        addFlag(list, count, hud::color::dim, buffer);
    }
    if ((flags & FlagKit) && player.defuser)
        addFlag(list, count, player.defusing ? hud::color::bad : hud::color::info, player.defusing ? "DEFUSE" : "KIT");
    if ((flags & FlagScoped) && player.scoped)
        addFlag(list, count, hud::color::info, "ZOOM");
    if ((flags & FlagReload) && player.reloading)
        addFlag(list, count, hud::color::warn, "RELOAD");
    if ((flags & FlagFlashed) && player.flashDuration > 0.5f)
        addFlag(list, count, hud::color::warn, "FLASH");
    if ((flags & FlagBomb) && player.hasBomb)
        addFlag(list, count, hud::color::bad, "C4");
    if (fresh && (flags & FlagDoubleTap) && view.charged)
        addFlag(list, count, hud::accent(), "DT");
    if (fresh && (flags & FlagLagComp) && view.breakingLagComp)
        addFlag(list, count, hud::color::bad, "LC");
    if (fresh && (flags & FlagResolver) && view.resolver && view.resolver[0]) {
        std::snprintf(buffer, sizeof(buffer), "R: %s", view.resolver);
        addFlag(list, count, hud::color::dim, buffer);
    }
    if (fresh && (flags & FlagLethal) && realtime - view.aimAt < 0.3f && view.aimLethal)
        addFlag(list, count, hud::color::good, "LETHAL");
    drawFlagColumn(drawList, { max.x + 5.0f, min.y - 1.0f }, list, count);
}

// Флаги в предпросмотре меню: пример со всеми включёнными флагами (значения выдуманные)
void drawPreviewFlagsImpl(ImDrawList* drawList, ImVec2 position) noexcept
{
    const int flags = config.view.espFlags;
    FlagList list;
    int count = 0;
    const auto flag = [&](int bit, ImU32 color, const char* text) {
        if (flags & bit)
            addFlag(list, count, color, text);
    };
    flag(FlagArmor, hud::color::text, "HK");
    flag(FlagMoney, hud::color::dim, "$4750");
    flag(FlagKit, hud::color::info, "KIT");
    flag(FlagScoped, hud::color::info, "ZOOM");
    flag(FlagReload, hud::color::warn, "RELOAD");
    flag(FlagFlashed, hud::color::warn, "FLASH");
    flag(FlagBomb, hud::color::bad, "C4");
    flag(FlagDoubleTap, hud::accent(), "DT");
    flag(FlagLagComp, hud::color::bad, "LC");
    flag(FlagResolver, hud::color::dim, "R: слои");
    flag(FlagLethal, hud::color::good, "LETHAL");
    drawFlagColumn(drawList, position, list, count);
}

// ---------- куда целится рейдж ----------
// Точки мультипоинта: зелёная — выстрел сюда убивает, жёлтая — набирает мин. урон.
// Кольцо — точка, которую рейдж выбрал; красное — туда только что ушёл выстрел.
void drawAimDot(ImDrawList* drawList, const PlayerData& player, float realtime) noexcept
{
    if (!config.view.aimDot || !player.enemy || !player.alive || player.dormant)
        return;
    const auto view = ragebot::targetView(player.index);

    if (const float age = realtime - view.pointsAt; age >= 0.0f && age < 0.3f) {
        const float alpha = 1.0f - age / 0.3f * 0.5f;
        for (int k = 0; k < view.pointCount; ++k) {
            const int state = view.pointState[k];
            if (state == 0)
                continue;
            ImVec2 screen;
            if (!Helpers::worldToScreen({ view.points[k][0], view.points[k][1], view.points[k][2] }, screen))
                continue;
            hud::dot(drawList, screen, 2.5f, state == 2 ? hud::color::good : hud::color::warn, alpha);
        }
    }

    const float age = realtime - view.aimAt;
    if (age > 0.25f)
        return;
    ImVec2 screen;
    if (!Helpers::worldToScreen({ view.aimPoint[0], view.aimPoint[1], view.aimPoint[2] }, screen))
        return;
    const float alpha = 1.0f - age / 0.25f * 0.6f;
    const ImU32 color = view.aimedNow ? hud::color::bad : view.aimLethal ? hud::color::good : hud::color::warn;
    drawList->AddCircle(screen, 7.0f, withAlpha(IM_COL32(0, 0, 0, 255), alpha * 0.8f), 20, 3.0f);
    drawList->AddCircle(screen, 7.0f, withAlpha(color, alpha), 20, 1.5f);
}

// Скелет, по которому ушёл выстрел: белый — ждём ответа сервера, зелёный — попал, красный — мимо
void drawShotSkeleton(ImDrawList* drawList, float realtime) noexcept
{
    if (!config.view.shotCapsules)
        return;
    const auto shot = ragebot::shotView();
    const float age = realtime - shot.at;
    if (shot.at <= 0.0f || age > 2.5f || age < 0.0f)
        return;
    const float alpha = 1.0f - age / 2.5f;
    const ImU32 color = shot.result > 0 ? hud::color::good : shot.result < 0 ? hud::color::bad : hud::color::text;
    for (int k = 0; k < shot.count; ++k) {
        ImVec2 sa, sb;
        if (!Helpers::worldToScreen({ shot.a[k][0], shot.a[k][1], shot.a[k][2] }, sa)
            || !Helpers::worldToScreen({ shot.b[k][0], shot.b[k][1], shot.b[k][2] }, sb))
            continue;
        hud::line(drawList, sa, sb, color, alpha);
    }
}

// Цифры урона: выскакивают над врагом жирным, всплывают и гаснут. Красная — в голову.
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
            lastHit = { now, event.damage, event.headshot };
        }
        pendingDamage.clear();
        if (pendingKill) {
            pendingKill = false;
            killAt = now;
        }
    }
    constexpr double lifetime = 1.2;
    std::erase_if(damageNumbers, [now](const DamageNumber& number) { return now - number.at > lifetime; });
    if (!config.view.damageNumbers)
        return;
    ImFont* font = hud::boldFont();
    for (const auto& number : damageNumbers) {
        const float age = static_cast<float>(now - number.at);
        const float t = age / static_cast<float>(lifetime);
        ImVec2 screen;
        if (!Helpers::worldToScreen(number.position, screen))
            continue;
        screen.y -= hud::easeOut(age, static_cast<float>(lifetime)) * 44.0f;
        char text[16];
        std::snprintf(text, sizeof(text), "%d", number.damage);
        const ImU32 color = number.headshot ? hud::color::bad : hud::color::text;
        const float alpha = t < 0.6f ? 1.0f : 1.0f - (t - 0.6f) / 0.4f;
        hud::textCentered(drawList, font, screen, withAlpha(color, alpha), text);
    }
}

[[nodiscard]] float randomUnit() noexcept
{
    return std::uniform_real_distribution<float>{ 0.0f, 1.0f }(sparkRng);
}

void spawnSparks(const PendingImpact& impact, double now) noexcept
{
    csgo::Vector back = impact.from - impact.point;     // к стрелку — туда в основном и летят искры
    const float backLength = back.length();
    back = backLength > 0.001f ? back / backLength : csgo::Vector{ 0.0f, 0.0f, 1.0f };

    const float size = config.view.sparkSize;
    const int count = std::clamp(config.view.sparkCount, 4, 60);
    for (int i = 0; i < count && sparks.size() < 900; ++i) {
        // случайное направление на сфере, развёрнутое в полусферу к стрелку, плюс подброс вверх
        const float z = randomUnit() * 2.0f - 1.0f;
        const float a = randomUnit() * 6.2831853f;
        const float r = std::sqrt(std::max(0.0f, 1.0f - z * z));
        csgo::Vector dir{ r * std::cos(a), r * std::sin(a), z };
        if (dir.dotProduct(back) < 0.0f)
            dir = dir * -1.0f;
        dir = dir + back * 0.55f;
        dir.z += 0.45f;
        if (const float l = dir.length(); l > 0.001f)
            dir = dir / l;
        const float speed = (110.0f + randomUnit() * 380.0f) * size;
        sparks.push_back({ impact.point + back * 1.5f, dir * speed, 0.0f, 0.3f + randomUnit() * 0.6f });
    }
    impactFlashes.push_back({ impact.point + back * 1.5f, now });
}

void drawSparks(ImDrawList* drawList) noexcept
{
    const double now = ImGui::GetTime();
    static double lastFrame = now;
    const float dt = std::clamp(static_cast<float>(now - lastFrame), 0.0f, 0.05f);
    lastFrame = now;

    {
        std::scoped_lock lock{ impactMutex };
        for (const auto& impact : pendingImpacts) {
            // пробитие даёт несколько импактов почти в одной точке — хватит одной вспышки
            const bool duplicate = std::any_of(impactFlashes.begin(), impactFlashes.end(), [&](const ImpactFlash& f) {
                return now - f.at < 0.05 && f.position.distTo(impact.point) < 4.0f;
            });
            if (!duplicate)
                spawnSparks(impact, now);
        }
        pendingImpacts.clear();
    }

    const float drag = std::exp(-2.2f * dt);
    for (auto& spark : sparks) {
        spark.velocity.z -= 620.0f * dt;     // тяжелее обычной гравитации — красиво падают дугой
        spark.velocity = spark.velocity * drag;
        spark.position += spark.velocity * dt;
        spark.life += dt;
    }
    std::erase_if(sparks, [](const Spark& spark) { return spark.life >= spark.maxLife; });
    std::erase_if(impactFlashes, [now](const ImpactFlash& f) { return now - f.at > 0.25; });

    if (!config.view.impactSparks)
        return;

    const auto* c = config.view.sparkColor;
    const ImU32 tint = IM_COL32(static_cast<int>(c[0] * 255), static_cast<int>(c[1] * 255), static_cast<int>(c[2] * 255), 255);
    const float alphaScale = c[3];
    const float tailScale = 0.035f * config.view.sparkSize;

    for (const auto& flash : impactFlashes) {
        const float t = static_cast<float>(now - flash.at) / 0.25f;
        ImVec2 screen;
        if (!Helpers::worldToScreen(flash.position, screen))
            continue;
        const float radius = 3.0f + t * 12.0f;
        drawList->AddCircleFilled(screen, radius * 1.8f, withAlpha(tint, (1.0f - t) * 0.18f * alphaScale), 20);
        drawList->AddCircleFilled(screen, radius * 0.6f, withAlpha(IM_COL32(255, 250, 225, 255), (1.0f - t) * 0.9f * alphaScale), 16);
    }

    for (const auto& spark : sparks) {
        const float t = spark.life / spark.maxLife;
        ImVec2 head, tail;
        if (!Helpers::worldToScreen(spark.position, head) || !Helpers::worldToScreen(spark.position - spark.velocity * tailScale, tail))
            continue;
        // сначала почти белая (раскалённая), к концу жизни — цвет из настроек
        const float heat = std::clamp(1.0f - t * 1.6f, 0.0f, 1.0f);
        const auto lerp = [heat](int hot, float cold) { return static_cast<int>(hot * heat + cold * 255.0f * (1.0f - heat)); };
        const ImU32 core = IM_COL32(lerp(255, c[0]), lerp(245, c[1]), lerp(200, c[2]), 255);
        const float alpha = std::pow(1.0f - t, 1.3f) * alphaScale;
        drawList->AddLine(tail, head, withAlpha(tint, alpha * 0.28f), 4.0f);
        drawList->AddLine(tail, head, withAlpha(core, alpha), 1.5f);
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
            drawList->AddPolyline(points, count, withAlpha(hud::color::bad, (1.0f - t) * 0.9f), ImDrawFlags_Closed, 1.5f);
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

    // зевс в руках — для круга дальности
    bool zeus = false;
    if (localPlayer && localPlayer.get().isAlive()) {
        if (const auto weapon = csgo::Entity::from(retSpoofGadgets->client, localPlayer.get().getActiveWeapon()); weapon.getPOD()
            && weapon.itemDefinitionIndex() == WeaponId::Taser) {
            zeus = true;
            if (const auto data = weapon.getWeaponData())
                zeusReach = data->range;
        }
    }
    holdingZeus = zeus;
}

void onDamage(int victimIndex, int damage, bool headshot) noexcept
{
    std::scoped_lock lock{ eventMutex };
    if (pendingDamage.size() < 32)
        pendingDamage.push_back({ victimIndex, damage, headshot });
}

void onImpact(float x, float y, float z, float fromX, float fromY, float fromZ) noexcept
{
    std::scoped_lock lock{ impactMutex };
    if (config.view.bulletTracers && tracers.size() < 48)
        tracers.push_back({ { fromX, fromY, fromZ }, { x, y, z }, -1.0 });
    if (!config.view.impactSparks)
        return;
    if (pendingImpacts.size() < 32)
        pendingImpacts.push_back({ { x, y, z }, { fromX, fromY, fromZ } });
}

void onKill() noexcept
{
    std::scoped_lock lock{ eventMutex };
    pendingKill = true;
}

// extended yaw: у ног три луча — отправленный взгляд (белый), куда ломаем LBY (акцент, пунктир)
// и LBY, который прислал сервер (акцент). Работает — LBY-луч стоит на пунктире, а не на взгляде.
void drawExtendedYaw(ImDrawList* drawList) noexcept
{
    if (!config.view.serverModel || config.antiAim.extendedYaw <= 0)
        return;
    const auto view = antiaim::extendedView();
    if (!view.active)
        return;
    const csgo::Vector origin{ view.origin[0], view.origin[1], view.origin[2] + 2.0f };
    ImVec2 center;
    if (!Helpers::worldToScreen(origin, center))
        return;
    const auto ray = [&](float yaw, float length, ImU32 color, bool dashed, const char* label) {
        const float rad = Helpers::deg2rad(yaw);
        const csgo::Vector end{ origin.x + std::cos(rad) * length, origin.y + std::sin(rad) * length, origin.z };
        ImVec2 tip;
        if (!Helpers::worldToScreen(end, tip))
            return;
        if (dashed) {
            for (int k = 0; k < 8; k += 2) {
                const float a = k / 8.0f, b = (k + 1) / 8.0f;
                drawList->AddLine({ center.x + (tip.x - center.x) * a, center.y + (tip.y - center.y) * a },
                                  { center.x + (tip.x - center.x) * b, center.y + (tip.y - center.y) * b }, color, 2.0f);
            }
        } else {
            drawList->AddLine(center, tip, color, 2.0f);
        }
        shadowText(drawList, { tip.x + 3.0f, tip.y - 7.0f }, color, label);
    };
    ray(view.eyeYaw, 40.0f, IM_COL32(255, 255, 255, 200), false, "взгляд");
    ray(view.flickYaw, 40.0f, withAlpha(ui::color::accent, 0.6f), true, "");
    ray(view.lby, 32.0f, withAlpha(ui::color::accent, 1.0f), false, "LBY");
}

// Где модель стоит на сервере, пока пакеты в чоке (лаг-пик, фейклаг): хитбокс-коробка игрока
// на позиции последней отправленной команды, линия от ног до неё и подпись с числом тиков.
// Видно и от 1-го лица: на лаг-пике коробка остаётся за углом, пока не отпустишь пачку.
void drawServerPosition(ImDrawList* drawList) noexcept
{
    if (!config.view.serverBox)
        return;
    const auto& local = GameData::local();
    if (!local.exists || !local.alive)
        return;
    const int choked = tickbase::chokedCommands();
    float sent[3];
    bool ducked = false;
    if (choked <= 0 || !antiaim::sentOrigin(sent, ducked))
        return;
    const csgo::Vector origin{ sent[0], sent[1], sent[2] };
    const float distance = (origin - local.origin).length();
    const bool peeking = defensive::lagPeekHolding();
    // стоим на месте — коробка совпала бы с нами; телепорт/респаун — точка устарела
    if ((distance < 2.0f && !peeking) || distance > 256.0f)
        return;

    const auto& c = config.view.serverBoxColor;
    const ImU32 color = ImGui::ColorConvertFloat4ToU32({ c[0], c[1], c[2], c[3] });
    const ImU32 fill = ImGui::ColorConvertFloat4ToU32({ c[0], c[1], c[2], c[3] * 0.12f });
    const float height = ducked ? 54.0f : 72.0f;
    ImVec2 corners[8];
    for (int i = 0; i < 8; ++i) {
        const csgo::Vector point{ origin.x + ((i & 1) ? 16.0f : -16.0f), origin.y + ((i & 2) ? 16.0f : -16.0f), origin.z + ((i & 4) ? height : 0.0f) };
        if (!Helpers::worldToScreen(point, corners[i]))
            return;
    }
    // заливка — по выпуклой оболочке не заморачиваемся: две грани (низ и верх) полупрозрачно
    const ImVec2 bottom[]{ corners[0], corners[1], corners[3], corners[2] };
    const ImVec2 top[]{ corners[4], corners[5], corners[7], corners[6] };
    drawList->AddConvexPolyFilled(bottom, 4, fill);
    drawList->AddConvexPolyFilled(top, 4, fill);
    for (int i = 0; i < 8; ++i) {
        for (int bit = 1; bit <= 4; bit <<= 1) {
            if (i & bit)
                continue;
            drawList->AddLine(corners[i], corners[i | bit], withAlpha(IM_COL32(0, 0, 0, 255), c[3] * 0.6f), 3.0f);
            drawList->AddLine(corners[i], corners[i | bit], color, 1.2f);
        }
    }

    // линия от наших ног до серверных
    ImVec2 feet, serverFeet;
    if (Helpers::worldToScreen(local.origin, feet) && Helpers::worldToScreen(origin, serverFeet))
        drawList->AddLine(feet, serverFeet, withAlpha(color, 0.6f), 1.0f);

    ImVec2 label;
    if (Helpers::worldToScreen({ origin.x, origin.y, origin.z + height + 6.0f }, label)) {
        char text[48];
        std::snprintf(text, sizeof(text), peeking ? "лаг-пик · %d т." : "сервер · %d т.", choked);
        const auto size = textSize(text);
        shadowText(drawList, { label.x - size.x * 0.5f, label.y - size.y }, color, text);
    }
}

namespace
{

[[nodiscard]] ImU32 toColor(const float rgba[4]) noexcept
{
    return IM_COL32(static_cast<int>(rgba[0] * 255.0f), static_cast<int>(rgba[1] * 255.0f), static_cast<int>(rgba[2] * 255.0f), static_cast<int>(rgba[3] * 255.0f));
}

// ---------- стрелки на врагов за экраном ----------
// Вид — наш «наконечник» на кольце вокруг прицела; логика — как у старых стрелок Osiris: направление
// «по радару» (вперёд — вверх, сзади — вниз, наклон камеры не влияет), стрелка есть, пока враг вне поля
// зрения, дормант гаснет по таймеру ESP. Чем ближе враг, тем ярче и крупнее; красная — слышно/видно сейчас.
void drawOffscreenArrows(ImDrawList* drawList, float realtime) noexcept
{
    if (!config.view.offscreenArrows || !GameData::local().exists || !GameData::local().alive)
        return;
    const auto& matrix = GameData::toScreenMatrix();
    const auto display = ImGui::GetIO().DisplaySize;
    const ImVec2 center{ display.x * 0.5f, display.y * 0.5f };
    const float radius = static_cast<float>(config.view.arrowRadius);
    const auto& me = GameData::local().origin;
    // рыскание камеры из матрицы: строка w = глубина вдоль взгляда, т.е. направление вперёд
    const float yaw = std::atan2(matrix._42, matrix._41);
    for (const auto& player : GameData::players()) {
        if (!player.enemy || !player.alive || player.inViewFrustum)
            continue;
        const float fade = player.fadingAlphaAt(realtime);
        if (player.dormant && fade <= 0.0f)
            continue;
        const float dx = me.x - player.origin.x, dy = me.y - player.origin.y;
        ImVec2 direction{ std::cos(yaw) * dy - std::sin(yaw) * dx, std::cos(yaw) * dx + std::sin(yaw) * dy };
        const float length = std::sqrt(direction.x * direction.x + direction.y * direction.y);
        if (length < 0.001f)
            continue;
        direction = { direction.x / length, direction.y / length };

        const float distance = std::sqrt((player.origin.x - me.x) * (player.origin.x - me.x) + (player.origin.y - me.y) * (player.origin.y - me.y));
        const float nearness = 1.0f - std::clamp(distance / 2500.0f, 0.0f, 1.0f);
        const float size = 9.0f + nearness * 7.0f;
        const float pulse = 0.85f + 0.15f * std::sin(static_cast<float>(ImGui::GetTime()) * 5.0f + static_cast<float>(player.index));
        const float alpha = player.dormant ? 0.6f * fade : (0.45f + nearness * 0.55f) * pulse;
        const ImU32 color = player.dormant ? hud::color::dim : (player.visible || player.audible) ? hud::color::bad : hud::color::text;

        const ImVec2 tip{ center.x + direction.x * (radius + size), center.y + direction.y * (radius + size) };
        const ImVec2 side{ -direction.y, direction.x };
        const ImVec2 left{ center.x + direction.x * (radius - size * 0.5f) + side.x * size * 0.75f, center.y + direction.y * (radius - size * 0.5f) + side.y * size * 0.75f };
        const ImVec2 right{ center.x + direction.x * (radius - size * 0.5f) - side.x * size * 0.75f, center.y + direction.y * (radius - size * 0.5f) - side.y * size * 0.75f };
        const ImVec2 notch{ center.x + direction.x * radius, center.y + direction.y * radius };
        // «наконечник»: два треугольника с вырезом сзади + тёмный контур
        drawList->AddTriangleFilled(tip, left, notch, withAlpha(color, alpha));
        drawList->AddTriangleFilled(tip, notch, right, withAlpha(color, alpha * 0.75f));
        const ImVec2 outline[]{ tip, left, notch, right };
        drawList->AddPolyline(outline, 4, withAlpha(IM_COL32(0, 0, 0, 255), alpha * 0.8f), ImDrawFlags_Closed, 1.0f);
        if (!player.dormant && nearness > 0.2f) {
            char text[16];
            std::snprintf(text, sizeof(text), "%.0fm", distance * 0.01905f);
            const auto textSizeValue = textSize(text);
            shadowText(drawList, { notch.x - direction.x * 14.0f - textSizeValue.x * 0.5f, notch.y - direction.y * 14.0f - textSizeValue.y * 0.5f }, withAlpha(hud::color::dim, alpha), text);
        }
    }
}

// ---------- хитмаркер у прицела ----------
// Четыре штриха разлетаются от прицела и гаснут; рядом — урон. Белый — тело, красный — голова.
void drawHitMarker(ImDrawList* drawList) noexcept
{
    if (!config.view.hitMarker)
        return;
    constexpr float lifetime = 0.45f;
    const float age = static_cast<float>(ImGui::GetTime() - lastHit.at);
    if (age < 0.0f || age > lifetime)
        return;
    const auto display = ImGui::GetIO().DisplaySize;
    const ImVec2 center{ std::floor(display.x * 0.5f), std::floor(display.y * 0.5f) };
    const float alpha = 1.0f - (age / lifetime) * (age / lifetime);
    const float gap = 5.0f + hud::easeOut(age, 0.12f) * 4.0f;
    constexpr float length = 7.0f;
    const ImU32 color = lastHit.headshot ? hud::color::bad : hud::color::text;
    for (const auto& [dx, dy] : { std::pair{ 1.0f, 1.0f }, std::pair{ -1.0f, 1.0f }, std::pair{ 1.0f, -1.0f }, std::pair{ -1.0f, -1.0f } })
        hud::line(drawList, { center.x + dx * gap, center.y + dy * gap }, { center.x + dx * (gap + length), center.y + dy * (gap + length) }, color, alpha, 1.5f);
    char text[16];
    std::snprintf(text, sizeof(text), "%d", lastHit.damage);
    hud::text(drawList, hud::boldFont(), { center.x + gap + length + 6.0f, center.y - gap - length - 8.0f }, withAlpha(color, alpha), text);
}

// ---------- трассеры своих пуль ----------
void drawTracers(ImDrawList* drawList) noexcept
{
    const double now = ImGui::GetTime();
    constexpr double lifetime = 1.4;
    std::vector<Tracer> copy;
    {
        std::scoped_lock lock{ impactMutex };
        for (auto& tracer : tracers) {
            if (tracer.at < 0.0)
                tracer.at = now;
        }
        std::erase_if(tracers, [now](const Tracer& tracer) { return now - tracer.at > lifetime; });
        if (config.view.bulletTracers)
            copy = tracers;
    }
    const ImU32 color = toColor(config.view.tracerColor);
    for (const auto& tracer : copy) {
        // начало — чуть впереди и ниже глаза: сам глаз в первом лице на экран не проецируется
        auto direction = tracer.to - tracer.from;
        const float length = direction.length();
        if (length < 48.0f)
            continue;
        direction = direction / length;
        const csgo::Vector start{ tracer.from.x + direction.x * 40.0f, tracer.from.y + direction.y * 40.0f, tracer.from.z + direction.z * 40.0f - 5.0f };
        ImVec2 a, b;
        if (!Helpers::worldToScreen(start, a) || !Helpers::worldToScreen(tracer.to, b))
            continue;
        const float t = static_cast<float>((now - tracer.at) / lifetime);
        const float alpha = (1.0f - t * t);
        drawList->AddLine(a, b, withAlpha(color, alpha * 0.18f), 5.0f);     // ореол
        drawList->AddLine(a, b, withAlpha(color, alpha * 0.9f), 1.4f);
        hud::dot(drawList, b, 2.0f, color, alpha);
    }
}

// ---------- след за собой ----------
void drawTrail(ImDrawList* drawList) noexcept
{
    const double now = ImGui::GetTime();
    constexpr double lifetime = 1.6;
    const auto& local = GameData::local();
    if (!config.view.localTrail || !local.exists || !local.alive) {
        trail.clear();
        return;
    }
    if (trail.empty() || (trail.back().position - local.origin).squareLength() > 9.0f)
        trail.push_back({ local.origin, now });
    std::erase_if(trail, [now](const TrailPoint& point) { return now - point.at > lifetime; });
    const ImU32 color = toColor(config.view.trailColor);
    for (std::size_t i = 1; i < trail.size(); ++i) {
        // разрыв (телепорт, респаун) не соединяем
        if ((trail[i].position - trail[i - 1].position).squareLength() > 80.0f * 80.0f)
            continue;
        ImVec2 a, b;
        const csgo::Vector lift{ 0.0f, 0.0f, 2.0f };
        if (!Helpers::worldToScreen(trail[i - 1].position + lift, a) || !Helpers::worldToScreen(trail[i].position + lift, b))
            continue;
        const float alpha = 1.0f - static_cast<float>((now - trail[i].at) / lifetime);
        drawList->AddLine(a, b, withAlpha(color, alpha * 0.2f), 6.0f);
        drawList->AddLine(a, b, withAlpha(color, alpha), 2.0f);
    }
}

// ---------- метки вражеских гранат ----------
// На летящей гранате врага — плашка: тип и расстояние до нас. HE и молотов ближе ~350 ед. — красная.
void drawGrenadeWarnings(ImDrawList* drawList) noexcept
{
    if (!config.view.grenadeWarning || !GameData::local().exists || !GameData::local().alive)
        return;
    const auto& me = GameData::local().origin;
    for (const auto& projectile : GameData::projectiles()) {
        if (!projectile.thrownByEnemy || projectile.exploded || projectile.trajectory.empty() || !projectile.name)
            continue;
        const char* label = nullptr;
        bool dangerous = false;
        const std::string_view name{ projectile.name };
        if (name == "HE Grenade") {
            label = "HE";
            dangerous = true;
        } else if (name == "Molotov") {
            label = "FIRE";
            dangerous = true;
        } else if (name == "Flashbang") {
            label = "FLASH";
        } else if (name == "Smoke Grenade") {
            label = "SMOKE";
        } else {
            continue;
        }
        const auto& position = projectile.trajectory.back().second;
        const float distance = (position - me).length();
        const bool close = dangerous && distance < 350.0f;
        // хвост полёта
        const std::size_t count = projectile.trajectory.size();
        for (std::size_t i = count > 12 ? count - 12 : 1; i < count; ++i) {
            ImVec2 a, b;
            if (Helpers::worldToScreen(projectile.trajectory[i - 1].second, a) && Helpers::worldToScreen(projectile.trajectory[i].second, b))
                drawList->AddLine(a, b, withAlpha(close ? hud::color::bad : hud::color::warn, 0.25f + 0.5f * static_cast<float>(i + 12 - count) / 12.0f), 1.5f);
        }
        ImVec2 screen;
        if (!Helpers::worldToScreen(position, screen))
            continue;
        char text[32];
        std::snprintf(text, sizeof(text), "%s  %.0fm", label, distance * 0.01905f);
        const ImU32 color = close ? hud::color::bad : dangerous ? hud::color::warn : hud::color::dim;
        const auto size = textSize(text);
        hud::dot(drawList, screen, 3.0f, color);
        hud::chip(drawList, { screen.x - size.x * 0.5f - 6.0f, screen.y - size.y - 14.0f }, color, text);
    }
}

// ---------- круг дальности зевса ----------
void drawZeusRange(ImDrawList* drawList) noexcept
{
    if (!config.view.zeusRange || !holdingZeus.load() || !GameData::local().exists || !GameData::local().alive)
        return;
    const float reach = std::clamp(zeusReach.load(), 100.0f, 400.0f);
    const auto& origin = GameData::local().origin;
    constexpr int segments = 64;
    ImVec2 points[segments];
    int count = 0;
    for (int i = 0; i < segments; ++i) {
        const float angle = 2.0f * 3.14159265f * static_cast<float>(i) / segments;
        if (!Helpers::worldToScreen({ origin.x + std::cos(angle) * reach, origin.y + std::sin(angle) * reach, origin.z + 2.0f }, points[count]))
            continue;
        ++count;
    }
    // часть круга за камерой — рисуем то, что видно, отрезками между соседями
    if (count == segments) {
        drawList->AddPolyline(points, count, withAlpha(IM_COL32(0, 0, 0, 255), 0.5f), ImDrawFlags_Closed, 3.0f);
        drawList->AddPolyline(points, count, withAlpha(hud::accent(), 0.9f), ImDrawFlags_Closed, 1.5f);
    } else {
        for (int i = 1; i < count; ++i) {
            const float dx = points[i].x - points[i - 1].x, dy = points[i].y - points[i - 1].y;
            if (dx * dx + dy * dy < 200.0f * 200.0f)
                drawList->AddLine(points[i - 1], points[i], withAlpha(hud::accent(), 0.9f), 1.5f);
        }
    }
}


// ---------- погода: частицы вокруг камеры ----------
// Симуляция и трассы — в игровом потоке (stepWeather), отрисовка — в потоке рендера (drawWeather).

enum class Weather { Off, Snow, Rain, Fireflies, Ash };

struct WeatherParticle {
    csgo::Vector position;
    csgo::Vector velocity;
    float phase = 0.0f;
    float scale = 1.0f;
    bool checked = false;       // проверка «под открытым небом» уже была
    bool outdoor = false;
    bool visible = false;       // виден из глаз (не за стеной)
};

std::mutex weatherMutex;
std::vector<WeatherParticle> weatherParticles;
csgo::Vector weatherEye{};
float weatherSimTime = 0.0f;    // realtime последнего шага
int weatherKind = 0;
std::mt19937 weatherRng{ std::random_device{}() };

float weatherRandom(float min, float max) noexcept
{
    return std::uniform_real_distribution<float>{ min, max }(weatherRng);
}

csgo::Vector weatherVelocity(Weather kind, const WeatherParticle& p, float time) noexcept
{
    switch (kind) {
    case Weather::Snow:
        return { std::sin(time * 0.7f + p.phase) * 14.0f, std::cos(time * 0.5f + p.phase * 1.3f) * 14.0f, -55.0f * p.scale };
    case Weather::Rain:
        return { 40.0f, 12.0f, -950.0f * (0.85f + 0.3f * p.scale) };
    case Weather::Ash:
        return { std::sin(time * 0.4f + p.phase) * 9.0f, std::cos(time * 0.3f + p.phase) * 9.0f, -22.0f * p.scale };
    case Weather::Fireflies:
        return { std::sin(time * 0.9f + p.phase) * 22.0f, std::cos(time * 0.7f + p.phase * 1.7f) * 22.0f, std::sin(time * 1.3f + p.phase * 0.6f) * 9.0f };
    default:
        return {};
    }
}

void spawnWeather(WeatherParticle& p, Weather kind, float radius, bool top) noexcept
{
    p.position.x = weatherEye.x + weatherRandom(-radius, radius);
    p.position.y = weatherEye.y + weatherRandom(-radius, radius);
    if (kind == Weather::Fireflies)
        p.position.z = weatherEye.z + weatherRandom(-radius * 0.25f, radius * 0.2f);
    else if (top)
        p.position.z = weatherEye.z + radius * 0.5f + weatherRandom(0.0f, radius * 0.15f);
    else
        p.position.z = weatherEye.z + weatherRandom(-radius * 0.4f, radius * 0.6f);
    p.phase = weatherRandom(0.0f, 6.2831f);
    p.scale = weatherRandom(0.6f, 1.4f);
    p.checked = false;
    p.outdoor = false;
    p.visible = false;
}

constexpr unsigned weatherSolidMask = 0x400B;   // MASK_SOLID_BRUSHONLY
constexpr unsigned short weatherSurfSky = 0x4;  // SURF_SKY

void drawWeather(ImDrawList* drawList, float realtime) noexcept
{
    std::scoped_lock lock{ weatherMutex };
    const auto& w = config.world;
    const auto kind = static_cast<Weather>(w.weather);
    if (kind == Weather::Off || weatherParticles.empty())
        return;
    const auto tint = [&w](float alphaScale) {
        return ImGui::ColorConvertFloat4ToU32({ w.particleColor[0], w.particleColor[1], w.particleColor[2], std::clamp(w.particleColor[3] * alphaScale, 0.0f, 1.0f) });
    };
    const float ahead = std::clamp(realtime - weatherSimTime, 0.0f, 0.1f);
    const float radius = w.particleRadius;
    for (const auto& p : weatherParticles) {
        if (!p.visible)
            continue;
        const auto position = p.position + p.velocity * ahead;
        const float distance = (position - weatherEye).length();
        if (distance < 12.0f || distance > radius * 1.2f)
            continue;
        ImVec2 screen;
        if (!Helpers::worldToScreen(position, screen))
            continue;
        // к краю облака тают, вблизи крупнее
        const float edge = 1.0f - std::clamp((distance - radius * 0.7f) / (radius * 0.4f), 0.0f, 1.0f);
        const float size = std::clamp(w.particleSize * 260.0f / distance, 0.5f, 7.0f) * p.scale;
        switch (kind) {
        case Weather::Snow:
            drawList->AddCircleFilled(screen, size, tint(edge), 10);
            break;
        case Weather::Ash:
            drawList->AddCircleFilled(screen, size * 0.8f, tint(edge * 0.8f), 6);
            break;
        case Weather::Rain: {
            ImVec2 tail;
            if (Helpers::worldToScreen(position - p.velocity * 0.02f, tail))
                drawList->AddLine(tail, screen, tint(edge * 0.55f), std::max(1.0f, size * 0.35f));
            break;
        }
        case Weather::Fireflies: {
            const float flicker = 0.55f + 0.45f * std::sin(realtime * 3.0f + p.phase * 4.0f);
            drawList->AddCircleFilled(screen, size * 3.2f, tint(edge * flicker * 0.12f), 16);
            drawList->AddCircleFilled(screen, size * 0.9f, tint(edge * flicker), 8);
            break;
        }
        default:
            break;
        }
    }
}

}

void stepWeather(const EngineInterfaces& engineInterfaces, const Memory& memory) noexcept
{
    std::scoped_lock lock{ weatherMutex };
    const auto& w = config.world;
    const auto kind = static_cast<Weather>(w.weather);
    if (kind == Weather::Off || !localPlayer) {
        weatherParticles.clear();
        return;
    }
    const float realtime = memory.globalVars->realtime;
    const float dt = memory.globalVars->intervalPerTick;
    const auto& engineTrace = engineInterfaces.engineTrace();
    weatherEye = localPlayer.get().getEyePosition();
    const float radius = w.particleRadius;
    const auto count = static_cast<std::size_t>(std::clamp(w.particles, 20, 800));
    if (static_cast<int>(kind) != weatherKind) {
        weatherParticles.clear();
        weatherKind = static_cast<int>(kind);
    }
    while (weatherParticles.size() < count) {
        weatherParticles.emplace_back();
        spawnWeather(weatherParticles.back(), kind, radius, false);
    }
    weatherParticles.resize(count);

    const bool needsSky = kind != Weather::Fireflies;
    for (auto& p : weatherParticles) {
        p.velocity = weatherVelocity(kind, p, realtime);
        p.position += p.velocity * dt;

        // держим облако вокруг камеры: ушла за край — переносим на другую сторону
        bool moved = false;
        for (int axis = 0; axis < 2; ++axis) {
            float& c = axis == 0 ? p.position.x : p.position.y;
            const float center = axis == 0 ? weatherEye.x : weatherEye.y;
            if (c - center > radius) { c -= radius * 2.0f; moved = true; }
            else if (center - c > radius) { c += radius * 2.0f; moved = true; }
        }
        if (kind == Weather::Fireflies) {
            if (p.position.z - weatherEye.z > radius * 0.25f) { p.position.z -= radius * 0.45f; moved = true; }
            else if (weatherEye.z - p.position.z > radius * 0.3f) { p.position.z += radius * 0.45f; moved = true; }
        } else if (p.position.z < weatherEye.z - radius * 0.45f) {
            spawnWeather(p, kind, radius, true);
        } else if (p.position.z > weatherEye.z + radius * 0.75f) {
            p.position.z -= radius * 1.1f;
            moved = true;
        }
        if (moved)
            p.checked = false;
    }

    // трассы порциями: каждый тик часть проверок «небо над головой» и «видно из глаз»
    int skyBudget = 48;
    for (auto& p : weatherParticles) {
        if (skyBudget <= 0)
            break;
        if (p.checked)
            continue;
        if (needsSky) {
            csgo::Trace trace;
            engineTrace.traceRay({ p.position, { p.position.x, p.position.y, p.position.z + 4096.0f } }, weatherSolidMask, { localPlayer.get().getPOD() }, trace);
            p.outdoor = !trace.startSolid && (trace.fraction >= 0.999f || (trace.surface.flags & weatherSurfSky));
        } else {
            p.outdoor = true;
        }
        p.checked = true;
        --skyBudget;
    }
    static std::size_t cursor = 0;
    const std::size_t visBudget = std::min<std::size_t>(64, weatherParticles.size());
    for (std::size_t i = 0; i < visBudget; ++i) {
        cursor = (cursor + 1) % weatherParticles.size();
        auto& p = weatherParticles[cursor];
        if (!p.checked || !p.outdoor) {
            p.visible = false;
            continue;
        }
        csgo::Trace trace;
        engineTrace.traceRay({ weatherEye, p.position }, weatherSolidMask, { localPlayer.get().getPOD() }, trace);
        p.visible = !trace.startSolid && trace.fraction >= 0.97f;
    }
    weatherSimTime = realtime;
}

void draw(ImDrawList* drawList, float realtime) noexcept
{
    GameData::Lock lock;
    drawWeather(drawList, realtime);
    drawTrail(drawList);
    drawZeusRange(drawList);
    drawTracers(drawList);
    drawGrenadeWarnings(drawList);
    drawOffscreenArrows(drawList, realtime);
    drawServerPosition(drawList);
    for (const auto& player : GameData::players()) {
        drawFlags(drawList, player, realtime);
        drawAimDot(drawList, player, realtime);
    }
    drawShotSkeleton(drawList, realtime);
    drawFootsteps(drawList);
    drawGrenadeTimers(drawList, realtime);
    drawDamageNumbers(drawList);
    drawSpreadCircle(drawList);
    drawExtendedYaw(drawList);
    drawSparks(drawList);
    drawKillFlash(drawList);
    drawHitMarker(drawList);
}

}

namespace hvh::world
{

namespace
{

bool materialsTouched = false;      // на материалах сейчас наши цвета — при выключении вернуть белый
bool cvarsTouched = false;
std::size_t lastKey = 0;
std::chrono::steady_clock::time_point lastPass{};

[[nodiscard]] std::size_t settingsKey(const WorldConfig& w) noexcept
{
    std::size_t hash = 1469598103934665603ull;
    const auto mix = [&hash](float value) {
        std::uint32_t bits;
        std::memcpy(&bits, &value, sizeof(bits));
        hash = (hash ^ bits) * 1099511628211ull;
    };
    for (int i = 0; i < 3; ++i) {
        mix(w.wallColor[i]); mix(w.propColor[i]); mix(w.skyColor[i]); mix(w.fogColor[i]); mix(w.ambientColor[i]);
    }
    mix(w.propAlpha); mix(w.fogStart); mix(w.fogEnd); mix(w.fogDensity);
    mix(w.enabled ? 1.0f : 0.0f); mix(w.skyColorOn ? 1.0f : 0.0f); mix(w.fog ? 1.0f : 0.0f); mix(w.ambient ? 1.0f : 0.0f);
    return hash;
}

void setCvar(csgo::Cvar cvar, const char* name, float value) noexcept
{
    if (const auto var = cvar.findVar(name))
        csgo::ConVar::from(retSpoofGadgets->client, var).setValue(value);
}

void setCvar(csgo::Cvar cvar, const char* name, const char* value) noexcept
{
    if (const auto var = cvar.findVar(name))
        csgo::ConVar::from(retSpoofGadgets->client, var).setValue(value);
}

void modulateMaterials(const OtherInterfaces& interfaces, bool on) noexcept
{
    const auto& w = config.world;
    const auto materials = interfaces.getMaterialSystem();
    for (short h = materials.firstMaterial(); h != materials.invalidMaterial(); h = materials.nextMaterial(h)) {
        const auto mat = csgo::Material::from(retSpoofGadgets->client, materials.getMaterial(h));
        if (mat.getPOD() == nullptr || !mat.isPrecached())
            continue;
        const char* group = mat.getTextureGroupName();
        if (group == nullptr)
            continue;
        const std::string_view textureGroup{ group };
        if (textureGroup.starts_with("World")) {
            if (on) mat.colorModulate(w.wallColor[0], w.wallColor[1], w.wallColor[2]);
            else mat.colorModulate(1.0f, 1.0f, 1.0f);
        } else if (textureGroup.starts_with("StaticProp")) {
            if (on) mat.colorModulate(w.propColor[0], w.propColor[1], w.propColor[2]);
            else mat.colorModulate(1.0f, 1.0f, 1.0f);
            mat.alphaModulate(on ? w.propAlpha : 1.0f);
        } else if (textureGroup.starts_with("SkyBox")) {
            if (on && w.skyColorOn) mat.colorModulate(w.skyColor[0], w.skyColor[1], w.skyColor[2]);
            else mat.colorModulate(1.0f, 1.0f, 1.0f);
        }
    }
}

void applyCvars(csgo::Cvar cvar, bool on) noexcept
{
    const auto& w = config.world;
    const bool fog = on && w.fog;
    setCvar(cvar, "fog_override", fog ? 1.0f : 0.0f);
    if (fog) {
        char color[48];
        std::snprintf(color, sizeof(color), "%d %d %d", static_cast<int>(w.fogColor[0] * 255.0f),
            static_cast<int>(w.fogColor[1] * 255.0f), static_cast<int>(w.fogColor[2] * 255.0f));
        setCvar(cvar, "fog_enable", 1.0f);
        setCvar(cvar, "fog_color", color);
        setCvar(cvar, "fog_start", w.fogStart);
        setCvar(cvar, "fog_end", std::max(w.fogEnd, w.fogStart + 10.0f));
        setCvar(cvar, "fog_maxdensity", w.fogDensity);
        // в 3D-скайбоксе тот же туман, иначе на горизонте виден шов
        setCvar(cvar, "fog_enableskybox", 1.0f);
        setCvar(cvar, "fog_colorskybox", color);
        setCvar(cvar, "fog_startskybox", w.fogStart);
        setCvar(cvar, "fog_endskybox", std::max(w.fogEnd, w.fogStart + 10.0f));
        setCvar(cvar, "fog_maxdensityskybox", w.fogDensity);
    }
    const bool ambient = on && w.ambient;
    setCvar(cvar, "mat_ambient_light_r", ambient ? w.ambientColor[0] : 0.0f);
    setCvar(cvar, "mat_ambient_light_g", ambient ? w.ambientColor[1] : 0.0f);
    setCvar(cvar, "mat_ambient_light_b", ambient ? w.ambientColor[2] : 0.0f);
}

}

void apply(const OtherInterfaces& interfaces, bool inGame) noexcept
{
    const bool on = config.world.enabled && inGame;
    const auto key = settingsKey(config.world);
    const auto now = std::chrono::steady_clock::now();
    const bool changed = key != lastKey;

    if (on || cvarsTouched) {
        if (changed || (on && !cvarsTouched)) {
            applyCvars(interfaces.getCvar(), on);
            cvarsTouched = on;
        }
    }

    if (!inGame) {
        // материалы карты выгружаются вместе с ней — на следующей карте начнём заново
        materialsTouched = false;
        lastKey = key;
        return;
    }

    if (on) {
        // новые материалы подгружаются по ходу игры — проходим раз в секунду и сразу при изменении
        if (changed || !materialsTouched || now - lastPass > std::chrono::seconds{ 1 }) {
            modulateMaterials(interfaces, true);
            materialsTouched = true;
            lastPass = now;
        }
    } else if (materialsTouched) {
        modulateMaterials(interfaces, false);
        materialsTouched = false;
    }
    lastKey = key;
}

// ---------- свет: блум/экспозиция, яркость моделей, солнце ----------
namespace
{

struct TonemapOffsets {
    std::uint32_t useMin = 0, useMax = 0, useBloom = 0, min = 0, max = 0, bloom = 0;
    bool ok = false;
};

struct TonemapBackup {
    bool useMin, useMax, useBloom;
    float min, max, bloom;
};

std::unordered_map<csgo::EntityPOD*, TonemapBackup> tonemapBackups;

const TonemapOffsets& tonemapOffsets() noexcept
{
    static TonemapOffsets o = [] {
        TonemapOffsets t;
        t.useMin = Netvars::get(fnv::hash("CEnvTonemapController->m_bUseCustomAutoExposureMin"));
        t.useMax = Netvars::get(fnv::hash("CEnvTonemapController->m_bUseCustomAutoExposureMax"));
        t.useBloom = Netvars::get(fnv::hash("CEnvTonemapController->m_bUseCustomBloomScale"));
        t.min = Netvars::get(fnv::hash("CEnvTonemapController->m_flCustomAutoExposureMin"));
        t.max = Netvars::get(fnv::hash("CEnvTonemapController->m_flCustomAutoExposureMax"));
        t.bloom = Netvars::get(fnv::hash("CEnvTonemapController->m_flCustomBloomScale"));
        t.ok = t.useMin && t.useMax && t.useBloom && t.min && t.max && t.bloom;
        return t;
    }();
    return o;
}

template <typename T>
T& tonemapField(csgo::EntityPOD* entity, std::uint32_t offset) noexcept
{
    return *reinterpret_cast<T*>(reinterpret_cast<std::uintptr_t>(entity) + offset);
}

[[nodiscard]] bool isTonemapController(const ClientInterfaces& clientInterfaces, csgo::EntityPOD* pod) noexcept
{
    if (!pod)
        return false;
    const auto clientClass = csgo::Entity::from(retSpoofGadgets->client, pod).getNetworkable().getClientClass();
    return clientClass && clientClass->networkName && std::strcmp(clientClass->networkName, "CEnvTonemapController") == 0;
}

}

void applyLight(const ClientInterfaces& clientInterfaces, const OtherInterfaces& interfaces, bool inGame) noexcept
{
    const auto& w = config.world;
    const auto cvar = interfaces.getCvar();

    // яркость моделей: r_modelAmbientMin (выключили — 0, как у игры по умолчанию)
    {
        static bool forced = false;
        if (const auto var = cvar.findVar("r_modelAmbientMin")) {
            const auto cv = csgo::ConVar::from(retSpoofGadgets->client, var);
            if (w.modelBrightness) {
                if (std::abs(cv.getFloat() - w.modelAmbient) > 0.001f)
                    cv.setValue(w.modelAmbient);
                forced = true;
            } else if (forced) {
                cv.setValue(0.0f);
                forced = false;
            }
        }
    }

    // солнце: динамические тени карты падают оттуда, куда скажем (cl_csm_rot_*)
    {
        static bool forced = false;
        static float lastPitch = -1.0f, lastYaw = -1000.0f;
        if (w.sunOverride) {
            if (!forced || lastPitch != w.sunPitch || lastYaw != w.sunYaw) {
                setCvar(cvar, "cl_csm_rot_override", 1.0f);
                setCvar(cvar, "cl_csm_rot_x", w.sunPitch);
                setCvar(cvar, "cl_csm_rot_y", w.sunYaw);
                setCvar(cvar, "cl_csm_rot_z", 0.0f);
                lastPitch = w.sunPitch;
                lastYaw = w.sunYaw;
                forced = true;
            }
        } else if (forced) {
            setCvar(cvar, "cl_csm_rot_override", 0.0f);
            forced = false;
        }
    }

    const auto& o = tonemapOffsets();
    if (!o.ok)
        return;
    if (!inGame) {
        tonemapBackups.clear();
        return;
    }
    // перебирать сущности есть смысл, только когда включено или надо вернуть как было
    if (!w.tonemap && tonemapBackups.empty())
        return;

    // контроллер ищем раз в секунду, между поисками — по запомненным индексам
    static std::vector<int> found;
    static double nextScan = 0.0;
    const double now = ImGui::GetTime();
    const auto& entityList = clientInterfaces.getEntityList();
    if (now >= nextScan || found.empty()) {
        nextScan = now + 1.0;
        found.clear();
        const int highest = entityList.getHighestEntityIndex();
        for (int i = 65; i <= highest; ++i) {
            if (isTonemapController(clientInterfaces, entityList.getEntity(i)))
                found.push_back(i);
        }
    }
    for (const int i : found) {
        const auto pod = entityList.getEntity(i);
        if (!isTonemapController(clientInterfaces, pod))
            continue;
        if (w.tonemap) {
            if (!tonemapBackups.contains(pod)) {
                tonemapBackups[pod] = { tonemapField<bool>(pod, o.useMin), tonemapField<bool>(pod, o.useMax), tonemapField<bool>(pod, o.useBloom),
                    tonemapField<float>(pod, o.min), tonemapField<float>(pod, o.max), tonemapField<float>(pod, o.bloom) };
            }
            tonemapField<bool>(pod, o.useMin) = true;
            tonemapField<bool>(pod, o.useMax) = true;
            tonemapField<bool>(pod, o.useBloom) = true;
            tonemapField<float>(pod, o.min) = w.exposure;
            tonemapField<float>(pod, o.max) = w.exposure;
            tonemapField<float>(pod, o.bloom) = w.bloom;
        } else if (const auto it = tonemapBackups.find(pod); it != tonemapBackups.end()) {
            const auto& b = it->second;
            tonemapField<bool>(pod, o.useMin) = b.useMin;
            tonemapField<bool>(pod, o.useMax) = b.useMax;
            tonemapField<bool>(pod, o.useBloom) = b.useBloom;
            tonemapField<float>(pod, o.min) = b.min;
            tonemapField<float>(pod, o.max) = b.max;
            tonemapField<float>(pod, o.bloom) = b.bloom;
            tonemapBackups.erase(it);
        }
    }
}

bool fogActive() noexcept
{
    return config.world.enabled && config.world.fog;
}

}

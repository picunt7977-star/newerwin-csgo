#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <random>
#include <string>
#include <vector>

#include <imgui/imgui.h>

#include "AntiAim.h"
#include "Extras.h"
#include "HvhConfig.h"
#include "HvhGui.h"
#include "Movement.h"
#include "Ragebot.h"
#include "Defensive.h"
#include "Tickbase.h"

#include <GameData.h>
#include <Memory.h>
#include <Helpers.h>
#include <Interfaces/OtherInterfaces.h>
#include <CSGO/ConVar.h>
#include <CSGO/Engine.h>
#include <CSGO/Cvar.h>
#include <CSGO/Engine.h>
#include <CSGO/Engine.h>
#include <CSGO/Entity.h>
#include <CSGO/LocalPlayer.h>
#include <CSGO/Vector.h>
#include <RetSpoof/RetSpoofGadgets.h>

namespace hvh::extras
{

namespace
{

// ---------- квары ----------

struct CvarState {
    const char* name;
    csgo::ConVarPOD* pod = nullptr;
    float original = 0.0f;  // значение до нас — вернуть при выключении
    float current = 0.0f;   // что сейчас выставлено (плавный переход)
    bool touched = false;   // мы меняли квар
};

std::array<CvarState, 5> cvars{ { { "r_aspectratio" }, { "viewmodel_fov" }, { "viewmodel_offset_x" }, { "viewmodel_offset_y" }, { "viewmodel_offset_z" } } };

// Плавно к цели (как в hysteria: lerp ~8/с), при выключении — обратно к исходному и отпускаем
void drive(const OtherInterfaces& interfaces, CvarState& state, bool enabled, float target, float frameTime) noexcept
{
    if (!state.pod) {
        state.pod = interfaces.getCvar().findVar(state.name);
        if (!state.pod)
            return;
    }
    const auto cvar = csgo::ConVar::from(retSpoofGadgets->client, state.pod);
    if (!state.touched) {
        if (!enabled)
            return;
        state.original = cvar.getFloat();
        state.current = state.original;
        state.touched = true;
    }
    const float goal = enabled ? target : state.original;
    state.current += (goal - state.current) * std::min(1.0f, frameTime * 8.0f);
    if (std::abs(goal - state.current) < 0.002f)
        state.current = goal;
    cvar.setValue(state.current);
    if (!enabled && state.current == state.original)
        state.touched = false;
}

// ---------- индикаторы ----------

std::mutex mutex;
struct Marker {
    csgo::Vector position;
    bool hit;
    double at;
};
std::vector<Marker> markers;     // под mutex
int minDamage = 0;
bool minDamageOverridden = false;

float lastTalk = -100.0f;

void shadowText(ImDrawList* d, ImVec2 position, ImU32 color, const char* text) noexcept
{
    d->AddText({ position.x + 1.0f, position.y + 1.0f }, IM_COL32(0, 0, 0, 200), text);
    d->AddText(position, color, text);
}

ImU32 withAlpha(ImU32 color, float alpha) noexcept
{
    const auto a = static_cast<int>(((color >> IM_COL32_A_SHIFT) & 0xFF) * std::clamp(alpha, 0.0f, 1.0f));
    return (color & ~IM_COL32_A_MASK) | (static_cast<ImU32>(a) << IM_COL32_A_SHIFT);
}

// рамка как у ватермарки
void box(ImDrawList* d, ImVec2 pos, ImVec2 size) noexcept
{
    const ImVec2 max{ pos.x + size.x, pos.y + size.y };
    d->AddRectFilled(pos, max, IM_COL32(0, 0, 0, 255));
    d->AddRectFilled({ pos.x + 1.0f, pos.y + 1.0f }, { max.x - 1.0f, max.y - 1.0f }, IM_COL32(60, 60, 60, 255));
    d->AddRectFilled({ pos.x + 2.0f, pos.y + 2.0f }, { max.x - 2.0f, max.y - 2.0f }, IM_COL32(19, 19, 19, 235));
    const float mid = (pos.x + max.x) * 0.5f;
    d->AddRectFilledMultiColor({ pos.x + 2.0f, pos.y + 2.0f }, { mid, pos.y + 3.0f }, IM_COL32(55, 177, 218, 255), IM_COL32(204, 70, 205, 255), IM_COL32(204, 70, 205, 255), IM_COL32(55, 177, 218, 255));
    d->AddRectFilledMultiColor({ mid, pos.y + 2.0f }, { max.x - 2.0f, pos.y + 3.0f }, IM_COL32(204, 70, 205, 255), IM_COL32(204, 227, 54, 255), IM_COL32(204, 227, 54, 255), IM_COL32(204, 70, 205, 255));
}

void drawScope(ImDrawList* d, ImVec2 center) noexcept
{
    const auto& v = config.view;
    const float gap = static_cast<float>(v.scopeGap);
    const float size = static_cast<float>(v.scopeSize);
    const ImU32 solid = ui::color::accent;
    const ImU32 clear = withAlpha(ui::color::accent, 0.0f);
    // от центра наружу гаснет
    d->AddRectFilledMultiColor({ center.x + gap, center.y }, { center.x + gap + size, center.y + 1.0f }, solid, clear, clear, solid);
    d->AddRectFilledMultiColor({ center.x - gap - size, center.y }, { center.x - gap, center.y + 1.0f }, clear, solid, solid, clear);
    d->AddRectFilledMultiColor({ center.x, center.y + gap }, { center.x + 1.0f, center.y + gap + size }, solid, solid, clear, clear);
    if (!v.scopeT)
        d->AddRectFilledMultiColor({ center.x, center.y - gap - size }, { center.x + 1.0f, center.y - gap }, clear, clear, solid, solid);
}

void drawMarkers(ImDrawList* d) noexcept
{
    const double now = ImGui::GetTime();
    const float life = std::max(config.view.shotMarkerTime, 0.2f);
    std::vector<Marker> copy;
    {
        std::scoped_lock lock{ mutex };
        std::erase_if(markers, [now, life](const Marker& m) { return now - m.at > life; });
        copy = markers;
    }
    for (const auto& m : copy) {
        if (!m.hit && !config.view.missMarkers)
            continue;
        ImVec2 screen;
        if (!Helpers::worldToScreen(m.position, screen))
            continue;
        const float t = static_cast<float>(now - m.at) / life;
        const float alpha = 1.0f - t * t;
        constexpr float s = 4.0f;
        if (m.hit) {
            const ImU32 col = withAlpha(ui::color::accent, alpha);
            d->AddLine({ screen.x - s, screen.y }, { screen.x + s + 1.0f, screen.y }, withAlpha(IM_COL32(0, 0, 0, 255), alpha), 3.0f);
            d->AddLine({ screen.x, screen.y - s }, { screen.x, screen.y + s + 1.0f }, withAlpha(IM_COL32(0, 0, 0, 255), alpha), 3.0f);
            d->AddLine({ screen.x - s, screen.y }, { screen.x + s + 1.0f, screen.y }, col, 1.0f);
            d->AddLine({ screen.x, screen.y - s }, { screen.x, screen.y + s + 1.0f }, col, 1.0f);
        } else {
            const ImU32 col = withAlpha(ui::color::bad, alpha);
            d->AddLine({ screen.x - s, screen.y - s }, { screen.x + s, screen.y + s }, col, 1.5f);
            d->AddLine({ screen.x - s, screen.y + s }, { screen.x + s, screen.y - s }, col, 1.5f);
        }
    }
}

void sanitize(std::string& text) noexcept
{
    // кавычки и ; ломают консольную команду say
    std::erase_if(text, [](char c) { return c == '"' || c == ';' || c == '\n' || c == '\r'; });
    if (text.size() > 24)
        text.resize(24);
}

}

void update(const OtherInterfaces& interfaces, float frameTime) noexcept
{
    const auto& v = config.view;
    drive(interfaces, cvars[0], v.aspectRatio, std::clamp(v.aspectRatioValue, 0.8f, 2.4f), frameTime);
    drive(interfaces, cvars[1], v.viewmodel, static_cast<float>(std::clamp(v.viewmodelFov, 40, 120)), frameTime);
    drive(interfaces, cvars[2], v.viewmodel, v.viewmodelX, frameTime);
    drive(interfaces, cvars[3], v.viewmodel, v.viewmodelY, frameTime);
    drive(interfaces, cvars[4], v.viewmodel, v.viewmodelZ, frameTime);
}

void onShotResult(const csgo::Vector& position, bool hit) noexcept
{
    if (!config.view.shotMarkers)
        return;
    std::scoped_lock lock{ mutex };
    if (markers.size() > 32)
        markers.erase(markers.begin());
    markers.push_back({ position, hit, ImGui::GetTime() });
}

void setMinDamage(int value, bool overridden) noexcept
{
    minDamage = value;
    minDamageOverridden = overridden;
}

int currentMinDamage() noexcept
{
    return minDamage;
}

void onDeath(const csgo::Engine& engine, int attacker, int victim, int local, bool headshot) noexcept
{
    const auto& v = config.view;
    if (!v.trashTalk || local <= 0)
        return;
    const bool kill = attacker == local && victim != local;
    const bool death = victim == local && attacker != local && attacker > 0;
    if (!(kill && (v.trashTalkWhen & 1)) && !(death && (v.trashTalkWhen & 2)))
        return;
    // не чаще раза в 1.5 с: сервер кикает за флуд, а на мультикилле хватит одной фразы
    const float now = static_cast<float>(ImGui::GetTime());
    if (now - lastTalk < 1.5f)
        return;
    lastTalk = now;

    csgo::PlayerInfo info{};
    std::string name = engine.getPlayerInfo(kill ? victim : attacker, info) ? info.name : "";
    sanitize(name);
    if (name.empty())
        name = "бот";

    // в каждой фразе не больше одного %s (ник), других % нет
    static constexpr const char* kills[]{
        "1 by newerwin",
        "%s, резольвер newerwin передаёт привет",
        "сорри %s, newerwin не промахивается",
        "%s, купи newerwin и приходи ещё",
        "твой AA не помог, %s. newerwin видит всё",
        "newerwin > твой чит",
        "%s отправлен в спектры. спонсор — newerwin",
        "ez. newerwin.",
        "%s, твой десинк newerwin читает как книжку",
        "минус %s. newerwin не устаёт",
        "%s, может, хватит пастить? бери newerwin",
        "это был не скилл, %s, это newerwin",
        "%s, на что ты играешь? newerwin в описании нет",
        "newerwin: попадает туда, где ты прячешься",
        "%s, джиттер не спасает от newerwin",
        "%s, спи спокойно. newerwin рядом",
        "очередной клиент newerwin-а уехал в лобби",
        "%s, твой резольвер хуже, чем у newerwin",
        "%s, newerwin не верит в фейки",
        "и снова newerwin. и снова ты, %s",
        "%s, refund свой чит, ставь newerwin",
        "newerwin on top, %s on bottom",
        "%s, ты бы хоть дефенсив включил",
        "тихо, %s. newerwin работает",
    };
    static constexpr const char* headshots[]{
        "hs by newerwin",
        "%s, голова — это к newerwin",
        "тап в голову. newerwin.",
        "%s, твоя голова теперь в логах newerwin",
        "newerwin нашёл твою голову, %s",
        "%s, фристенд не помог. hs by newerwin",
        "голова %s — 1, newerwin — 1. счёт в нашу",
        "%s, где голова? у newerwin в прицеле",
        "one tap. newerwin.",
        "%s, твой AA не прятал голову от newerwin",
        "хедшот из newerwin — без шансов, %s",
    };
    static constexpr const char* deaths[]{
        "повезло тебе, %s. newerwin запомнил",
        "newerwin просто разогревается",
        "ок %s, раунд твой. newerwin вернётся",
        "даже newerwin иногда даёт фору",
        "%s, наслаждайся, пока newerwin перезаряжается",
        "%s, это был подарок от newerwin",
        "newerwin поддался. не привыкай, %s",
        "%s, запомни этот момент. больше не повторится",
        "лаги. newerwin тут ни при чём",
        "%s, твой чит сегодня в ударе. newerwin завтра",
        "newerwin записал тебя, %s. жди ответку",
        "сервер подыграл тебе, %s. newerwin знает",
        "%s, один раз — случайность. newerwin вернётся",
        "newerwin в режиме благотворительности",
    };
    static std::mt19937 rng{ std::random_device{}() };
    const char* const* pool = kill ? (headshot ? headshots : kills) : deaths;
    const std::size_t size = kill ? (headshot ? std::size(headshots) : std::size(kills)) : std::size(deaths);
    // одна и та же фраза два раза подряд не идёт
    static std::size_t lastPick[3]{ 99, 99, 99 };
    auto& last = lastPick[kill ? (headshot ? 1 : 0) : 2];
    std::size_t pick = std::uniform_int_distribution<std::size_t>{ 0, size - 1 }(rng);
    if (pick == last)
        pick = (pick + 1) % size;
    last = pick;
    const char* phrase = pool[pick];

    char message[160];
    std::snprintf(message, sizeof(message), phrase, name.c_str());
    char command[192];
    std::snprintf(command, sizeof(command), "say \"%s\"", message);
    engine.clientCmdUnrestricted(command);
}

void draw(ImDrawList* d) noexcept
{
    const auto& v = config.view;
    if (v.shotMarkers)
        drawMarkers(d);

    if (!localPlayer || !localPlayer.get().isAlive())
        return;
    const auto& display = ImGui::GetIO().DisplaySize;
    const ImVec2 center{ display.x * 0.5f, display.y * 0.5f };

    if (v.customScope && localPlayer.get().isScoped())
        drawScope(d, center);

    if (v.damageIndicator && config.rage.enabled && minDamage > 0) {
        char text[16];
        if (minDamage > 100)
            std::snprintf(text, sizeof(text), "HP+%d", minDamage - 100);
        else
            std::snprintf(text, sizeof(text), "%d", minDamage);
        shadowText(d, { center.x + 8.0f, center.y - 8.0f - ImGui::GetFontSize() }, minDamageOverridden ? ui::color::accent : withAlpha(ui::color::text, 0.7f), text);
    }

    if (v.slowdownWarning) {
        const float modifier = localPlayer.get().velocityModifier();
        if (modifier < 0.99f && modifier >= 0.0f) {
            char text[48];
            std::snprintf(text, sizeof(text), "замедление %.0f%%", (1.0f - modifier) * 100.0f);
            const float width = 120.0f;
            const ImVec2 at{ center.x - width * 0.5f, center.y - display.y * 0.22f };
            const auto size = ImGui::CalcTextSize(text);
            shadowText(d, { center.x - size.x * 0.5f, at.y }, IM_COL32(255, 255, 255, 255), text);
            const float barY = at.y + size.y + 3.0f;
            d->AddRectFilled({ at.x - 1.0f, barY - 1.0f }, { at.x + width + 1.0f, barY + 4.0f }, IM_COL32(0, 0, 0, 200));
            d->AddRectFilled({ at.x, barY }, { at.x + width * modifier, barY + 3.0f }, ui::lerpColor(ui::color::bad, ui::color::accent, modifier));
        }
    }

    if (v.lcIndicator) {
        const int left = defensive::windowLeft();
        const char* text = "LC";
        const auto size = ImGui::CalcTextSize(text);
        const ImVec2 at{ center.x - size.x * 0.5f, center.y + display.y * 0.18f };
        shadowText(d, at, left > 0 ? ui::color::good : withAlpha(ui::color::text, 0.35f), text);
        if (left > 0) {
            const float width = 40.0f;
            const float barY = at.y + size.y + 2.0f;
            d->AddRectFilled({ center.x - width * 0.5f - 1.0f, barY - 1.0f }, { center.x + width * 0.5f + 1.0f, barY + 3.0f }, IM_COL32(0, 0, 0, 200));
            d->AddRectFilled({ center.x - width * 0.5f, barY }, { center.x - width * 0.5f + width * std::min(left / 14.0f, 1.0f), barY + 2.0f }, ui::color::good);
        }
    }
}

}

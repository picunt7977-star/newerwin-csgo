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
#include "Hud.h"
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

using hud::withAlpha;

void drawScope(ImDrawList* d, ImVec2 center) noexcept
{
    const auto& v = config.view;
    const float gap = static_cast<float>(v.scopeGap);
    const float size = static_cast<float>(v.scopeSize);
    const ImU32 solid = ui::color::accent;
    const ImU32 clear = withAlpha(ui::color::accent, 0.0f);
    // обычный: от центра наружу гаснет; инвертор — то же самое наоборот: у центра прозрачно, к концу ярко
    const ImU32 inner = v.scopeInvert ? clear : solid;
    const ImU32 outer = v.scopeInvert ? solid : clear;
    d->AddRectFilledMultiColor({ center.x + gap, center.y }, { center.x + gap + size, center.y + 1.0f }, inner, outer, outer, inner);
    d->AddRectFilledMultiColor({ center.x - gap - size, center.y }, { center.x - gap, center.y + 1.0f }, outer, inner, inner, outer);
    d->AddRectFilledMultiColor({ center.x, center.y + gap }, { center.x + 1.0f, center.y + gap + size }, inner, inner, outer, outer);
    if (!v.scopeT)
        d->AddRectFilledMultiColor({ center.x, center.y - gap - size }, { center.x + 1.0f, center.y - gap }, outer, outer, inner, inner);
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
            // попадание — зелёный плюс
            hud::line(d, { screen.x - s, screen.y }, { screen.x + s + 1.0f, screen.y }, hud::color::good, alpha, 1.0f);
            hud::line(d, { screen.x, screen.y - s }, { screen.x, screen.y + s + 1.0f }, hud::color::good, alpha, 1.0f);
        } else {
            // пуля прошла мимо — красный крестик
            hud::line(d, { screen.x - s, screen.y - s }, { screen.x + s, screen.y + s }, hud::color::bad, alpha, 1.0f);
            hud::line(d, { screen.x - s, screen.y + s }, { screen.x + s, screen.y - s }, hud::color::bad, alpha, 1.0f);
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

    // ---- индикаторы у прицела: один стиль (Hud.h), всё по центру, сверху вниз ----
    ImFont* font = hud::smallFont();

    // мин. урон — справа-сверху от прицела; цветом акцента, когда его держит бинд
    if (v.damageIndicator && config.rage.enabled && minDamage > 0) {
        char text[16];
        if (minDamage > 100)
            std::snprintf(text, sizeof(text), "HP+%d", minDamage - 100);
        else
            std::snprintf(text, sizeof(text), "%d", minDamage);
        hud::text(d, font, { center.x + 8.0f, center.y - 8.0f - font->FontSize }, minDamageOverridden ? hud::accent() : hud::color::dim, text);
    }

    // замедление после попадания по нам — над прицелом: подпись и полоска «сколько скорости осталось»
    if (v.slowdownWarning) {
        const float modifier = localPlayer.get().velocityModifier();
        if (modifier < 0.99f && modifier >= 0.0f) {
            char text[48];
            std::snprintf(text, sizeof(text), "SLOW %.0f%%", (1.0f - modifier) * 100.0f);
            constexpr float width = 96.0f;
            const float y = center.y - display.y * 0.22f;
            hud::textCentered(d, font, { center.x, y }, hud::color::warn, text);
            hud::bar(d, { center.x - width * 0.5f, y + font->FontSize + 4.0f }, width, 3.0f, modifier, ui::lerpColor(hud::color::bad, hud::color::good, modifier));
        }
    }

    // окно дефенсива — под прицелом: «LC» зелёным и полоска, пока сервер не пишет по нам лагкомп
    if (v.lcIndicator) {
        const int left = defensive::windowLeft();
        const float y = center.y + display.y * 0.18f;
        hud::textCentered(d, font, { center.x, y }, left > 0 ? hud::color::good : withAlpha(hud::color::dim, 0.5f), "LC");
        if (left > 0) {
            constexpr float width = 40.0f;
            hud::bar(d, { center.x - width * 0.5f, y + font->FontSize + 3.0f }, width, 3.0f, static_cast<float>(left) / 14.0f, hud::color::good);
        }
    }
}

}

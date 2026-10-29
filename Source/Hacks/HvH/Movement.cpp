#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <string>

#include <imgui/imgui.h>

#include "HvhConfig.h"
#include "HvhGui.h"
#include "Movement.h"
#include "Tickbase.h"

#include <GameData.h>
#include <Helpers.h>
#include <CSGO/ConVar.h>
#include <CSGO/Cvar.h>
#include <CSGO/Engine.h>
#include <CSGO/Entity.h>
#include <CSGO/EntityList.h>
#include <Interfaces/ClientInterfaces.h>
#include <Interfaces/EngineInterfaces.h>
#include <Memory.h>
#include <RetSpoof/RetSpoofGadgets.h>
#include <CSGO/LocalPlayer.h>
#include <CSGO/UserCmd.h>

namespace hvh::movement
{

namespace
{

bool fakeDucking = false;

bool peekActive = false;
bool peekReturning = false;
csgo::Vector peekOrigin;

[[nodiscard]] float normalizeYaw(float yaw) noexcept
{
    yaw = std::remainder(yaw, 360.0f);
    return yaw;
}

}

void bunnyHop(csgo::UserCmd* cmd) noexcept
{
    if (!config.movement.bunnyHop || !localPlayer || !localPlayer.get().isAlive())
        return;
    const auto moveType = localPlayer.get().moveType();
    if (moveType == MoveType::LADDER || moveType == MoveType::NOCLIP)
        return;
    // в воздухе прыжок отпускаем — тогда в тик приземления он снова «нажат» и прыжок без потери скорости
    if (!localPlayer.get().isOnGround())
        cmd->buttons &= ~csgo::UserCmd::IN_JUMP;
}

void autoStrafe(csgo::UserCmd* cmd, float viewYaw, const csgo::Cvar& cvar, float interval) noexcept
{
    const auto mode = static_cast<AutoStrafe>(config.movement.autoStrafe);
    // тик приземления с нажатым прыжком — уже воздух: прыжок раньше трения, дальше AirMove
    if (mode == AutoStrafe::Off || !localPlayer || !localPlayer.get().isAlive()
        || (localPlayer.get().isOnGround() && !(cmd->buttons & csgo::UserCmd::IN_JUMP)))
        return;
    const auto moveType = localPlayer.get().moveType();
    if (moveType == MoveType::LADDER || moveType == MoveType::NOCLIP)
        return;

    using csgo::UserCmd;
    const float forwardKey = ((cmd->buttons & UserCmd::IN_FORWARD) ? 1.0f : 0.0f) - ((cmd->buttons & UserCmd::IN_BACK) ? 1.0f : 0.0f);
    const float rightKey = ((cmd->buttons & UserCmd::IN_MOVERIGHT) ? 1.0f : 0.0f) - ((cmd->buttons & UserCmd::IN_MOVELEFT) ? 1.0f : 0.0f);

    // куда лететь
    float targetYaw = viewYaw;
    if (mode == AutoStrafe::Keys) {
        if (forwardKey != 0.0f || rightKey != 0.0f)
            targetYaw += Helpers::rad2deg(std::atan2(-rightKey, forwardKey));
    } else if (forwardKey < 0.0f) {
        targetYaw += 180.0f;
    }

    const auto velocity = localPlayer.get().velocity();
    const float speed = std::hypot(velocity.x, velocity.y);

    float wishYaw;
    if (speed < 30.0f) {
        wishYaw = targetYaw;
    } else {
        // AirAccelerate: прибавка = min(accel, 30 - v·cosφ), где accel = sv_airaccelerate * 250 * tick.
        // Если accel >= 30 — лучший угол 90° к скорости, иначе acos((30 - accel) / v)
        static const auto airAccelerateVar = cvar.findVar("sv_airaccelerate");
        const float airAccelerate = airAccelerateVar ? csgo::ConVar::from(retSpoofGadgets->client, airAccelerateVar).getFloat() : 12.0f;
        const float accel = airAccelerate * 250.0f * interval;
        const float best = Helpers::rad2deg(std::acos(std::clamp((30.0f - accel) / speed, 0.0f, 1.0f)));
        // на сколько градусов скорость поворачивается за тик при таком вводе
        const float turnPerTick = Helpers::rad2deg(std::atan2(std::min(30.0f, accel), speed));

        const float velocityYaw = Helpers::rad2deg(std::atan2(velocity.y, velocity.x));
        const float delta = std::remainder(targetYaw - velocityYaw, 360.0f);

        static bool flip = false;
        flip = !flip;
        // почти в нужную сторону — зигзаг (чистый набор скорости), иначе доворачиваем
        const float side = std::abs(delta) <= turnPerTick ? (flip ? 1.0f : -1.0f) : (delta > 0.0f ? 1.0f : -1.0f);
        wishYaw = velocityYaw + side * best;
    }

    const float relative = Helpers::deg2rad(wishYaw - viewYaw);
    cmd->forwardmove = std::cos(relative) * 450.0f;
    cmd->sidemove = -std::sin(relative) * 450.0f;
}

void fakeDuck(csgo::UserCmd* cmd) noexcept
{
    const auto& key = config.movement.fakeDuckKey;
    fakeDucking = key.isSet() && key.isDown() && tickbase::isInstalled()
        && localPlayer && localPlayer.get().isAlive() && localPlayer.get().isOnGround();
    tickbase::setFakeDuck(fakeDucking);
    if (!fakeDucking)
        return;

    // без задержки приседания, иначе за 7 тиков не успеть
    cmd->buttons |= csgo::UserCmd::IN_BULLRUSH;
    // первую половину чока стоим (с этой высоты и стреляем), вторую — сидим:
    // отправленный тик всегда приходится на присед, его и видят остальные
    if (tickbase::chokedCommands() > 6)
        cmd->buttons |= csgo::UserCmd::IN_DUCK;
    else
        cmd->buttons &= ~csgo::UserCmd::IN_DUCK;
}

void exploits(const EngineInterfaces& engineInterfaces, const ClientInterfaces& clientInterfaces, const Memory& memory) noexcept
{
    static bool teleportWasDown = false;
    static std::array<bool, 65> enemyWasVisible{};

    const auto& ex = config.exploits;
    const bool teleportDown = ex.teleportKey.isSet() && ex.teleportKey.isDown();
    const bool teleportPressed = teleportDown && !teleportWasDown;
    teleportWasDown = teleportDown;

    if (!localPlayer || !localPlayer.get().isAlive() || !tickbase::isInstalled()) {
        enemyWasVisible.fill(false);
        return;
    }

    if (teleportPressed && tickbase::isCharged()) {
        tickbase::requestShift();
        return;
    }

    if (!ex.defensive)
        return;

    // дефенсив: враг только что увидел нас — рывок на заряженные тики. Между двумя пакетами мы
    // смещаемся сразу на десятки юнитов, и его лагкомпенсация/бектрек по нашей прошлой позиции не работает
    bool newlySpotted = false;
    const int maxClients = std::min(engineInterfaces.getEngine().getMaxClients(), 64);
    for (int i = 1; i <= maxClients; ++i) {
        const auto entity = csgo::Entity::from(retSpoofGadgets->client, clientInterfaces.getEntityList().getEntity(i));
        if (!entity.getPOD() || entity.getNetworkable().isDormant() || !entity.isAlive() || !localPlayer.get().isOtherEnemy(memory, entity)) {
            enemyWasVisible[i] = false;
            continue;
        }
        const bool visible = localPlayer.get().visibleTo(engineInterfaces, memory, entity);
        if (visible && !enemyWasVisible[i])
            newlySpotted = true;
        enemyWasVisible[i] = visible;
    }

    // стоя рывок ничего не даёт — нужна скорость
    if (newlySpotted && tickbase::isCharged() && localPlayer.get().velocity().length2D() > 60.0f)
        tickbase::requestShift();
}

namespace
{
bool buyPending = false;
}

void onRoundStart() noexcept
{
    buyPending = true;
}

void autoBuy(const csgo::Engine& engine) noexcept
{
    const auto& buy = config.autoBuy;
    if (!buyPending || !localPlayer || !localPlayer.get().isAlive())
        return;
    buyPending = false;
    if (!buy.enabled)
        return;

    // Предметы своей стороны: чужие (scar20 у T, кусачки у T, молотов у CT) движок отбрасывает с «usage: buy <item>»
    const bool ct = localPlayer.get().getTeamNumber() == csgo::Team::CT;
    static constexpr const char* primariesT[]{ "", "buy ssg08; ", "buy awp; ", "buy g3sg1; " };
    static constexpr const char* primariesCT[]{ "", "buy ssg08; ", "buy awp; ", "buy scar20; " };
    static constexpr const char* secondaries[]{ "", "buy deagle; ", "buy elite; ", "buy p250; ", "" };
    std::string command = (ct ? primariesCT : primariesT)[std::clamp(buy.primary, 0, 3)];
    if (buy.extras & BuyArmor)
        command += "buy vesthelm; buy vest; ";
    const int secondary = std::clamp(buy.secondary, 0, 4);
    command += secondary == 4 ? (ct ? "buy fn57; " : "buy tec9; ") : secondaries[secondary];
    if (buy.extras & BuyTaser)
        command += "buy taser; ";
    if (buy.extras & BuyHe)
        command += "buy hegrenade; ";
    if (buy.extras & BuySmoke)
        command += "buy smokegrenade; ";
    if (buy.extras & BuyMolotov)
        command += ct ? "buy incgrenade; " : "buy molotov; ";
    if (buy.extras & BuyFlash)
        command += "buy flashbang; ";
    if ((buy.extras & BuyDefuser) && ct)
        command += "buy defuser; ";
    if (!command.empty())
        engine.clientCmdUnrestricted(command.c_str());
}

bool isFakeDucking() noexcept
{
    return fakeDucking;
}

void autoPeek(csgo::UserCmd* cmd, float viewYaw, bool shot) noexcept
{
    const auto& key = config.movement.autoPeekKey;
    if (!key.isSet() || !key.isDown() || !localPlayer || !localPlayer.get().isAlive()) {
        peekActive = false;
        peekReturning = false;
        return;
    }

    const auto origin = localPlayer.get().getAbsOrigin();
    if (!peekActive) {
        if (!localPlayer.get().isOnGround())
            return;
        peekActive = true;
        peekReturning = false;
        peekOrigin = origin;
    }

    if (shot)
        peekReturning = true;
    constexpr auto moveButtons = csgo::UserCmd::IN_FORWARD | csgo::UserCmd::IN_BACK | csgo::UserCmd::IN_MOVELEFT | csgo::UserCmd::IN_MOVERIGHT;
    if (config.movement.autoPeekOnRelease && !(cmd->buttons & moveButtons))
        peekReturning = true;

    if (!peekReturning)
        return;

    const float dx = peekOrigin.x - origin.x;
    const float dy = peekOrigin.y - origin.y;
    const float distance = std::hypot(dx, dy);
    if (distance < 5.0f) {
        peekReturning = false;
        return;
    }

    const float yaw = Helpers::rad2deg(std::atan2(dy, dx));
    const float relative = Helpers::deg2rad(yaw - viewYaw);
    cmd->forwardmove = std::cos(relative) * 450.0f;
    cmd->sidemove = -std::sin(relative) * 450.0f;
}

void fixMovement(csgo::UserCmd* cmd, float viewYaw) noexcept
{
    // поворачиваем вектор ввода на разницу между камерой и отправляемым yaw
    const float delta = Helpers::deg2rad(cmd->viewangles.y - viewYaw);
    const float forward = cmd->forwardmove;
    const float side = cmd->sidemove;
    cmd->forwardmove = std::cos(delta) * forward - std::sin(delta) * side;
    cmd->sidemove = std::sin(delta) * forward + std::cos(delta) * side;

    // кнопки под настоящее направление: по ним анимстейт решает про стрейф ног,
    // а с AA WASD не совпадает с вводом и слой стрейфа дёргается (слайдвок их потом сам переворачивает)
    using csgo::UserCmd;
    cmd->buttons &= ~(UserCmd::IN_FORWARD | UserCmd::IN_BACK | UserCmd::IN_MOVELEFT | UserCmd::IN_MOVERIGHT);
    if (cmd->forwardmove > 1.0f) cmd->buttons |= UserCmd::IN_FORWARD;
    else if (cmd->forwardmove < -1.0f) cmd->buttons |= UserCmd::IN_BACK;
    if (cmd->sidemove > 1.0f) cmd->buttons |= UserCmd::IN_MOVERIGHT;
    else if (cmd->sidemove < -1.0f) cmd->buttons |= UserCmd::IN_MOVELEFT;

    // слайдвок: кнопки наоборот — анимация ног идёт «в обратку»
    if (config.antiAim.enabled && config.antiAim.slideWalk && localPlayer && localPlayer.get().isOnGround()) {
        const auto swap = [cmd](int a, int b) {
            const bool hasA = cmd->buttons & a;
            const bool hasB = cmd->buttons & b;
            cmd->buttons &= ~(a | b);
            if (hasA) cmd->buttons |= b;
            if (hasB) cmd->buttons |= a;
        };
        swap(UserCmd::IN_FORWARD, UserCmd::IN_BACK);
        swap(UserCmd::IN_MOVELEFT, UserCmd::IN_MOVERIGHT);
    }
}

void fastLadder(csgo::UserCmd* cmd, float viewPitch, float viewYaw) noexcept
{
    if (!config.movement.fastLadder || !localPlayer || !localPlayer.get().isAlive() || localPlayer.get().moveType() != MoveType::LADDER)
        return;

    using csgo::UserCmd;
    const bool up = (cmd->buttons & UserCmd::IN_FORWARD) && !(cmd->buttons & UserCmd::IN_BACK);
    const bool down = (cmd->buttons & UserCmd::IN_BACK) && !(cmd->buttons & UserCmd::IN_FORWARD);
    const int side = ((cmd->buttons & UserCmd::IN_MOVERIGHT) ? 1 : 0) - ((cmd->buttons & UserCmd::IN_MOVELEFT) ? 1 : 0);

    // на лестнице скорость складывается из «вперёд» и «вбок» по отдельности:
    // смотрим вниз и боком к лестнице — обе составляющие уходят в подъём
    if (up && viewPitch < 45.0f) {
        cmd->viewangles.x = 89.0f;
        cmd->viewangles.y = normalizeYaw(viewYaw + (side == 0 ? 90.0f : side < 0 ? 150.0f : 30.0f));
        cmd->buttons &= ~(UserCmd::IN_FORWARD | UserCmd::IN_MOVELEFT);
        cmd->buttons |= UserCmd::IN_BACK | UserCmd::IN_MOVERIGHT;
        cmd->forwardmove = -450.0f;
        cmd->sidemove = 450.0f;
    } else if (down) {
        cmd->viewangles.x = 89.0f;
        cmd->viewangles.y = normalizeYaw(viewYaw + (side == 0 ? 90.0f : side > 0 ? 150.0f : 30.0f));
        cmd->buttons &= ~(UserCmd::IN_BACK | UserCmd::IN_MOVERIGHT);
        cmd->buttons |= UserCmd::IN_FORWARD | UserCmd::IN_MOVELEFT;
        cmd->forwardmove = 450.0f;
        cmd->sidemove = -450.0f;
    }
}

void syncRendering(const csgo::Cvar& cvar) noexcept
{
    // при многопоточном рендере кадр рисуется на кадр позже, а ESP берёт данные то этого, то следующего кадра
    static bool forced = false;
    if (!config.view.syncRender && !forced)
        return;
    static const auto queueMode = cvar.findVar("mat_queue_mode");
    if (!queueMode)
        return;
    const auto var = csgo::ConVar::from(retSpoofGadgets->client, queueMode);
    if (config.view.syncRender) {
        if (var.getInt() != 0)
            var.setValue(0);
        forced = true;
    } else {
        var.setValue(-1);
        forced = false;
    }
}

float scopeZoomFactor() noexcept
{
    if (!config.view.removeScopeZoom || !config.view.secondZoom || !localPlayer || !localPlayer.get().isScoped())
        return 1.0f;
    // первый уровень у всех снайперок 40, второй 15 (у AWP 10)
    const int fov = localPlayer.get().fov();
    if (fov <= 0 || fov >= 30)
        return 1.0f;
    return std::clamp(config.view.secondZoomFactor, 1.0f, 8.0f);
}

void keepZoomSensitivity(const csgo::Engine& engine, const csgo::Cvar& cvar) noexcept
{
    static csgo::Vector last;
    static bool haveLast = false;

    if (!engine.isInGame() || !localPlayer || !localPlayer.get().isAlive()) {
        haveLast = false;
        return;
    }

    auto angles = engine.getViewAngles();
    const int fov = localPlayer.get().fov();
    const int defaultFov = localPlayer.get().defaultFov() > 0 ? localPlayer.get().defaultFov() : 90;

    if (haveLast && config.view.removeScopeZoom && localPlayer.get().isScoped() && fov > 0 && fov < defaultFov) {
        // игра умножает сенсу на zoom_sensitivity_ratio_mouse * (fov / default_fov) — делим обратно
        static const auto ratioVar = cvar.findVar("zoom_sensitivity_ratio_mouse");
        float ratio = ratioVar ? csgo::ConVar::from(retSpoofGadgets->client, ratioVar).getFloat() : 1.0f;
        if (ratio <= 0.0f)
            ratio = 1.0f;
        // на своём втором зуме сенса падает как у игры: ratio / увеличение
        const float zoom = scopeZoomFactor();
        const float factor = zoom > 1.0f
            ? static_cast<float>(defaultFov) / static_cast<float>(fov) / zoom
            : static_cast<float>(defaultFov) / (static_cast<float>(fov) * ratio);

        const float dx = angles.x - last.x;
        const float dy = std::remainder(angles.y - last.y, 360.0f);
        // большие скачки — не мышь (респавн, телепорт, сервер повернул камеру)
        if (std::abs(dx) < 30.0f && std::abs(dy) < 30.0f && (dx != 0.0f || dy != 0.0f)) {
            angles.x = std::clamp(last.x + dx * factor, -89.0f, 89.0f);
            angles.y = normalizeYaw(last.y + dy * factor);
            angles.z = 0.0f;
            engine.setViewAngles(angles);
        }
    }

    last = angles;
    haveLast = true;
}

void draw(ImDrawList* drawList) noexcept
{
    if (!peekActive)
        return;

    // кольцо на полу в точке возврата
    constexpr int segments = 40;
    constexpr float radius = 16.0f;
    ImVec2 points[segments];
    int count = 0;
    {
        GameData::Lock lock;
        for (int i = 0; i < segments; ++i) {
            const float a = 2.0f * std::numbers::pi_v<float> * i / segments;
            if (!Helpers::worldToScreen({ peekOrigin.x + std::cos(a) * radius, peekOrigin.y + std::sin(a) * radius, peekOrigin.z + 1.0f }, points[count]))
                return;
            ++count;
        }
    }
    const ImU32 col = peekReturning ? ui::color::accent2 : ui::color::accent;
    drawList->AddConvexPolyFilled(points, count, (col & 0x00FFFFFF) | (50u << 24));
    drawList->AddPolyline(points, count, col, ImDrawFlags_Closed, 2.0f);
}

}

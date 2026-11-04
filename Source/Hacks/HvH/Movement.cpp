#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <numbers>
#include <string>

#include <imgui/imgui.h>

#include "HvhConfig.h"
#include "HvhGui.h"
#include "Movement.h"
#include "Ragebot.h"
#include "Tickbase.h"

#include <GameData.h>
#include <Helpers.h>
#include <CSGO/ConVar.h>
#include <CSGO/Cvar.h>
#include <CSGO/Engine.h>
#include <CSGO/Entity.h>
#include <CSGO/EngineTrace.h>
#include <CSGO/GlobalVars.h>
#include <CSGO/EntityList.h>
#include <Interfaces/ClientInterfaces.h>
#include <Interfaces/EngineInterfaces.h>
#include <Memory.h>
#include <Netvars.h>
#include <RetSpoof/RetSpoofGadgets.h>
#include <CSGO/LocalPlayer.h>
#include <CSGO/UserCmd.h>

namespace hvh::movement
{

namespace
{

bool fakeDucking = false;
float fdSentDuck = -1.0f;   // присед на последнем отправленном тике (то, что видят враги)
float fdLowDuck = -1.0f;    // минимум приседа за цикл (с этой высоты стреляем)

bool peekActive = false;
bool peekReturning = false;
bool peekTeleported = false;    // заряд на этот возврат уже потрачен
int peekReturnTicks = 0;
csgo::Vector peekOrigin;

// Сколько пройдём за сдвиг ticks команд, если каждую жмём к точке: разгон sv_accelerate от текущей скорости
[[nodiscard]] float shiftCoverage(float speed, float maxSpeed, int ticks, float interval) noexcept
{
    float distance = 0.0f;
    for (int i = 0; i < ticks; ++i) {
        speed = std::min(speed + 5.5f * maxSpeed * interval, maxSpeed);
        distance += speed * interval;
    }
    return distance;
}

[[nodiscard]] float normalizeYaw(float yaw) noexcept
{
    return std::remainder(yaw, 360.0f);
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

// Фейкдак. Пачка из N команд (чок), отправляется последняя — только её присед видят враги и лагкомп.
// Первую половину пачки стоим (с этой высоты стреляем), вторую садимся: к отправке модель в полном
// приседе (IN_BULLRUSH снимает задержку приседа, 7 тиков хватает). Чок N — 14, но часть серверов
// обрабатывает за раз меньше команд и выкидывает хвост пачки, то есть как раз присед: враг видит нас
// стоящими через раз («то 100, то 0»). Тогда N уменьшаем, пока отправка не придётся на присед.
void fakeDuck(csgo::UserCmd* cmd, float) noexcept
{
    static int badSends = 0;
    static float cycleLow = 1.0f;

    const auto& key = config.movement.fakeDuckKey;
    fakeDucking = key.isSet() && key.isDown() && tickbase::isInstalled()
        && localPlayer && localPlayer.get().isAlive() && localPlayer.get().isOnGround();
    tickbase::setFakeDuck(fakeDucking);
    if (!fakeDucking) {
        badSends = 0;
        cycleLow = 1.0f;
        fdSentDuck = fdLowDuck = -1.0f;
        return;
    }

    cmd->buttons |= csgo::UserCmd::IN_BULLRUSH;
    const int limit = tickbase::fakeDuckChoke();
    const int choked = tickbase::chokedCommands();
    if (choked >= limit / 2)
        cmd->buttons |= csgo::UserCmd::IN_DUCK;
    else
        cmd->buttons &= ~csgo::UserCmd::IN_DUCK;

    // проверка: прошлая команда ушла — какой присед получил сервер (предсказание поправлено по нему)
    static const auto duckAmountOffset = Netvars::get(fnv::hash("CBasePlayer->m_flDuckAmount"));
    if (!duckAmountOffset)
        return;
    const float amount = std::clamp(*reinterpret_cast<const float*>(std::uintptr_t(localPlayer.get().getPOD()) + duckAmountOffset), 0.0f, 1.0f);
    if (choked == 0) {
        fdSentDuck = amount;
        fdLowDuck = cycleLow;
        cycleLow = 1.0f;
        if (amount < 0.5f) {
            if (++badSends >= 2) {
                tickbase::setFakeDuckChoke(limit - 1);
                badSends = 0;
            }
        } else if (amount >= 0.85f) {
            badSends = 0;
        }
    }
    cycleLow = std::min(cycleLow, amount);
}

namespace
{
// Увидит ли враг нашу голову через `ahead` секунд, если и он, и мы продолжим двигаться как сейчас.
// Его глаз двигаем только по горизонтали (прыжок врага не угадать), свой — по полной скорости.
bool willSee(const EngineInterfaces& engineInterfaces, const Memory& memory, const csgo::Entity& enemy,
             const csgo::Vector& ourEye, const csgo::Vector& ourVelocity, float ahead) noexcept
{
    const auto enemyVelocity = enemy.velocity();
    const csgo::Vector from = enemy.getEyePosition() + csgo::Vector{ enemyVelocity.x * ahead, enemyVelocity.y * ahead, 0.0f };
    const csgo::Vector to = ourEye + ourVelocity * ahead;
    for (const csgo::Vector& point : { to + csgo::Vector{ 0.0f, 0.0f, 5.0f }, to - csgo::Vector{ 0.0f, 0.0f, 20.0f } }) {
        if (memory.lineGoesThroughSmoke(from, point, 1))
            continue;
        csgo::Trace trace;
        engineInterfaces.engineTrace().traceRay({ from, point }, 0x46004009, enemy.getPOD(), trace);
        if (trace.entity == localPlayer.get().getPOD() || trace.fraction > 0.97f)
            return true;
    }
    return false;
}
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
        tickbase::setThreat(false);
        return;
    }

    if (teleportPressed && tickbase::isCharged()) {
        tickbase::requestShift();
        return;
    }

    // Кто нас сейчас видит: для рывка «враг только что увидел» и чтобы не дозаряжаться посреди перестрелки
    bool newlySpotted = false, anyVisible = false;

    const auto& aa = config.antiAim;
    const bool peekCheck = aa.peekDefensive && tickbase::doubleTapEnabled() && tickbase::defensiveAllowed()
        && tickbase::isCharged() && !tickbase::isShifting() && tickbase::defensiveTicksLeft() == 0;
    const float ahead = static_cast<float>(std::clamp(aa.peekTicks, 2, 16)) * memory.globalVars->intervalPerTick;
    const csgo::Vector ourEye = localPlayer.get().getEyePosition();
    const csgo::Vector ourVelocity = localPlayer.get().velocity();
    bool peekSoon = false;
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
        anyVisible = anyVisible || visible;
        enemyWasVisible[i] = visible;
        // пик: сейчас не видит, а через peekTicks тиков (оба продолжают двигаться как сейчас) — увидит
        if (peekCheck && !visible && !peekSoon)
            peekSoon = willSee(engineInterfaces, memory, entity, ourEye, ourVelocity, ahead);
    }
    tickbase::setThreat(anyVisible);

    // Дефенсив на пике: рывок до того, как враг нас увидел (или в тик, когда увидел — тут и стоя:
    // окно скрытых углов, а его лагкомп берёт старую запись). Записи лагкомпа на выглядывании нет —
    // его чит стреляет по нам за стеной. В окне работают снапы дефенсив AA («У стены» — у стены).
    if (peekCheck && (peekSoon || newlySpotted)) {
        static float lastPeekShift = -10.0f;
        const float now = memory.globalVars->realtime;
        if (now < lastPeekShift)
            lastPeekShift = -10.0f;
        if (now - lastPeekShift > 0.5f) {
            // зажат автопик — оставляем 6 тиков на телепорт назад
            const bool peekHeld = config.movement.autoPeekKey.isSet() && config.movement.autoPeekKey.isDown();
            const int charged = tickbase::chargedTicks();
            if (!peekHeld) {
                tickbase::requestShift();
                lastPeekShift = now;
                return;
            }
            if (charged >= 12) {
                tickbase::requestShift(charged - 6);
                lastPeekShift = now;
                return;
            }
        }
    }

    // дефенсив DT рвётся при выглядывании и без отдельной галки «Дефенсив»
    if (!ex.defensive && !tickbase::defensiveDoubleTap())
        return;
    // пока зажат автопик, заряд бережём под телепорт назад
    if (config.movement.autoPeekKey.isSet() && config.movement.autoPeekKey.isDown())
        return;

    // Дефенсив: враг только что увидел нас — рывок на заряженные тики, его лагкомп по нашей прошлой позиции
    // не работает. Не чаще раза в секунду: враг то пропадал, то появлялся, и рывков было по три подряд
    // (лог 01.11 12:53:12), а после каждого 6 тиков зарядки — стоишь и не стреляешь.
    static float lastSpotShift = -10.0f;
    const float now = memory.globalVars->realtime;
    if (now < lastSpotShift)
        lastSpotShift = -10.0f;
    // стоя рывок ничего не даёт — нужна скорость
    if (newlySpotted && now - lastSpotShift > 1.0f && tickbase::isCharged() && tickbase::defensiveAllowed() && localPlayer.get().velocity().length2D() > 60.0f) {
        tickbase::requestShift();
        lastSpotShift = now;
    }
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

void fakeDuckStats(float& sent, float& low) noexcept
{
    sent = fdSentDuck;
    low = fdLowDuck;
}

bool isFakeDucking() noexcept
{
    return fakeDucking;
}

void autoPeek(csgo::UserCmd* cmd, float viewYaw, bool shot, float interval) noexcept
{
    const auto& key = config.movement.autoPeekKey;
    if (!key.isSet() || !key.isDown() || !localPlayer || !localPlayer.get().isAlive()) {
        peekActive = false;
        peekReturning = false;
        tickbase::holdShotShift(false);
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

    // пока зажат автопик, не дозаряжаемся посреди выглядывания и дефенсив заряд не тратит
    tickbase::holdShotShift(tickbase::doubleTapEnabled());

    if (!peekReturning) {
        peekTeleported = false;
        peekReturnTicks = 0;
    }
    if (shot && !peekReturning) {
        peekReturning = true;
        char text[160];
        std::snprintf(text, sizeof(text), "[автопик] выстрел — возврат: DT %s, режим %s, заряд %d, до точки %.0f ед.",
            tickbase::doubleTapEnabled() ? "вкл" : "выкл", tickbase::fastFireEnabled() ? "быстрый" : "дефенсив",
            tickbase::chargedTicks(), std::hypot(peekOrigin.x - origin.x, peekOrigin.y - origin.y));
        ragebot::logToFile(text);
    }
    constexpr auto moveButtons = csgo::UserCmd::IN_FORWARD | csgo::UserCmd::IN_BACK | csgo::UserCmd::IN_MOVELEFT | csgo::UserCmd::IN_MOVERIGHT;
    if (config.movement.autoPeekMode == static_cast<int>(AutoPeekMode::ShotOrRelease) && !(cmd->buttons & moveButtons))
        peekReturning = true;

    if (!peekReturning)
        return;

    const float dx = peekOrigin.x - origin.x;
    const float dy = peekOrigin.y - origin.y;
    const float distance = std::hypot(dx, dy);
    const auto velocity = localPlayer.get().velocity();
    if (distance < 5.0f && velocity.length2D() < 40.0f) {
        peekReturning = false;
        return;
    }

    // Возврат телепортом: заряженные команды уходят одним пакетом, в каждой движение к точке (эта же
    // функция в каждой прогоняемой команде), сервер проходит их за один тик. Раньше сдвиг шёл сразу
    // на выстреле, а автостоп к этому моменту гасил скорость в ноль: с места за 14 тиков проходишь
    // ~30 ед. (со скоупом AWP ~13) — телепорта не было видно. Теперь сначала разгоняемся к точке и
    // сдвигаем, когда сдвиг довезёт до самой точки, скорость почти максимальная или прошло 6 тиков.
    // телепорт только при включённом DT; без него — обычный возврат ногами
    if (!peekTeleported && tickbase::doubleTapEnabled() && !tickbase::isShifting() && tickbase::chargedTicks() >= 4) {
        float maxSpeed = 250.0f;
        if (const auto weapon = csgo::Entity::from(retSpoofGadgets->client, localPlayer.get().getActiveWeapon()); weapon.getPOD()) {
            if (const auto data = weapon.getWeaponData())
                maxSpeed = localPlayer.get().isScoped() ? data->maxSpeedAlt : data->maxSpeed;
        }
        maxSpeed = std::clamp(maxSpeed, 50.0f, 260.0f);
        const float speed = velocity.length2D();
        const bool arrives = shiftCoverage(speed, maxSpeed, tickbase::chargedTicks() + 1, interval) >= distance - 4.0f;
        // Сразу со следующего тика после выстрела (на самом тике выстрела движение назад добавило бы
        // разброса). Заряд под автопик — до 14 тиков: даже с места после автостопа это ~35–40 ед.
        // Раньше ждали разгона до 14 тиков — телепорт шёл через ~0.2 с после выстрела.
        if (arrives || speed >= maxSpeed * 0.9f || peekReturnTicks >= 1) {
            // Тратим только сколько нужно до точки (+2 тика запаса на торможение у точки, не меньше 6 —
            // на меньшем сдвиге сервер тикбейс не откатывает). Раньше уходил весь заряд (14) на каждый
            // выстрел, и вторую половину перестрелки стояли без DT и дефенсива. Скорость берём к точке:
            // сразу после выстрела мы ещё едем от неё.
            const float towards = (velocity.x * dx + velocity.y * dy) / std::max(distance, 0.001f);
            const int charge = tickbase::chargedTicks();
            int need = charge;
            for (int n = 1; n <= charge; ++n) {
                if (shiftCoverage(towards, maxSpeed, n + 1, interval) >= distance - 4.0f) {
                    need = n;
                    break;
                }
            }
            need = std::clamp(need + 2, std::min(6, charge), charge);
            tickbase::requestShift(need);
            peekTeleported = true;
            char text[128];
            std::snprintf(text, sizeof(text), "[автопик] телепорт: %d из %d тиков, скорость %.0f, до точки %.0f ед., тик возврата %d",
                need, charge, speed, distance, peekReturnTicks);
            ragebot::logToFile(text);
        }
    } else if (!peekTeleported && peekReturnTicks == 0) {
        char text[128];
        std::snprintf(text, sizeof(text), "[автопик] без телепорта: DT %s, сдвиг идёт %s, заряд %d",
            tickbase::doubleTapEnabled() ? "вкл" : "выкл", tickbase::isShifting() ? "да" : "нет", tickbase::chargedTicks());
        ragebot::logToFile(text);
    }
    if (!tickbase::isShifting())
        ++peekReturnTicks;

    // Раньше жали 450 к точке до самого конца: на 250 ед/с игрока проносило мимо, и он качался
    // туда-обратно у кольца. Теперь желаемая скорость падает к точке (тормозной путь ~ v²/2a),
    // а ввод — по разнице желаемой и текущей скорости: вблизи это само становится торможением.
    const float wantedSpeed = std::min(distance * 6.0f, 260.0f);
    const float wantX = dx / std::max(distance, 0.001f) * wantedSpeed - velocity.x;
    const float wantY = dy / std::max(distance, 0.001f) * wantedSpeed - velocity.y;
    if (std::hypot(wantX, wantY) < 5.0f)
        return;
    const float yaw = Helpers::rad2deg(std::atan2(wantY, wantX));
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

namespace
{
float fallSpeed = 0.0f;     // скорость падения до предсказания этого тика
bool fallUnduck = false;    // прошлый тик присели перед землёй — сейчас встать и прыгнуть
}

void noFallDamagePre() noexcept
{
    fallSpeed = localPlayer && localPlayer.get().isAlive() ? -localPlayer.get().velocity().z : 0.0f;
}

void noFallDamage(csgo::UserCmd* cmd, int flagsBeforePrediction) noexcept
{
    if (!config.movement.noFallDamage || !localPlayer || !localPlayer.get().isAlive()) {
        fallUnduck = false;
        return;
    }
    const auto moveType = localPlayer.get().moveType();
    if (moveType == MoveType::LADDER || moveType == MoveType::NOCLIP) {
        fallUnduck = false;
        return;
    }
    using csgo::UserCmd;
    // тик после приседа: в воздухе вставание опускает ноги на землю, и прыжок срабатывает раньше,
    // чем игра посчитает приземление с урона
    if (fallUnduck) {
        cmd->buttons &= ~UserCmd::IN_DUCK;
        cmd->buttons |= UserCmd::IN_JUMP;
        fallUnduck = false;
        return;
    }
    // урон начинается с ~580 ед/с, берём с запасом
    if (fallSpeed < 500.0f)
        return;
    const bool wasInAir = !(flagsBeforePrediction & 1);             // 1 — FL_ONGROUND
    const bool landsNow = (localPlayer.get().flags() & 1) != 0;     // после предсказания этой команды
    if (wasInAir && landsNow) {
        // присед в воздухе мгновенно поджимает ноги — в этот тик не приземляемся
        cmd->buttons |= UserCmd::IN_DUCK;
        cmd->buttons &= ~UserCmd::IN_JUMP;
        fallUnduck = true;
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

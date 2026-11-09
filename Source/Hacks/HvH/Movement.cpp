#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <numbers>
#include <string>

#include <imgui/imgui.h>

#include "Autowall.h"
#include "Extras.h"
#include "HvhConfig.h"
#include "HvhGui.h"
#include "Movement.h"
#include "Ragebot.h"
#include "Tickbase.h"

#include <GameData.h>
#include <Helpers.h>
#include <CSGO/ConVar.h>
#include <CSGO/Constants/ConVarNames.h>
#include <CSGO/Cvar.h>
#include <CSGO/Engine.h>
#include <CSGO/Entity.h>
#include <CSGO/EngineTrace.h>
#include <CSGO/GlobalVars.h>
#include <CSGO/EntityList.h>
#include <Interfaces/ClientInterfaces.h>
#include <Interfaces/EngineInterfaces.h>
#include <Interfaces/OtherInterfaces.h>
#include <CSGO/matrix3x4.h>
#include <CSGO/AnimationLayer.h>
#include <CSGO/WeaponData.h>
#include <CSGO/WeaponId.h>
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

// AI peek: Idle — ищем точку; Moving — идём к ней; Cooldown — ждём возврата автопика и паузу
enum class AiPeekState { Idle, Moving, Cooldown };
AiPeekState aiState = AiPeekState::Idle;
float aiYaw = 0.0f;             // куда идти (мировой yaw)
float aiDistance = 0.0f;        // насколько отойти от точки автопика
float aiDeadline = 0.0f;        // realtime: не вышли на цель за это время — назад
float aiCooldownUntil = 0.0f;
int aiSafeCount = 0;            // сколько проверок подряд точка держится
float aiSafeYaw = 0.0f;
int aiScanTick = 0;

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
    static int prevChoked = -1;     // чок на прошлом тике: по нему видно, полной ли пачкой ушёл пакет

    const auto& key = config.movement.fakeDuckKey;
    const bool was = fakeDucking;
    fakeDucking = key.isSet() && key.isDown() && tickbase::isInstalled()
        && localPlayer && localPlayer.get().isAlive() && localPlayer.get().isOnGround();
    // Урезанный чок раньше жил до перезапуска игры: каждый ранний пакет (выстрел, DT) считался
    // «сервер выкинул присед», лимит падал 14 → 13 → ... → 4 и не возвращался — модель на сервере
    // переставала садиться вовсе. Каждое нажатие ФД начинаем с полного чока.
    if (fakeDucking && !was)
        tickbase::setFakeDuckChoke(14);
    tickbase::setFakeDuck(fakeDucking);
    if (!fakeDucking) {
        badSends = 0;
        cycleLow = 1.0f;
        prevChoked = -1;
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
    // ушедшая пачка была полной — только тогда её присед что-то говорит о сервере
    const bool fullPack = prevChoked + 1 >= limit;
    prevChoked = choked;
    if (choked == 0) {
        fdSentDuck = amount;
        fdLowDuck = cycleLow;
        cycleLow = 1.0f;
        if (!fullPack) {
            // пакет ушёл раньше (первый цикл после нажатия, сдвиг) — не улика
        } else if (amount < 0.5f) {
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

void exploits(const EngineInterfaces& engineInterfaces, const ClientInterfaces& clientInterfaces, const Memory& memory) noexcept
{
    static bool teleportWasDown = false;

    const auto& ex = config.exploits;
    const bool teleportDown = ex.teleportKey.isSet() && ex.teleportKey.isDown();
    const bool teleportPressed = teleportDown && !teleportWasDown;
    teleportWasDown = teleportDown;

    if (!localPlayer || !localPlayer.get().isAlive() || !tickbase::isInstalled()) {
        tickbase::setThreat(false);
        return;
    }
    if (teleportPressed && tickbase::isCharged())
        tickbase::requestShift();

    (void)engineInterfaces;
    (void)clientInterfaces;
    (void)memory;
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

// Серверные лимиты: сколько команд сервер обрабатывает за один свой тик
// (sv_maxusrcmdprocessticks). Больше этого держать в пачке бессмысленно — хвост он выкинет.
// Cvar реплицируется не на всех сборках; если его нет, предел остаётся 14.
//
// Сглаживания ошибки предсказания тут БОЛЬШЕ НЕТ. Попытка размазать рывки через cl_smooth /
// cl_smoothtime была ошибкой: cl_smoothtime — это время, за которое клиент ДОГОНЯЕТ серверную
// позицию, то есть всё это время своя модель и камера едут в прошлом. С дефенсивом расхождение
// держится постоянно, поэтому 0.5 с превращались в постоянное отставание себя от себя
// («моделька идёт не со мной»). Рывок длится тики, а отставание — полсекунды: лекарство было
// хуже болезни. Правильное лечение рывков — не создавать их: заряд чоком (ChargeStyle::Choke),
// на котором команды не теряются вовсе.
void exploitTuning(const csgo::Cvar& cvar) noexcept
{
    static csgo::ConVarPOD* processPod = nullptr;
    static bool resolved = false;

    if (!resolved) {
        resolved = true;
        processPod = cvar.findVar(csgo::sv_maxusrcmdprocessticks);

        // Чиним за собой: версия от 07.11 форсила cl_smoothtime 0.5, а cvar архивный — значение
        // могло уехать в config.cfg. Ровно 0.5 ставили только мы, свои настройки не трогаем.
        if (const auto pod = cvar.findVar(csgo::cl_smoothtime)) {
            const auto smoothTime = csgo::ConVar::from(retSpoofGadgets->client, pod);
            if (std::abs(smoothTime.getFloat() - 0.5f) < 0.001f)
                smoothTime.setValue(0.1f);
        }
    }

    tickbase::setProcessLimit(processPod ? csgo::ConVar::from(retSpoofGadgets->client, processPod).getInt() : 0);
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
        aiState = AiPeekState::Idle;
        aiSafeCount = 0;
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
    // AI peek сам ведёт без WASD — «отпустил WASD» тогда не считается
    if (config.movement.autoPeekMode == static_cast<int>(AutoPeekMode::ShotOrRelease) && !(cmd->buttons & moveButtons)
        && !(config.movement.aiPeek && aiState != AiPeekState::Idle))
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

// AI peek. Зажат автопик, WASD не жмём: раз в 2 тика проверяем направления вокруг (8–16 штук) на 2 дистанциях
// (половина и полный разгон за ~0.3 с), откуда пробиваем ближайшего к прицелу врага (голова / грудь / таз) на
// мин. урон рейджа. Точка держится aiPeekSafety проверок подряд — идём туда; дальше стреляет рейдж (автостоп
// его), после выстрела автопик возвращает назад. Не дошли/не выстрелили — возврат сами, пауза 0.4 с.
void aiPeek(csgo::UserCmd* cmd, float viewPitch, float viewYaw, bool rageHasTarget, const EngineInterfaces& engineInterfaces,
            const ClientInterfaces& clientInterfaces, const OtherInterfaces& interfaces, const Memory& memory) noexcept
{
    const auto& mv = config.movement;
    const float now = memory.globalVars->realtime;
    if (aiCooldownUntil > now + 5.0f)
        aiCooldownUntil = 0.0f;
    constexpr auto moveButtons = csgo::UserCmd::IN_FORWARD | csgo::UserCmd::IN_BACK | csgo::UserCmd::IN_MOVELEFT | csgo::UserCmd::IN_MOVERIGHT;
    if (!mv.aiPeek || !config.rage.enabled || !peekActive || !localPlayer || !localPlayer.get().isAlive()
        || (cmd->buttons & (moveButtons | csgo::UserCmd::IN_JUMP))) {
        aiState = AiPeekState::Idle;
        aiSafeCount = 0;
        return;
    }

    if (aiState == AiPeekState::Cooldown) {
        if (peekReturning || now < aiCooldownUntil)
            return;
        aiState = AiPeekState::Idle;
        aiSafeCount = 0;
    }

    const auto origin = localPlayer.get().getAbsOrigin();
    const float fromStart = std::hypot(origin.x - peekOrigin.x, origin.y - peekOrigin.y);

    if (aiState == AiPeekState::Moving) {
        // выстрел был — автопик уже возвращает
        if (peekReturning) {
            aiState = AiPeekState::Cooldown;
            aiCooldownUntil = now + 0.4f;
            return;
        }
        // рейдж нашёл точку — он тормозит и стреляет, не мешаем
        if (rageHasTarget)
            return;
        if (fromStart > aiDistance * 1.15f + 4.0f || now > aiDeadline) {
            peekReturning = true;
            aiState = AiPeekState::Cooldown;
            aiCooldownUntil = now + 0.4f;
            return;
        }
        const float relative = Helpers::deg2rad(aiYaw - viewYaw);
        cmd->forwardmove = std::cos(relative) * 450.0f;
        cmd->sidemove = -std::sin(relative) * 450.0f;
        return;
    }

    // ---- Idle: ищем точку ----
    if (peekReturning || rageHasTarget || !localPlayer.get().isOnGround() || ++aiScanTick % 2 != 0)
        return;
    const auto weapon = csgo::Entity::from(retSpoofGadgets->client, localPlayer.get().getActiveWeapon());
    if (!weapon.getPOD() || weapon.clip() <= 0)
        return;
    const auto type = weapon.getWeaponType();
    if (type == WeaponType::Knife || type == WeaponType::Grenade || type == WeaponType::C4 || weapon.itemDefinitionIndex() == WeaponId::Taser)
        return;
    const auto data = weapon.getWeaponData();
    if (!data)
        return;
    // выйти успеем к готовности оружия (идти ~0.3 с)
    const float serverTime = memory.globalVars->serverTime();
    if (std::max(weapon.nextPrimaryAttack(), localPlayer.get().nextAttack()) > serverTime + 0.3f)
        return;

    // цель — враг ближе всех к прицелу
    const auto eye = localPlayer.get().getEyePosition();
    const float pitchRad = Helpers::deg2rad(viewPitch), yawRad = Helpers::deg2rad(viewYaw);
    const csgo::Vector aimDir{ std::cos(pitchRad) * std::cos(yawRad), std::cos(pitchRad) * std::sin(yawRad), -std::sin(pitchRad) };
    int targetIndex = 0;
    float bestDot = std::cos(Helpers::deg2rad(std::clamp(config.rage.fov, 1.0f, 180.0f)));
    const int maxClients = std::min(engineInterfaces.getEngine().getMaxClients(), 64);
    for (int i = 1; i <= maxClients; ++i) {
        const auto entity = csgo::Entity::from(retSpoofGadgets->client, clientInterfaces.getEntityList().getEntity(i));
        if (!entity.getPOD() || entity.getNetworkable().isDormant() || !entity.isAlive() || entity.gunGameImmunity()
            || !localPlayer.get().isOtherEnemy(memory, entity))
            continue;
        auto to = entity.getAbsOrigin() + csgo::Vector{ 0.0f, 0.0f, 48.0f } - eye;
        const float length = to.length();
        if (length < 1.0f)
            continue;
        to /= length;
        const float dot = to.x * aimDir.x + to.y * aimDir.y + to.z * aimDir.z;
        if (dot > bestDot) {
            bestDot = dot;
            targetIndex = i;
        }
    }
    if (!targetIndex) {
        aiSafeCount = 0;
        return;
    }
    const auto enemy = csgo::Entity::from(retSpoofGadgets->client, clientInterfaces.getEntityList().getEntity(targetIndex));
    static csgo::matrix3x4 bones[128];
    if (!enemy.setupBones(bones, 128, 0x100, 0.0f))
        return;
    // голова (8), грудь (6), таз (0) — с множителями хитгрупп
    struct AimPoint { csgo::Vector position; int hitgroup; };
    const std::array<AimPoint, 3> points{ AimPoint{ bones[8].origin(), HitGroup::Head }, AimPoint{ bones[6].origin(), HitGroup::Chest },
                                          AimPoint{ bones[0].origin(), HitGroup::Stomach } };
    const int setting = std::max(extras::currentMinDamage(), 1);
    const int health = std::max(enemy.health(), 1);
    const float need = static_cast<float>(setting > 100 ? health + (setting - 100) : std::min(setting, health));

    const auto& engineTrace = engineInterfaces.engineTrace();
    const auto eyeOffset = eye - origin;
    const float interval = memory.globalVars->intervalPerTick;
    const float maxSpeed = std::clamp(data->maxSpeed, 50.0f, 260.0f);
    const float reach = shiftCoverage(localPlayer.get().velocity().length2D(), maxSpeed, static_cast<int>(0.3f / interval), interval);
    const auto enemyOrigin = enemy.getAbsOrigin();
    const float toEnemyYaw = Helpers::rad2deg(std::atan2(enemyOrigin.y - origin.y, enemyOrigin.x - origin.x));

    // направления: сначала поперёк линии на врага (так выглядывают из-за угла), потом остальные
    const int count = std::clamp(mv.aiPeekQuality * 16 / 100, 6, 16);
    std::array<float, 16> yaws{};
    for (int k = 0; k < count; ++k)
        yaws[k] = toEnemyYaw + 360.0f * static_cast<float>(k) / static_cast<float>(count);
    std::sort(yaws.begin(), yaws.begin() + count, [toEnemyYaw](float a, float b) {
        return std::abs(90.0f - std::abs(normalizeYaw(a - toEnemyYaw))) < std::abs(90.0f - std::abs(normalizeYaw(b - toEnemyYaw)));
    });

    const auto reachable = [&](const csgo::Vector& dir, float distance) {
        // проход на уровне колен и груди (+16 ед. — полуширина хитбокса игрока), и под ногами есть пол
        for (const float height : { 20.0f, 52.0f }) {
            csgo::Trace trace;
            const csgo::Vector from{ origin.x, origin.y, origin.z + height };
            engineTrace.traceRay({ from, from + dir * (distance + 16.0f) }, 0x201400B, { localPlayer.get().getPOD() }, trace);
            if (trace.fraction < 1.0f)
                return false;
        }
        const auto spot = origin + dir * distance;
        csgo::Trace ground;
        engineTrace.traceRay({ spot + csgo::Vector{ 0.0f, 0.0f, 18.0f }, spot - csgo::Vector{ 0.0f, 0.0f, 24.0f } }, 0x201400B, { localPlayer.get().getPOD() }, ground);
        return ground.fraction < 1.0f;
    };

    bool found = false;
    float foundYaw = 0.0f, foundDistance = 0.0f;
    for (int k = 0; k < count && !found; ++k) {
        const float yawRadK = Helpers::deg2rad(yaws[k]);
        const csgo::Vector dir{ std::cos(yawRadK), std::sin(yawRadK), 0.0f };
        for (const float part : { 0.5f, 1.0f }) {
            const float distance = reach * part;
            if (distance < 8.0f || !reachable(dir, distance))
                break;
            const auto from = origin + dir * distance + eyeOffset;
            for (const auto& point : points) {
                const float raw = autowall::getDamageBetween(engineInterfaces, interfaces, from, point.position, data, config.rage.autowall);
                if (raw > 0.0f && autowall::scaleDamage(enemy, point.hitgroup, data, raw) >= need) {
                    found = true;
                    break;
                }
            }
            if (found) {
                foundYaw = yaws[k];
                foundDistance = distance;
                break;
            }
        }
    }
    if (!found) {
        aiSafeCount = 0;
        return;
    }
    // одна и та же сторона несколько проверок подряд — не дёргаемся на случайную щель
    aiSafeCount = aiSafeCount > 0 && std::abs(normalizeYaw(foundYaw - aiSafeYaw)) < 35.0f ? aiSafeCount + 1 : 1;
    aiSafeYaw = foundYaw;
    if (aiSafeCount < std::clamp(mv.aiPeekSafety, 1, 16))
        return;
    aiState = AiPeekState::Moving;
    aiYaw = foundYaw;
    aiDistance = foundDistance;
    aiDeadline = now + 0.8f;
    aiSafeCount = 0;
}

bool aiPeekMoving() noexcept
{
    return aiState == AiPeekState::Moving;
}

// Авто хайдшоты (как в hysteria): DT включён, а мы на земле стоим / идём на shift / сидим — быстрая стрельба
// уступает хайдшотам. Не на автопике стоя (там выглядываем — DT), не в воздухе, не с дробовиками/пулемётами
void autoHideShots(const csgo::UserCmd* cmd) noexcept
{
    const auto& ex = config.exploits;
    bool engage = false;
    if (ex.autoHideShots && tickbase::doubleTapEnabled() && localPlayer && localPlayer.get().isAlive()
        && localPlayer.get().isOnGround() && !(cmd->buttons & csgo::UserCmd::IN_JUMP)) {
        const bool ducking = (localPlayer.get().flags() & (1 << 1)) != 0;
        const bool slow = localPlayer.get().velocity().length2D() < 5.0f || (cmd->buttons & csgo::UserCmd::IN_SPEED);
        const bool peekHeld = config.movement.autoPeekKey.isSet() && config.movement.autoPeekKey.isDown();
        if ((slow && !peekHeld) || ducking) {
            if (const auto weapon = csgo::Entity::from(retSpoofGadgets->client, localPlayer.get().getActiveWeapon()); weapon.getPOD()) {
                const auto id = weapon.itemDefinitionIndex();
                const auto type = weapon.getWeaponType();
                const auto data = weapon.getWeaponData();
                const bool deagle = id == WeaponId::Deagle;
                bool allowed = data && !data->fullAuto && type != WeaponType::Pistol;     // снайперки, бёрст
                if (deagle)
                    allowed = ex.autoHsDeagle;
                else if (type == WeaponType::Pistol || type == WeaponType::SubMachinegun || type == WeaponType::Rifle)
                    allowed = ex.autoHsRifles;
                if (id == WeaponId::Revolver || id == WeaponId::Taser || type == WeaponType::Knife || type == WeaponType::Grenade || type == WeaponType::C4)
                    allowed = false;
                engage = allowed;
            }
        }
    }
    tickbase::setAutoHideShots(engage);
}

// Аним брейкер: после клиентской анимации своей модели (RENDER_START, после оригинала) — правим позы и слой.
// Индексы поз из модели игрока: 0 strafe_yaw, 6 jump_fall, 7 move_yaw, 12 body_pitch; слой 6 — MOVEMENT_MOVE
void animBreaker(float realtime) noexcept
{
    static bool wasInAir = false;
    static float landedAt = -10.0f;
    const auto& aa = config.antiAim;
    if (!aa.enabled || !aa.animBreaker || !localPlayer || !localPlayer.get().isAlive()) {
        wasInAir = false;
        return;
    }
    if (realtime < landedAt)
        landedAt = -10.0f;
    const bool inAir = !localPlayer.get().isOnGround();
    if (wasInAir && !inAir)
        landedAt = realtime;
    wasInAir = inAir;

    auto& poses = localPlayer.get().poseParameters();
    if (inAir) {
        const auto air = static_cast<AnimBreak>(aa.breakerAir);
        if (air == AnimBreak::Static) {
            poses[6] = 1.0f;
        } else if (air == AnimBreak::Moonwalk) {
            const auto layers = localPlayer.get().animOverlays();
            if (layers && localPlayer.get().animOverlayCount() > csgo::LAYER_MOVEMENT_MOVE)
                layers[csgo::LAYER_MOVEMENT_MOVE].weight = 1.0f;
        }
    } else {
        const auto ground = static_cast<AnimBreak>(aa.breakerGround);
        if (ground == AnimBreak::Static)
            poses[0] = 1.0f;
        else if (ground == AnimBreak::Moonwalk)
            poses[7] = 0.0f;
        // кивок головой при приземлении (~0.3 с) — держим pitch ровно
        if (aa.breakerLandPitch && realtime - landedAt < 0.3f)
            poses[12] = 0.5f;
    }
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

    // слайдвок: кнопки наоборот — анимация ног идёт «в обратку». Аним брейкер: «Статика» на земле его
    // включает (ноги не шагают — это видят и враги), «Мунвок» — выключает (ноги шагают, у себя — задом)
    const auto& aa = config.antiAim;
    const auto ground = static_cast<AnimBreak>(aa.animBreaker ? aa.breakerGround : 0);
    const bool slide = (aa.slideWalk && ground != AnimBreak::Moonwalk) || ground == AnimBreak::Static;
    if (aa.enabled && slide && localPlayer && localPlayer.get().isOnGround()) {
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

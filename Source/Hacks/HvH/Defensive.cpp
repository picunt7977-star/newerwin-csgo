#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <limits>

#include "Defensive.h"
#include "HvhConfig.h"
#include "Movement.h"
#include "Ragebot.h"
#include "Tickbase.h"

#include <Helpers.h>
#include <CSGO/Engine.h>
#include <CSGO/Entity.h>
#include <CSGO/EngineTrace.h>
#include <CSGO/EntityList.h>
#include <CSGO/GlobalVars.h>
#include <CSGO/LocalPlayer.h>
#include <CSGO/NetworkChannel.h>
#include <CSGO/UserCmd.h>
#include <CSGO/Vector.h>
#include <Interfaces/ClientInterfaces.h>
#include <Interfaces/EngineInterfaces.h>
#include <Memory.h>

// Дефенсив, переписанный с нуля 07.11. Разбор модели — в docs/INTERNALS.md.
// Тик проходит четыре стадии:
//   1) затвор  — можем ли вообще работать (включён, живы, детур стоит, заряд не занят второй пулей DT);
//   2) окно    — что с текущим окном: тики пачки, подтверждение отката сервером, замер;
//   3) повод   — наблюдение за врагами и биты триггеров;
//   4) решение — сколько копить и рвать ли сейчас.
// Сам заряд и сдвиг живут в Tickbase.cpp (детур CL_Move); здесь только политика.

namespace hvh::defensive
{

namespace
{

constexpr int FL_ONGROUND = 1 << 0;

// ---- затвор ----
bool active = false;
bool trigger = false;

// ---- окно ----
int left = 0;               // тиков окна после пачки осталось
int length = 0;             // длина окна на входе (пачка + откат)
int counter = 0;            // номер окна
int shiftWindow = 0;        // тиков пачки ещё впереди (пока идёт сдвиг)
bool confirmed = false;     // текущее окно подтверждено сервером, а не только предсказано

// ---- замер отката ----
// Откат виден только по m_nTickBase, который приходит в снапшоте, то есть с опозданием на пинг.
// Поэтому после каждого рывка ждём дедлайн (пинг + запас) и берём пик отката за это время.
float learned = 0.0f;
int samples = 0;
int peak = 0;
int confirmWait = 0;
int probesFailed = 0;
int pulses = 0;
bool chokeActive = false;   // в этом тике дефенсив работает чоком, а не пропусками
Verdict serverVerdict = Verdict::Unknown;
float verdictAt = -1.0f;    // realtime, когда поставили вердикт «не откатывает» — перепроверим

// ---- наблюдение ----
int sincePulse = 1000;      // тиков с последнего рывка
int lastShots = 0;
int shotHold = 0;           // тиков, пока свой выстрел ещё считается поводом
std::array<bool, 65> wasVisible{};
float nearestEnemy = std::numeric_limits<float>::max();

char status[128] = "Выключен";

// Что модуль видит вокруг в этом тике
struct Sight {
    bool sees = false;      // нас видит хотя бы один враг
    bool justSeen = false;  // кто-то увидел нас именно на этом тике
    bool peeking = false;   // вот-вот выйдем на глаза
    float nearest = std::numeric_limits<float>::max();
};

// Увидит ли враг нашу голову, если мы доедем до точки `to`
[[nodiscard]] bool rayReaches(const EngineInterfaces& engineInterfaces, const Memory& memory, const csgo::Entity& enemy,
                             const csgo::Vector& from, const csgo::Vector& to) noexcept
{
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

// Пик: выйдем ли врагу на глаза в ближайшие `ahead` секунд. Проверяем три точки по пути, а не одну
// в конце прогноза, — так выход из-за угла ловится на несколько тиков раньше, а не ровно в тот
// момент, когда он уже произошёл и запись о нас у врага уже есть.
[[nodiscard]] bool willSee(const EngineInterfaces& engineInterfaces, const Memory& memory, const csgo::Entity& enemy,
                           const csgo::Vector& ourEye, const csgo::Vector& ourDrift, float ahead) noexcept
{
    const auto enemyVelocity = enemy.velocity();
    for (const float part : { 0.34f, 0.67f, 1.0f }) {
        const float t = ahead * part;
        const csgo::Vector from = enemy.getEyePosition() + csgo::Vector{ enemyVelocity.x * t, enemyVelocity.y * t, 0.0f };
        if (rayReaches(engineInterfaces, memory, enemy, from, ourEye + ourDrift * t))
            return true;
    }
    return false;
}

// Средняя скорость на горизонте прогноза. Одной текущей скорости мало: на старте выхода из-за
// угла она ещё нулевая, а ввод уже есть — пик ловился бы на несколько тиков позже.
[[nodiscard]] csgo::Vector driftVelocity(const csgo::UserCmd* cmd, const csgo::Vector& velocity) noexcept
{
    const float yaw = Helpers::deg2rad(cmd->viewangles.y);
    const float forward = std::clamp(cmd->forwardmove / 450.0f, -1.0f, 1.0f);
    const float side = std::clamp(cmd->sidemove / 450.0f, -1.0f, 1.0f);
    const csgo::Vector wish{
        (std::cos(yaw) * forward + std::sin(yaw) * side) * 250.0f,
        (std::sin(yaw) * forward - std::cos(yaw) * side) * 250.0f,
        0.0f
    };
    // на горизонте прогноза скорость примерно посередине между текущей и желаемой
    return { (velocity.x + wish.x) * 0.5f, (velocity.y + wish.y) * 0.5f, velocity.z };
}

[[nodiscard]] Sight look(const csgo::UserCmd* cmd, const EngineInterfaces& engineInterfaces,
                         const ClientInterfaces& clientInterfaces, const Memory& memory, bool wantPeek) noexcept
{
    Sight sight;
    const auto& cfg = config.defensive;
    const csgo::Vector ourEye = localPlayer.get().getEyePosition();
    const csgo::Vector drift = driftVelocity(cmd, localPlayer.get().velocity());
    const float ahead = static_cast<float>(std::clamp(cfg.peekTicks, 2, 16)) * memory.globalVars->intervalPerTick;

    const int maxClients = std::min(engineInterfaces.getEngine().getMaxClients(), 64);
    for (int i = 1; i <= maxClients; ++i) {
        const auto entity = csgo::Entity::from(retSpoofGadgets->client, clientInterfaces.getEntityList().getEntity(i));
        if (!entity.getPOD() || entity.getNetworkable().isDormant() || !entity.isAlive()
            || !localPlayer.get().isOtherEnemy(memory, entity)) {
            wasVisible[i] = false;
            continue;
        }
        sight.nearest = std::min(sight.nearest, (entity.getAbsOrigin() - ourEye).length());
        const bool sees = localPlayer.get().visibleTo(engineInterfaces, memory, entity);
        // «только что увидел» — тоже повод: записи лагкомпа на выходе у него ещё нет
        if (sees && !wasVisible[i])
            sight.justSeen = true;
        wasVisible[i] = sees;
        sight.sees = sight.sees || sees;
        if (wantPeek && !sees && !sight.peeking)
            sight.peeking = willSee(engineInterfaces, memory, entity, ourEye, drift, ahead);
    }
    return sight;
}

// Сколько тиков ждать ответа сервера: пинг туда-обратно плюс запас на интерполяцию снапшота
[[nodiscard]] int confirmDeadline(const EngineInterfaces& engineInterfaces, const Memory& memory) noexcept
{
    const auto channel = engineInterfaces.getEngine().getNetworkChannel();
    float latency = 0.0f;
    if (channel) {
        const auto net = csgo::NetworkChannel::from(retSpoofGadgets->client, channel);
        latency = net.getLatency(0) + net.getLatency(1);
    }
    const float interval = memory.globalVars->intervalPerTick > 0.0f ? memory.globalVars->intervalPerTick : 1.0f / 64.0f;
    return std::clamp(static_cast<int>(std::lround(latency / interval)) + 6, 8, 40);
}

// Длина отката, которую ждём от сервера. Пока замеров нет — верхняя оценка: больше, чем тиков в
// пачке, сервер не откатит.
[[nodiscard]] int expectedRollback(int shifted) noexcept
{
    return samples > 0 ? std::clamp(static_cast<int>(std::lround(learned)), 1, 14) : shifted;
}

// Замер закончился: сервер откатывает или нет
void closeProbe(float realtime) noexcept
{
    char text[256];
    if (peak >= 2) {
        learned = samples == 0 ? static_cast<float>(peak) : learned + (static_cast<float>(peak) - learned) * 0.3f;
        ++samples;
        probesFailed = 0;
        serverVerdict = Verdict::RollsBack;
        std::snprintf(text, sizeof(text), "[дефенсив] рывок %d: сервер откатил %d тик., в среднем %.1f (замеров %d)",
            pulses, peak, learned, samples);
    } else {
        ++probesFailed;
        // окна после пачки не было — что бы мы ни предсказали, тики уже не защищены
        left = 0;
        confirmed = false;
        if (samples == 0 && probesFailed >= 3) {
            serverVerdict = Verdict::Ignores;
            verdictAt = realtime;
            std::snprintf(text, sizeof(text), "[дефенсив] сервер тикбейс не откатывает (3 рывка подряд пустых) — "
                                              "работают только тики внутри пачки, заряд больше не жжём");
        } else {
            std::snprintf(text, sizeof(text), "[дефенсив] рывок %d: откат %d тик. — пусто (подряд пустых %d)",
                pulses, peak, probesFailed);
        }
    }
    peak = 0;
    ragebot::logToFile(text);
}

// ---- стадия 2: окно ----
void advanceWindow(const EngineInterfaces& engineInterfaces, const Memory& memory, float realtime) noexcept
{
    // Пачка прошла — открываем окно по факту её длины, а не по догадке о том, сколько будет заряда
    if (const int shifted = tickbase::takeShiftPack(); shifted > 0) {
        const int after = expectedRollback(shifted);
        left = after;
        // длину окна уже поставил первый тик пачки — тут только уточняем, если пачка вышла длиннее
        length = std::max(length, shifted + after);
        confirmed = false;
        peak = 0;
        confirmWait = confirmDeadline(engineInterfaces, memory);
        ++pulses;
        sincePulse = 0;
    }

    if (left > 0)
        --left;
    if (sincePulse < 1000)
        ++sincePulse;

    const int deficit = tickbase::rawDeficit();
    if (confirmWait > 0) {
        --confirmWait;
        peak = std::max(peak, deficit);
        if (deficit >= 2) {
            confirmed = true;
            // по факту окно может быть длиннее нашей оценки — верим серверу, а не себе
            left = std::max(left, deficit);
        }
        if (confirmWait == 0)
            closeProbe(realtime);
    } else if (confirmed && deficit > 0) {
        left = std::max(left, deficit);
    } else if (serverVerdict == Verdict::RollsBack && deficit > 0 && left == 0) {
        // Откат от чужого сдвига — телепорта автопика или второй пули DT. Окно от него такое же
        // настоящее, как от нашего рывка, и заряда оно не стоит: берём даром.
        left = deficit;
        length = std::max(length, deficit);
        confirmed = true;
        counter = (counter + 1) & 0xFFFF;
    }

    // Вердикт «не откатывает» снимаем через полминуты: карта могла смениться, cvar — тоже
    if (serverVerdict == Verdict::Ignores && verdictAt >= 0.0f && (realtime - verdictAt > 30.0f || realtime < verdictAt)) {
        serverVerdict = Verdict::Unknown;
        probesFailed = 0;
        verdictAt = -1.0f;
    }
}

void reset() noexcept
{
    left = length = 0;
    shiftWindow = 0;
    confirmed = false;
    confirmWait = 0;
    peak = 0;
    sincePulse = 1000;
    lastShots = 0;
    shotHold = 0;
    nearestEnemy = std::numeric_limits<float>::max();
    wasVisible.fill(false);
}

void setStatus(const char* text) noexcept
{
    std::snprintf(status, sizeof(status), "%s", text);
}

}

void update(csgo::UserCmd* cmd, const EngineInterfaces& engineInterfaces, const ClientInterfaces& clientInterfaces,
            const Memory& memory) noexcept
{
    auto& cfg = config.defensive;

    // ---- тики пачки: это не тики времени ----
    // Внутри рывка CreateMove зовётся по разу на каждую прогоняемую команду. Наблюдать и считать
    // окно тут нельзя: один реальный тик стоил бы десяти обходов игроков с трассировками, а окно и
    // интервал между рывками сгорали бы ещё внутри самого рывка — до переделки так и было, и окно
    // на 10 тиков целиком съедалось рывком, который его открыл.
    // Зато эти тики защищены сами по себе: лагкомп сервер запишет только по последней команде пачки.
    if (tickbase::isShifting()) {
        const int inPack = tickbase::shiftLeft() + 1;
        if (shiftWindow == 0) {
            // первый тик пачки: тут и известна её полная длина — с неё начинается окно, и режимы
            // скрытых углов «за окно» считают прогресс от этого числа
            length = inPack + expectedRollback(inPack);
            counter = (counter + 1) & 0xFFFF;
        }
        shiftWindow = inPack;
        return;
    }
    shiftWindow = 0;

    // ---- стадия 1: затвор ----
    // Заряд один. Если DT стреляет второй пулей, он принадлежит выстрелу — дефенсив молчит.
    active = cfg.enabled && localPlayer && localPlayer.get().isAlive() && tickbase::defensiveAllowed();
    // Свои окна (чок или рывок) при быстрой стрельбе DT невозможны: заряд принадлежит второй пуле,
    // а пачка «чок + сдвиг» была бы длиннее, чем сервер обрабатывает за раз. Но сам сдвиг на
    // выстреле даёт настоящий откат — его окна ловим и показываем в них скрытые углы.
    const bool ownWindows = !tickbase::fastFireEnabled();
    if (!active) {
        trigger = false;
        confirmed = false;
        tickbase::setDefensiveCharge(0, cfg.chargeSpread);
        tickbase::setDefensiveChoke(0);
        chokeActive = false;
        if (left > 0)
            --left;
        setStatus(!cfg.enabled ? "Выключен"
            : !tickbase::isInstalled() ? "Детур CL_Move не встал — рывки невозможны"
            : tickbase::fastFireEnabled() ? "DT в быстрой стрельбе — заряд принадлежит второй пуле"
            : movement::isFakeDucking() ? "Фейкдак: чок занят целиком, рывки невозможны"
            : "Ждём респауна");
        return;
    }

    const float realtime = memory.globalVars->realtime;
    const int chargeTarget = std::clamp(cfg.chargeTicks, 4, 14);

    // ---- стадия 2: окно ----
    advanceWindow(engineInterfaces, memory, realtime);

    // Каким способом открываем окна. Дефенсивному режиму DT собственный заряд не нужен, так что
    // чоку он не мешает.
    const bool chokeStyle = ownWindows && static_cast<ChargeStyle>(cfg.chargeStyle) == ChargeStyle::Choke;
    chokeActive = chokeStyle;

    // ---- стадия 3: повод ----
    // Пик считаем только когда он может пригодиться — три трассы на врага каждый тик стоят дорого.
    // «Пригодиться» зависит от способа: чоком окно открывается сразу (заряда ему не надо), пропуском —
    // только с полным зарядом, а при быстрой стрельбе DT своих окон нет вовсе. Раньше тут стояло
    // только условие про полный заряд, и в режиме «чоком» триггер «пик» не срабатывал никогда:
    // заряда в нём нет по определению, так что дефенсив открывал окна только когда нас уже увидели.
    const bool canOpen = ownWindows && left == 0 && (chokeStyle || tickbase::chargedTicks() >= chargeTarget);
    const bool wantPeek = (cfg.triggers & DefPeek) && canOpen;
    const Sight sight = look(cmd, engineInterfaces, clientInterfaces, memory, wantPeek);
    nearestEnemy = sight.nearest;
    tickbase::setThreat(sight.sees);

    // Свой выстрел: сразу после него враг бектрекает нас по тику выстрела — лучше, чтобы этого
    // тика у него не было. Такой рывок идёт вне очереди, интервал не ждём.
    const int shots = localPlayer.get().shotsFired();
    const bool justShot = shots > lastShots;
    lastShots = shots;
    if (justShot) {
        shotHold = 2;
        if (cfg.triggers & DefShot)
            sincePulse = 1000;
    } else if (shotHold > 0) {
        --shotHold;
    }

    const bool keyDown = cfg.key.isSet() && cfg.key.isDown();

    trigger = ((cfg.triggers & DefPeek) && (sight.peeking || sight.justSeen))
        || ((cfg.triggers & DefVisible) && sight.sees)
        || ((cfg.triggers & DefNear) && sight.nearest < 1200.0f)
        || ((cfg.triggers & DefKey) && keyDown)
        || ((cfg.triggers & DefShot) && shotHold > 0);

    // ---- стадия 4: решение ----
    const bool nearEnemy = sight.nearest < 1200.0f;
    // Вердикт «не откатывает» не запрещает окна совсем: раз в полминуты пробуем снова (cvar и
    // карта могли смениться), но заряд при этом не жжём постоянно.
    const bool probing = serverVerdict != Verdict::Ignores;
    const bool onGround = localPlayer.get().flags() & FL_ONGROUND;
    const bool landingSoon = !onGround && localPlayer.get().velocity().z < -150.0f;

    if (!ownWindows) {
        // Окна берём только от сдвигов DT: ни чок, ни рывок тут не наши
        tickbase::setDefensiveCharge(0, cfg.chargeSpread);
        tickbase::setDefensiveChoke(0);
    } else if (chokeStyle) {
        // ---- заряд чоком ----
        // Команды создаются и предсказываются, просто уходят пачкой. Серверная модель отстаёт от
        // нашей на длину чока — это и есть защита, и платить за неё замиранием персонажа не нужно.
        // Отдельный «рывок» тут не нужен: каждая отправленная пачка и есть окно, а интервал между
        // окнами равен длине чока.
        tickbase::setDefensiveCharge(0, cfg.chargeSpread);
        tickbase::setDefensiveChoke(trigger && probing ? chargeTarget : 0);
    } else {
        // ---- заряд пропуском тиков ----
        // Копим заранее: рывок делается только с полным зарядом (в NL force_defensive ставится лишь
        // при rage.exploit:get() >= 1). Заряжаемся, пока рядом вообще кто-то есть, а не только пока
        // висит повод, — иначе первое окно всегда опаздывает на всю зарядку.
        tickbase::setDefensiveChoke(0);
        const bool wantCharge = probing && (trigger || nearEnemy);
        // Пропуск тика — это несозданная команда и замерший персонаж. Пока повод висит, копим по
        // ползунку; когда рядом просто есть враг и повода нет — вдвое реже, чтобы ввод не дёргался
        // на пустом месте.
        const int spread = trigger ? std::clamp(cfg.chargeSpread, 1, 6)
                                   : std::clamp(std::max(cfg.chargeSpread, 1) * 2, 1, 6);
        tickbase::setDefensiveCharge(wantCharge ? chargeTarget : 0, spread);

        // Рывок импульсом, не очередью: не чаще раза в «интервал» тиков и только когда окна сейчас
        // нет. Иначе заряд уходит в непрерывный цикл «рывок → зарядка», а это сплошные пропуски.
        if (trigger && probing && left == 0 && shiftWindow == 0 && !landingSoon
            && tickbase::chargedTicks() >= chargeTarget
            && sincePulse >= std::max(cfg.interval, 1)) {
            // Длину окна поставит advanceWindow на следующем тике — по факту пачки, а не по заряду сейчас
            tickbase::requestShift(0, true);
            sincePulse = 0;
        }
    }

    // ---- строка для меню ----
    const bool choking = chokeStyle;
    if (left > 0 || shiftWindow > 0)
        std::snprintf(status, sizeof(status), confirmed ? "Окно: %d тик. (сервер подтвердил)" : "Окно: %d тик. (ждём подтверждения)",
            windowLeft());
    else if (serverVerdict == Verdict::Ignores)
        setStatus("Сервер тикбейс не откатывает: скрытые углы отключены, перепроверим через 30 с");
    else if (!ownWindows)
        setStatus("Быстрая стрельба DT: свои рывки нельзя, ловим окна от её сдвигов");
    else if (choking && !trigger)
        setStatus("Ждём повода (чока нет, идём как обычно)");
    else if (choking)
        std::snprintf(status, sizeof(status), "Чок %d из %d тик.: серверная модель отстаёт",
            tickbase::chokedCommands(), tickbase::defensiveChokeTicks());
    else if (tickbase::chargedTicks() < chargeTarget)
        std::snprintf(status, sizeof(status), "Заряд %d из %d тик.", tickbase::chargedTicks(), chargeTarget);
    else if (!trigger)
        setStatus("Заряжен, ждём повода");
    else if (sincePulse < std::max(cfg.interval, 1))
        std::snprintf(status, sizeof(status), "Интервал: ещё %d тик.", std::max(cfg.interval, 1) - sincePulse);
    else if (landingSoon)
        setStatus("Приземляемся — рывок съел бы прыжок");
    else
        setStatus("Готов к рывку");
}

bool windowActive() noexcept
{
    return windowLeft() > 0;
}

int windowLeft() noexcept
{
    // Тики пачки скрыты всегда: сервер пишет лагкомп только по её последней команде
    if (shiftWindow > 0)
        return shiftWindow;
    // Тики после пачки — только если сервер правда откатывает. Пока это не проверено, объявлять их
    // скрытыми нельзя: AA показал бы туда свои злые углы, а защиты за ними нет
    return serverVerdict == Verdict::RollsBack ? left : 0;
}

int windowLength() noexcept { return length; }
int windowCounter() noexcept { return counter; }
bool windowConfirmed() noexcept { return confirmed; }
bool enabled() noexcept { return active; }
bool triggerActive() noexcept { return trigger; }
float measuredRollback() noexcept { return samples > 0 ? learned : 0.0f; }
int pulseCount() noexcept { return pulses; }
bool chargeByChoke() noexcept { return chokeActive; }
Verdict verdict() noexcept { return serverVerdict; }
const char* statusLine() noexcept { return status; }

int chargePercent() noexcept
{
    const int target = std::clamp(config.defensive.chargeTicks, 4, 14);
    // при заряде чоком «заряд» — это накопленные чокнутые команды, а не пропущенные тики
    const int have = chokeActive ? tickbase::chokedCommands() : tickbase::chargedTicks();
    return std::clamp(have * 100 / target, 0, 100);
}

void onRoundStart() noexcept
{
    reset();
}

}

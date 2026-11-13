#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
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
// Откат виден по m_nTickBase, который приходит в снапшоте, то есть с опозданием на пинг.
//
// Первая версия привязывала замер к пачке: после каждой ставила дедлайн (пинг + запас) и брала
// пик отката за это время. Это не работало совсем — проверено на логах 07.11: 279 пачек, 9
// удачных замеров (3%), серии до 274 пустых подряд, и вердикт «сервер не откатывает» в 21:26:21
// при том, что через минуту тот же сервер откатывал на 8 и 14 тиков.
// Причина: пачки идут чаще, чем приходит ответ сервера (в режиме «чоком» — каждые 7-8 тиков,
// а дедлайн это пинг + 6), и каждая новая пачка обнуляла `peak` и рестартовала дедлайн. Замер
// почти никогда не доживал до конца, а три «пустых» подряд выключали модуль на 30 секунд.
//
// Теперь замер не привязан к пачке вовсе: следим за откатом непрерывно. Откат ≥ 2 тиков — это
// эпизод, и его пик пишется в среднее, когда эпизод закончился. Сколько пачек его вызвало и
// какой был пинг — не важно.
float learned = 0.0f;
int samples = 0;
int peak = 0;               // пик текущего эпизода отката
bool inRollback = false;    // откат виден прямо сейчас
int pulsesSinceRollback = 0;// пачек с последнего эпизода: много и ни одного — сервер не откатывает
int pulses = 0;
bool chokeActive = false;   // в этом тике дефенсив работает чоком, а не пропусками
Verdict serverVerdict = Verdict::Unknown;
float verdictAt = -1.0f;    // realtime, когда поставили вердикт «не откатывает» — перепроверим
int foreignRollbacks = 0;   // откатов без нашей пачки (не окна) с прошлой строки лога

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

// Длина отката, которую ждём от сервера. Пока замеров нет — верхняя оценка: больше, чем тиков в
// пачке, сервер не откатит.
[[nodiscard]] int expectedRollback(int shifted) noexcept
{
    return samples > 0 ? std::clamp(static_cast<int>(std::lround(learned)), 1, 14) : shifted;
}

// Эпизод отката закончился: пишем его пик в среднее
void closeRollback() noexcept
{
    if (peak < 2) {
        peak = 0;
        return;
    }
    learned = samples == 0 ? static_cast<float>(peak) : learned + (static_cast<float>(peak) - learned) * 0.3f;
    ++samples;
    char text[256];
    std::snprintf(text, sizeof(text), "[дефенсив] откат %d тик. (в среднем %.1f, эпизодов %d, пачек с прошлого %d, чужих откатов %d)",
        peak, learned, samples, pulsesSinceRollback, foreignRollbacks);
    foreignRollbacks = 0;
    ragebot::logToFile(text);
    peak = 0;
    pulsesSinceRollback = 0;
}

// ---- стадия 2: окно ----
// rttTicks — пинг туда-обратно в тиках: столько ждём, пока откат от нашей пачки дойдёт снапшотом
void advanceWindow(float realtime, int rttTicks) noexcept
{
    // Пачка прошла — открываем окно по факту её длины, а не по догадке о том, сколько будет заряда
    if (const int shifted = tickbase::takeShiftPack(); shifted > 0) {
        // 11.11: чоковая пачка тикбейс не двигает, и отката от неё не бывает в принципе. Раньше она
        // считалась «рывком без отката»: через 40 пачек (2-3 секунды пика) вердикт становился
        // «сервер не откатывает» и дефенсив чоком выключал себя на 30 секунд. И окна «после пачки»
        // у неё нет — left от неё только блокировал предсказание пика на длину пачки.
        const bool choke = tickbase::lastPackWasChoke();
        const int after = choke ? 0 : expectedRollback(shifted);
        left = after;
        // длину окна уже поставил первый тик пачки — тут только уточняем, если пачка вышла длиннее
        length = std::max(length, shifted + after);
        // Номер окна. В режиме «чоком» сюда приходят все пачки, и до этой правки счётчик не
        // двигался вовсе: его крутил только блок isShifting(), а чоковая пачка — не сдвиг.
        // Из-за этого рисунки скрытых углов, которые зависят от номера окна, стояли на месте.
        counter = (counter + 1) & 0xFFFF;
        ++pulses;
        if (!choke) {
            ++pulsesSinceRollback;
            sincePulse = 0;
        }
    }

    if (left > 0)
        --left;
    if (sincePulse < 1000)
        ++sincePulse;

    // ---- замер: откат ловим непрерывно, а не по дедлайну от пачки ----
    // Два тика и меньше — это обычный шум предсказания, а не откат.
    //
    // 09.11: откат, перед которым НЕ было нашей пачки/сдвига, — не окно. По логу 09.11 таких было
    // 753 за 11 минут («пачек с прошлого 0» в каждой строке): поправки предсказания, коррекция часов
    // сервера после фейклага 14. Их принимали за окно — AA показывал скрытые углы (рандом pitch/yaw)
    // ~16% времени без всякой защиты, и сервер их записывал: 5 из 9 попаданий по нам 09.11 пришлись
    // именно на «скрытые углы». Окно — только если наша пачка ушла не раньше, чем RTT + запас назад.
    const int rawDeficit = tickbase::rawDeficit();
    const bool ownCause = sincePulse <= rttTicks + 8;
    if (rawDeficit >= 2 && !ownCause && !inRollback) {
        static int lastForeign = 0;
        if (rawDeficit != lastForeign)
            ++foreignRollbacks;
        lastForeign = rawDeficit;
    }
    const int deficit = ownCause || inRollback ? rawDeficit : 0;
    if (deficit >= 2) {
        // Окно открылось (или продлилось). Чей сдвиг его вызвал — наш рывок, наша чоковая пачка,
        // телепорт автопика или вторая пуля DT — неважно: защита от него одна и та же.
        if (!inRollback && left == 0)
            counter = (counter + 1) & 0xFFFF;
        inRollback = true;
        peak = std::max(peak, deficit);
        confirmed = true;
        serverVerdict = Verdict::RollsBack;
        verdictAt = -1.0f;
        // по факту окно может быть длиннее нашей оценки — верим серверу, а не себе
        left = std::max(left, deficit);
        length = std::max(length, deficit);
    } else {
        if (inRollback) {
            inRollback = false;
            closeRollback();
        }
        if (left == 0)
            confirmed = false;
        // Сервер не откатывает вовсе: много пачек прошло, и ни одна не дала отката. Порог тут
        // большой намеренно — раньше хватало трёх «пустых» замеров подряд, и модуль выключал себя
        // на сервере, который на самом деле откатывал на 8-14 тиков (логи 07.11, 21:26).
        if (serverVerdict == Verdict::Unknown && pulsesSinceRollback >= 40) {
            serverVerdict = Verdict::Ignores;
            verdictAt = realtime;
            ragebot::logToFile("[дефенсив] сервер тикбейс не откатывает (40 пачек без отката) — "
                               "работают только тики внутри пачки, заряд больше не жжём");
        }
    }

    // Вердикт «не откатывает» снимаем через полминуты: карта могла смениться, cvar — тоже
    if (serverVerdict == Verdict::Ignores && verdictAt >= 0.0f && (realtime - verdictAt > 30.0f || realtime < verdictAt)) {
        serverVerdict = Verdict::Unknown;
        pulsesSinceRollback = 0;
        verdictAt = -1.0f;
    }
}

void reset() noexcept
{
    left = length = 0;
    shiftWindow = 0;
    confirmed = false;
    inRollback = false;
    peak = 0;
    pulsesSinceRollback = 0;
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

// ---- лаг-пик ----
// Пока выходим из-за угла, команды держатся в чоке: у нас на экране враг уже виден, а сервер
// (и сам враг) ещё считает нас за углом. Выстрел отпускает пачку (onShot → sendNext), и сервер
// получает «вышел и выстрелил» за один свой тик — враг видит нас уже после выстрела.
//
// Фазы:
//   Idle    — чока нет. Предсказали, что новый враг вот-вот увидит, — Holding.
//   Holding — пока выход не вплотную (дальше 2 тиков), пачку сливаем каждый тик: за углом это
//             бесплатно. За 2 тика до выхода чок копится, на выходе — весь лимит до выстрела.
//             Без выстрела — не дольше секунды.
//   Spent   — выстрелили или пик затянулся: обычный фейклаг, пока снова не спрячемся.
//
// 11.11 (вечер): лаг-пик почти не включался. Предсказание смотрело только луч в глаза, а «видит»
// считается и по ногам, и по рукам — они выходят из-за угла раньше головы. Враг «видел» руку
// раньше, чем срабатывало предсказание, и условие «предсказали, но ещё не видят» не выполнялось.
// Плюс любой враг, который уже видел нас, запрещал пик на всех остальных.
enum class PeekPhase { Idle, Holding, Spent };
PeekPhase peekPhase = PeekPhase::Idle;
int peekTicks = 0;          // тиков в текущей фазе
int peekExposed = 0;        // тиков, как нас видит «новый» враг (в Holding)
int peekQuiet = 0;          // тиков подряд: никто новый не видит и выхода не предвидится
int peekShots = 0;
int peekCount = 0;          // сколько пиков с чоком было (для меню)
std::uint64_t seenBefore = 0;   // кто видел нас ещё до начала пика — от них лаг-пик не прячет
char peekStatus[128] = "Выключен";

void setPeekStatus(const char* text) noexcept
{
    std::snprintf(peekStatus, sizeof(peekStatus), "%s", text);
}

void peekLog(const char* text) noexcept
{
    char line[160];
    std::snprintf(line, sizeof(line), "[лаг-пик] %s", text);
    ragebot::logToFile(line);
}

void resetLagPeek() noexcept
{
    peekPhase = PeekPhase::Idle;
    peekTicks = peekExposed = peekQuiet = 0;
    peekShots = 0;
    seenBefore = 0;
    tickbase::setLagPeekChoke(0);
}

// Увидит ли враг хоть что-то из нас через `ahead` секунд: глаза, ноги, плечи — те же части,
// по которым visibleTo считает «видит»
[[nodiscard]] bool willExpose(const EngineInterfaces& engineInterfaces, const Memory& memory, const csgo::Entity& enemy,
                              const csgo::Vector& drift, float ahead) noexcept
{
    const auto& self = localPlayer.get();
    const csgo::Vector eye = self.getEyePosition();
    const csgo::Vector feet = self.getAbsOrigin() + csgo::Vector{ 0.0f, 0.0f, 5.0f };
    const auto enemyVelocity = enemy.velocity();
    const csgo::Vector enemyEye = enemy.getEyePosition();
    // плечи — поперёк направления на врага
    csgo::Vector across{ -(enemyEye.y - eye.y), enemyEye.x - eye.x, 0.0f };
    const float len = std::max(std::hypot(across.x, across.y), 1.0f);
    across = { across.x / len * 14.0f, across.y / len * 14.0f, 0.0f };

    const auto reaches = [&](const csgo::Vector& from, const csgo::Vector& to) {
        if (memory.lineGoesThroughSmoke(from, to, 1))
            return false;
        csgo::Trace trace;
        engineInterfaces.engineTrace().traceRay({ from, to }, 0x46004009, enemy.getPOD(), trace);
        return trace.entity == self.getPOD() || trace.fraction > 0.97f;
    };
    for (const float part : { 0.5f, 1.0f }) {
        const float t = ahead * part;
        const csgo::Vector shift{ drift.x * t, drift.y * t, 0.0f };
        const csgo::Vector from = enemyEye + csgo::Vector{ enemyVelocity.x * t, enemyVelocity.y * t, 0.0f };
        const csgo::Vector chest = eye + shift - csgo::Vector{ 0.0f, 0.0f, 18.0f };
        if (reaches(from, eye + shift + csgo::Vector{ 0.0f, 0.0f, 5.0f }) || reaches(from, feet + shift)
            || reaches(from, chest + across) || reaches(from, chest - across))
            return true;
    }
    return false;
}

void updateLagPeek(const csgo::UserCmd* cmd, const EngineInterfaces& engineInterfaces,
                   const ClientInterfaces& clientInterfaces, const Memory& memory) noexcept
{
    const auto& ex = config.exploits;
    const bool keyOk = !ex.lagPeekKey.isSet() || ex.lagPeekKey.isDown();
    const char* off = !ex.lagPeek ? "Выключен"
        : !keyOk ? "Ждёт клавишу"
        : !tickbase::isInstalled() ? "Детур CL_Move не встал — чока нет"
        : movement::isFakeDucking() ? "Фейкдак: чок занят им"
        : tickbase::knifeAirQuiet() ? "Нож в прыжке с DT — без лагов"
        : !localPlayer || !localPlayer.get().isAlive() ? "Ждём респауна"
        : nullptr;
    if (off) {
        if (peekPhase != PeekPhase::Idle || tickbase::lagPeekChokeTicks() > 0)
            resetLagPeek();
        setPeekStatus(off);
        return;
    }

    const int limit = std::clamp(ex.lagPeekTicks, 2, 14);
    const int predict = std::clamp(ex.lagPeekPredict, 2, 16);
    const float interval = memory.globalVars->intervalPerTick;
    const csgo::Vector drift = driftVelocity(cmd, localPlayer.get().velocity());

    // Наблюдение — по нашей локальной (предсказанной) позиции: её сервер и получит, когда пачка уйдёт.
    // «Новый» — враг, который не видел нас до начала пика: прячемся именно от него.
    std::uint64_t seenNow = 0;
    bool coming = false;    // новый враг увидит в пределах «предсказания»
    bool close = false;     // ... или уже через 3 тика — чок пора копить
    const int maxClients = std::min(engineInterfaces.getEngine().getMaxClients(), 63);
    for (int i = 1; i <= maxClients; ++i) {
        const auto entity = csgo::Entity::from(retSpoofGadgets->client, clientInterfaces.getEntityList().getEntity(i));
        if (!entity.getPOD() || entity.getNetworkable().isDormant() || !entity.isAlive()
            || !localPlayer.get().isOtherEnemy(memory, entity))
            continue;
        if (localPlayer.get().visibleTo(engineInterfaces, memory, entity)) {
            seenNow |= std::uint64_t{ 1 } << i;
            continue;
        }
        if (peekPhase == PeekPhase::Spent)
            continue;
        if (!coming && willExpose(engineInterfaces, memory, entity, drift, static_cast<float>(predict) * interval))
            coming = true;
        if (coming && !close)
            // 3 тика, а не 2: по логам 11.11 выход часто случался раньше прогноза, и на выходе в чоке
            // было 0 — сервер получал уже открытую позицию. Тик запаса стоит одного тика чока после выхода.
            close = willExpose(engineInterfaces, memory, entity, drift, 3.0f * interval);
    }
    const bool newSees = (seenNow & ~seenBefore) != 0;

    // Клавиша лаг-пика зажата — чок держим сразу, ещё за стеной: серверная модель отстаёт уже
    // на подходе к углу, а не только с момента, когда предсказание поймало выход. Пачку не сливаем
    // и по тишине/таймауту не отпускаем — пока клавиша зажата и выстрела не было.
    const bool keyHeld = ex.lagPeekKey.isSet() && ex.lagPeekKey.isDown();

    const int shots = localPlayer.get().shotsFired();
    const bool justShot = shots > peekShots;
    peekShots = shots;
    ++peekTicks;
    peekQuiet = (newSees || coming) ? 0 : peekQuiet + 1;

    char text[128];
    switch (peekPhase) {
    case PeekPhase::Idle:
        seenBefore = seenNow;
        if (coming || keyHeld) {
            peekPhase = PeekPhase::Holding;
            peekTicks = peekExposed = 0;
            ++peekCount;
            // в чоке уже лежит фейклаг — сливаем, пока мы за углом (если выход не вплотную)
            if (!close && !keyHeld && tickbase::chokedCommands() > 0)
                tickbase::sendNextPacket();
        }
        break;
    case PeekPhase::Holding:
        if (justShot) {
            std::snprintf(text, sizeof(text), "выстрел через %d тик. после выхода, в пачке %d", peekExposed, tickbase::chokedCommands() + 1);
            peekLog(text);
            peekPhase = PeekPhase::Spent;
            peekTicks = 0;
            break;
        }
        if (peekQuiet >= 8 && !keyHeld) {
            if (peekExposed > 0) {
                std::snprintf(text, sizeof(text), "ушли обратно без выстрела (на виду %d тик.)", peekExposed);
                peekLog(text);
            }
            peekPhase = PeekPhase::Idle;
            peekTicks = 0;
            break;
        }
        if (newSees) {
            if (peekExposed == 0) {
                const int held = tickbase::chokedCommands();
                std::snprintf(text, sizeof(text), held == 0 ? "вышли раньше прогноза — сервер уже видит выход (чок 0 из %d)"
                    : "вышли: сервер держит нас на %d тик. позади (лимит %d)", held == 0 ? tickbase::lagPeekChokeTicks() : held, tickbase::lagPeekChokeTicks());
                peekLog(text);
            }
            ++peekExposed;
        } else if (!close && !keyHeld && tickbase::chokedCommands() > 0) {
            // выход ещё не вплотную — пачку сливаем, на выходе нужен весь лимит
            tickbase::sendNextPacket();
        }
        if (peekTicks > 64 && !keyHeld) {
            std::snprintf(text, sizeof(text), "секунда без выстрела — отпускаем (на виду %d тик.)", peekExposed);
            peekLog(text);
            peekPhase = PeekPhase::Spent;
            peekTicks = 0;
        }
        break;
    case PeekPhase::Spent:
        // с зажатой клавишей после выстрела снова держим, как только оружие готово (не ждём, пока спрячемся)
        if ((peekQuiet >= 16 && !newSees) || (keyHeld && peekTicks > 8)) {
            peekPhase = PeekPhase::Idle;
            peekTicks = 0;
        }
        break;
    }

    tickbase::setLagPeekChoke(peekPhase == PeekPhase::Holding ? limit : 0);

    switch (peekPhase) {
    case PeekPhase::Idle:
        setPeekStatus("Ждём выхода из-за угла");
        break;
    case PeekPhase::Holding:
        if (peekExposed > 0)
            std::snprintf(text, sizeof(text), "На виду %d тик., чок %d из %d", peekExposed, tickbase::chokedCommands(), tickbase::lagPeekChokeTicks());
        else
            std::snprintf(text, sizeof(text), keyHeld ? "Клавиша зажата: держим чок за стеной (%d из %d)"
                : close ? "Выход через 3 тика: копим чок (%d из %d)" : "Подходим к углу (чок %d из %d)",
                tickbase::chokedCommands(), tickbase::lagPeekChokeTicks());
        setPeekStatus(text);
        break;
    case PeekPhase::Spent:
        setPeekStatus("Пик отыгран — следующий, когда спрячемся");
        break;
    }
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

    // Лаг-пик — своя политика чока, от галки дефенсива не зависит
    updateLagPeek(cmd, engineInterfaces, clientInterfaces, memory);

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
    int rttTicks = 16;
    if (const auto channel = engineInterfaces.getEngine().getNetworkChannel()) {
        const auto net = csgo::NetworkChannel::from(retSpoofGadgets->client, channel);
        const float rtt = net.getLatency(0) + net.getLatency(1);
        rttTicks = std::clamp(static_cast<int>(std::ceil(rtt / memory.globalVars->intervalPerTick)), 2, 40);
    }
    advanceWindow(realtime, rttTicks);

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
    // «Рывок зарядом DT на пике»: DT в режиме дефенсив, заряд набран, автопик не держит его под телепорт
    const bool peekShiftReady = cfg.peekShift && chokeStyle && tickbase::doubleTapEnabled()
        && tickbase::chargedTicks() >= chargeTarget && left == 0
        && !(config.movement.autoPeekKey.isSet() && config.movement.autoPeekKey.isDown());
    const bool wantPeek = ((cfg.triggers & DefPeek) && canOpen) || peekShiftReady;
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

    // Рейдж держит цель и оружие готово — пик/видимость/близость окна не открывают: чок задержал бы
    // выстрел в пачке (сервер получает его позже, враг успевает убить первым), а рывок откатил бы наш
    // тикбейс прямо перед выстрелом. Окно — только сразу после выстрела, пока оружие перезаряжается.
    const bool aiming = ragebot::aimingNow() && !justShot && shotHold == 0;
    trigger = (!aiming && (cfg.triggers & DefPeek) && (sight.peeking || sight.justSeen))
        || (!aiming && (cfg.triggers & DefVisible) && sight.sees)
        || (!aiming && (cfg.triggers & DefNear) && sight.nearest < 1200.0f)
        || ((cfg.triggers & DefKey) && keyDown)
        || ((cfg.triggers & DefShot) && shotHold > 0);
    // В воздухе — окна всегда (кроме момента, когда рейдж целится: пачка задержала бы выстрел)
    if (cfg.inAir && !aiming && !(localPlayer.get().flags() & FL_ONGROUND))
        trigger = true;
    // Нож в прыжке с DT — без лагов (настройка «Нож в прыжке — без лагов»)
    if (tickbase::knifeAirQuiet())
        trigger = false;
    // Лаг-пик держит пачку — рывков и своего чока нет: рывок отправил бы пачку досрочно
    // (Tickbase: сдвиг при полном чоке ждёт отправки), и сервер получил бы нас уже на выходе
    if (peekPhase == PeekPhase::Holding)
        trigger = false;

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
        // Враг только что увидел нас (или увидит через пару тиков), а заряд DT лежит — тратим его на
        // настоящий сдвиг: сервер откатит тикбейс, и записи лагкомпа по нам на это время не будет.
        // Не когда рейдж целится (рывок перед выстрелом откатил бы наш тикбейс) и не чаще интервала.
        const bool pulse = peekShiftReady && probing && !aiming && (sight.peeking || sight.justSeen)
            && peekPhase != PeekPhase::Holding && !landingSoon && shiftWindow == 0
            && sincePulse >= std::max(cfg.interval, 1) && !tickbase::knifeAirQuiet();
        if (pulse) {
            tickbase::requestShift(0, true);
            sincePulse = 0;
        }
        // на тике рывка свой чок не копим: «чок + сдвиг» длиннее, чем сервер обработает за раз
        tickbase::setDefensiveChoke(trigger && probing && !pulse ? chargeTarget : 0);
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
    if (windowLeft() > 0)
        std::snprintf(status, sizeof(status), confirmed ? "Окно: %d тик. (сервер подтвердил откат)" : "Окно: %d тик.",
            windowLeft());
    else if (left > 0 || shiftWindow > 0)
        // пачка прошла, окно предсказано, но сервер ещё ни разу не показал отката — скрытые углы
        // в такое окно не идут: за ними не было бы защиты
        std::snprintf(status, sizeof(status), "Пачка прошла, отката от сервера ещё не видели (пачек %d)",
            pulsesSinceRollback);
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
    resetLagPeek();
}

bool lagPeekHolding() noexcept { return peekPhase == PeekPhase::Holding && tickbase::lagPeekChokeTicks() > 0; }
int lagPeekCount() noexcept { return peekCount; }
const char* lagPeekStatus() noexcept { return peekStatus; }

}

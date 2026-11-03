#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>
#include <optional>
#include <random>
#include <vector>

#include "AntiAim.h"
#include "HvhConfig.h"
#include "Movement.h"
#include "Tickbase.h"

#include <Interfaces/ClientInterfaces.h>
#include <Interfaces/EngineInterfaces.h>
#include <Memory.h>
#include <Netvars.h>
#include <fnv.h>
#include <CSGO/ClientClass.h>
#include <CSGO/Constants/ClassId.h>
#include <CSGO/Engine.h>
#include <CSGO/WeaponData.h>
#include <CSGO/Constants/GameEventNames.h>
#include <CSGO/Entity.h>
#include <CSGO/EngineTrace.h>
#include <Helpers.h>
#include <CSGO/GameEvent.h>
#include <CSGO/EntityList.h>
#include <CSGO/GlobalVars.h>
#include <CSGO/LocalPlayer.h>
#include <CSGO/UserCmd.h>
#include <CSGO/Vector.h>

// Антиаим по образцу hysteria: билдер по состояниям, кнопки (ручной, фристенд, edge yaw, roll),
// safe head, легит AA на E, плавные переходы, анти-брутфорс (переворот / подстройка / стойка пресетов)
// и дефенсив AA — скрытые углы на тиках, когда тикбейс откатился и лагкомп по нам не пишется.

namespace hvh::antiaim
{

namespace
{

constexpr int FL_ONGROUND = 1 << 0;
constexpr int FL_DUCKING = 1 << 1;

std::mt19937 rng{ std::random_device{}() };

Manual manual = Manual::None;
int side = 1;
AaState activeState = AaState::Default;
float currentRoll = 0.0f;

// ---- переключения билдера ----
// Модификатор и джиттер десинка шагают не на каждом отправленном пакете, а раз в «задержку» пакетов.
// phase — номер переключения, от него считаются все режимы.
int phase = 0;
int packetsSinceSwitch = 0;
int switchWait = 1;
int fluctuateStep = 0;
bool switchLong = false;
float switchRandom = 0.0f;      // «рандомизация»: добавка к размаху, своя на каждое переключение
float randomYaw = 0.0f;         // модификатор «Рандом»: держим значение до следующего переключения
int randomSide = 1;             // десинк «Рандом»
float rotatePhase = 0.0f;
float spinYaw = 0.0f;
// «Скиттер»: рваный порядок, по которому резольверу трудно угадать следующий шаг
constexpr int skitterSequence[]{ -1, 1, 0, -1, 1, 0, -1, 0, 1, -1, 0, 1 };

// ---- плавные переходы (натуральность) ----
AntiAimState blendFrom{};
AntiAimState lastBlended{};
float blendStart = -1.0f;
AaState blendState = AaState::Default;

// ---- анти-брутфорс ----
struct Impact {
    int userId;
    csgo::Vector position;
};
std::vector<Impact> pendingImpacts;
bool antiBruteFlip = false;
float antiBruteWeight = 0.0f;   // «Подстройка»: случайный сдвиг настроек в ±силе
float lastAntiBrute = -1.0f;
int rackIndex = -1;             // какой пресет стойки сейчас загружен (−1 — свои настройки)
bool haveOriginal = false;
AntiAimPreset original;         // что было до стойки — вернём по таймеру/в начале раунда

// ---- дефенсив AA ----
struct SnapRuntime {
    bool active = false;
    int ticks = 0;              // тиков в текущем окне
    int apex = 0;               // длина окна на входе
    int counter = 0;            // номер окна
    int delayed = 0;
    bool jitter = false;
    bool haveRandomPitch = false, haveRandomYaw = false;
    float randomPitch = 0.0f, randomYaw = 0.0f;
} snap;

// ---- легит AA ----
bool legitWait = false;

// ---- флики и увод от прицела ----
int flickCounter = 0;
int flickAfterShift = 0;
float flickDirection = 1.0f;
bool wasFlicking = false;
bool defensiveFlip = false;

void toggleManual(const KeyBind& key, Manual direction) noexcept
{
    if (key.isSet() && key.isPressed())
        manual = manual == direction ? Manual::None : direction;
}

[[nodiscard]] float distanceToSegment(const csgo::Vector& point, const csgo::Vector& a, const csgo::Vector& b) noexcept
{
    const auto ab = b - a;
    const float lengthSquared = ab.x * ab.x + ab.y * ab.y + ab.z * ab.z;
    float t = 0.0f;
    if (lengthSquared > 0.0f)
        t = std::clamp(((point.x - a.x) * ab.x + (point.y - a.y) * ab.y + (point.z - a.z) * ab.z) / lengthSquared, 0.0f, 1.0f);
    return (point - (a + ab * t)).length();
}

void storePreset(AntiAimPreset& out) noexcept
{
    out.states = config.antiAim.states;
    out.snaps = config.antiAim.snaps;
}

void loadPreset(const AntiAimPreset& preset) noexcept
{
    config.antiAim.states = preset.states;
    config.antiAim.snaps = preset.snaps;
}

void resetAntiBrute() noexcept
{
    antiBruteFlip = false;
    antiBruteWeight = 0.0f;
    lastAntiBrute = -1.0f;
    if (haveOriginal) {
        loadPreset(original);
        haveOriginal = false;
    }
    rackIndex = -1;
}

// Сработал анти-брутфорс: враг стрелял рядом с нами или мы выстрелили сами
void triggerAntiBrute(float realtime) noexcept
{
    const auto& aa = config.antiAim;
    // одна пуля даёт несколько импактов (пробитие) — не чаще раза в 0.25 с
    if (lastAntiBrute >= 0.0f && realtime - lastAntiBrute < 0.25f)
        return;
    lastAntiBrute = realtime;
    switch (static_cast<AntiBruteMode>(aa.antiBruteMode)) {
    case AntiBruteMode::Adjust: {
        const float power = std::clamp(aa.antiBrutePower, 1, 100) * 0.01f;
        antiBruteWeight = std::uniform_real_distribution<float>{ -power, power }(rng);
        break;
    }
    case AntiBruteMode::PresetRack: {
        const int count = static_cast<int>(aa.presetRack.size());
        if (count == 0) {
            antiBruteFlip = !antiBruteFlip;
            break;
        }
        if (!haveOriginal) {
            storePreset(original);
            haveOriginal = true;
        }
        if (aa.antiBruteOrder == 0 && count > 1) {
            int next = rackIndex;
            while (next == rackIndex)
                next = std::uniform_int_distribution<int>{ 0, count - 1 }(rng);
            rackIndex = next;
        } else {
            rackIndex = (rackIndex + 1) % count;
        }
        loadPreset(aa.presetRack[rackIndex]);
        break;
    }
    default:
        antiBruteFlip = !antiBruteFlip;
        break;
    }
    // новая сторона — и новая фаза джиттера: его перебор опирался на старый ритм
    ++phase;
}

struct ImpactListener : csgo::GameEventListener {
    void fireGameEvent(csgo::GameEventPOD* eventPointer) override
    {
        const auto event = csgo::GameEvent::from(retSpoofGadgets->client, eventPointer);
        if (pendingImpacts.size() < 64)
            pendingImpacts.push_back({ event.getInt("userid"), { event.getFloat("x"), event.getFloat("y"), event.getFloat("z") } });
    }
};
ImpactListener impactListener;
bool impactListenerRegistered = false;

// Пуля врага прошла рядом с нами — и к нам ближе, чем к любому союзнику (как в hysteria: стреляли в нас)
void processImpacts(const EngineInterfaces& engineInterfaces, const ClientInterfaces& clientInterfaces, const Memory& memory) noexcept
{
    if (pendingImpacts.empty())
        return;
    const int localIndex = localPlayer.get().getNetworkable().index();
    auto body = localPlayer.get().getAbsOrigin();
    body.z += 36.0f;
    const auto head = localPlayer.get().getEyePosition();
    const int maxClients = engineInterfaces.getEngine().getMaxClients();
    for (const auto& impact : pendingImpacts) {
        const int shooterIndex = engineInterfaces.getEngine().getPlayerForUserID(impact.userId);
        if (shooterIndex <= 0 || shooterIndex == localIndex)
            continue;
        const auto shooter = csgo::Entity::from(retSpoofGadgets->client, clientInterfaces.getEntityList().getEntity(shooterIndex));
        if (!shooter.getPOD() || shooter.getNetworkable().isDormant() || !shooter.isOtherEnemy(memory, localPlayer.get()))
            continue;
        // m_vecViewOffset у чужих игроков не приходит — глаза примерно на 64 над ногами
        auto shooterEye = shooter.getAbsOrigin();
        shooterEye.z += 64.0f;
        const float ours = std::min(distanceToSegment(body, shooterEye, impact.position), distanceToSegment(head, shooterEye, impact.position));
        if (ours > 80.0f)
            continue;
        bool closerTeammate = false;
        for (int i = 1; i <= maxClients && !closerTeammate; ++i) {
            if (i == localIndex)
                continue;
            const auto mate = csgo::Entity::from(retSpoofGadgets->client, clientInterfaces.getEntityList().getEntity(i));
            if (!mate.getPOD() || mate.getNetworkable().isDormant() || !mate.isAlive() || mate.isOtherEnemy(memory, localPlayer.get()))
                continue;
            auto center = mate.getAbsOrigin();
            center.z += 36.0f;
            closerTeammate = distanceToSegment(center, shooterEye, impact.position) < ours;
        }
        if (!closerTeammate)
            triggerAntiBrute(memory.globalVars->realtime);
    }
    pendingImpacts.clear();
}

// ---- состояние ----

// Состояние для билдера: воздух (с приседом), присед (стоя/в движении), шаг на shift, бег, стоя
AaState detectState(const csgo::UserCmd* cmd) noexcept
{
    const auto& player = localPlayer.get();
    const int flags = player.flags();
    const bool onGround = (flags & FL_ONGROUND) && !(cmd->buttons & csgo::UserCmd::IN_JUMP);
    const bool ducking = (cmd->buttons & csgo::UserCmd::IN_DUCK) || (flags & FL_DUCKING) || movement::isFakeDucking();
    const float speed = player.velocity().length2D();
    const bool moving = speed > 5.0f || std::abs(cmd->forwardmove) > 1.0f || std::abs(cmd->sidemove) > 1.0f;
    if (!onGround)
        return ducking ? AaState::AirCrouch : AaState::Air;
    if (ducking)
        return moving ? AaState::Sneaking : AaState::Crouching;
    if (moving && (cmd->buttons & csgo::UserCmd::IN_SPEED))
        return AaState::Walking;
    return moving ? AaState::Running : AaState::Standing;
}

// Какие настройки работают в этом состоянии: «Фейклаг» без эксплойтов главнее; «Воздух + присед»
// без своих — «Воздух»; «Присед в движении» — «Присед»; остальное без своих — «Общее»
AaState resolveState(AaState state) noexcept
{
    const auto& states = config.antiAim.states;
    const auto own = [&states](AaState s) { return states[static_cast<int>(s)].override; };
    const bool exploitActive = tickbase::doubleTapEnabled() || config.exploits.defensive;
    if (!exploitActive && own(AaState::Fakelag))
        return AaState::Fakelag;
    if (state == AaState::AirCrouch && !own(state))
        state = AaState::Air;
    if (state == AaState::Sneaking && !own(state))
        state = AaState::Crouching;
    return own(state) ? state : AaState::Default;
}

[[nodiscard]] int lerpInt(int a, int b, float t) noexcept
{
    return static_cast<int>(std::lround(static_cast<float>(a) + static_cast<float>(b - a) * t));
}

// Натуральность: числа плывут от прошлого состояния к новому, переключатели — на середине перехода
AntiAimState blended(AaState state, float realtime) noexcept
{
    const auto& target = config.antiAim.states[static_cast<int>(state)];
    const auto& aa = config.antiAim;
    if (!aa.naturality || aa.naturalityTime <= 0.0f) {
        blendStart = -1.0f;
        blendState = state;
        lastBlended = target;
        return target;
    }
    if (state != blendState) {
        blendFrom = lastBlended;
        blendStart = realtime;
        blendState = state;
    }
    const float t = blendStart < 0.0f ? 1.0f : std::clamp((realtime - blendStart) / aa.naturalityTime, 0.0f, 1.0f);
    AntiAimState out = t >= 0.5f ? target : blendFrom;
    out.yawOffset = lerpInt(blendFrom.yawOffset, target.yawOffset, t);
    out.addLeft = lerpInt(blendFrom.addLeft, target.addLeft, t);
    out.addRight = lerpInt(blendFrom.addRight, target.addRight, t);
    out.degree = lerpInt(blendFrom.degree, target.degree, t);
    out.spinSpeed = lerpInt(blendFrom.spinSpeed, target.spinSpeed, t);
    out.randomize = lerpInt(blendFrom.randomize, target.randomize, t);
    out.desyncLeft = lerpInt(blendFrom.desyncLeft, target.desyncLeft, t);
    out.desyncRight = lerpInt(blendFrom.desyncRight, target.desyncRight, t);
    lastBlended = out;
    return out;
}

int nextSwitchWait(const AntiAimState& st) noexcept
{
    const int ticks = std::clamp(st.delayTicks, 1, 16);
    switch (static_cast<SwitchDelay>(st.delayMode)) {
    case SwitchDelay::Static: return ticks;
    case SwitchDelay::Fluctuate:
        fluctuateStep = fluctuateStep % ticks + 1;
        return fluctuateStep;
    case SwitchDelay::Switch:
        switchLong = !switchLong;
        return switchLong ? ticks : 1;
    case SwitchDelay::Random: return std::uniform_int_distribution<int>{ 1, ticks }(rng);
    default: return 1;
    }
}

// Добавка модификатора к yaw. Размах — весь ход: центр-джиттер ±½, N-way от −½ до +½ и т. д.
float modifierYaw(const AntiAimState& st, float degree) noexcept
{
    const float half = degree * 0.5f;
    switch (static_cast<YawModifier>(st.modifier)) {
    case YawModifier::Jitter: return phase & 1 ? degree : 0.0f;
    case YawModifier::CenterJitter: return phase & 1 ? half : -half;
    case YawModifier::Spin:
        spinYaw = std::remainder(spinYaw + static_cast<float>(st.spinSpeed), 360.0f);
        return spinYaw;
    case YawModifier::Random: return randomYaw * (st.degree > 0 ? degree / static_cast<float>(st.degree) : 0.0f);
    case YawModifier::FiveWay: {
        const int ways = std::clamp(st.ways, 3, 8);
        const float t = static_cast<float>(phase % ways) / static_cast<float>(ways - 1);
        return -half + degree * t;
    }
    case YawModifier::Rotate: {
        // туда-обратно по размаху: скорость — сколько сотых хода за тик
        rotatePhase = std::fmod(rotatePhase + static_cast<float>(st.spinSpeed) * 0.01f, 2.0f);
        const float t = rotatePhase < 1.0f ? rotatePhase : 2.0f - rotatePhase;
        return -half + degree * t;
    }
    case YawModifier::Skitter: return static_cast<float>(skitterSequence[phase % std::size(skitterSequence)]) * half;
    default: return 0.0f;
    }
}

// угол на ближайшего по прицелу врага, если врагов нет — текущий угол камеры
bool freestandActive = false;
float freestandSide = 0.0f;     // +1 — голова спрятана влево, -1 — вправо
int freestandFlipTicks = 0;     // сколько тиков подряд фристенд просит другую сторону
int freestandHoldTicks = 0;     // сколько ещё держать прошлую сторону, если фристенду не от чего прятать

float atTargetsYaw(const csgo::UserCmd* cmd, const EngineInterfaces& engineInterfaces, const ClientInterfaces& clientInterfaces, const Memory& memory, csgo::Vector* targetEye = nullptr, bool* found = nullptr, bool includeDormant = false) noexcept
{
    const auto eye = localPlayer.get().getEyePosition();
    float bestFov = std::numeric_limits<float>::max();
    float bestYaw = cmd->viewangles.y;
    if (found)
        *found = false;

    for (int i = 1; i <= engineInterfaces.getEngine().getMaxClients(); ++i) {
        const auto entity = csgo::Entity::from(retSpoofGadgets->client, clientInterfaces.getEntityList().getEntity(i));
        if (entity.getPOD() == nullptr || entity.getPOD() == localPlayer.get().getPOD() || (!includeDormant && entity.getNetworkable().isDormant())
            || !entity.isAlive() || !entity.isOtherEnemy(memory, localPlayer.get()))
            continue;

        const float yaw = (entity.getAbsOrigin() - eye).toAngle().y;
        const float fov = std::abs(std::remainder(yaw - cmd->viewangles.y, 360.0f));
        if (fov < bestFov) {
            bestFov = fov;
            bestYaw = yaw;
            if (targetEye)
                *targetEye = entity.getEyePosition();
            if (found)
                *found = true;
        }
    }
    return bestYaw;
}

// Дефенсив AA: есть ли враг, который прямо сейчас целится в нас. Смотрим его присланный угол взгляда
// (у чужих он интерполируется, но для «смотрит ли в нашу сторону» этого хватает) и направление с его глаз
// на нашу голову. Возвращает yaw от нас на ближайшего такого врага — от него и уводим тело.
bool enemyAimingAtUs(const EngineInterfaces& engineInterfaces, const ClientInterfaces& clientInterfaces, const Memory& memory, float fov, float& threatYaw) noexcept
{
    const auto head = localPlayer.get().getEyePosition();
    bool found = false;
    float bestFov = fov;
    for (int i = 1; i <= engineInterfaces.getEngine().getMaxClients(); ++i) {
        const auto entity = csgo::Entity::from(retSpoofGadgets->client, clientInterfaces.getEntityList().getEntity(i));
        if (entity.getPOD() == nullptr || entity.getPOD() == localPlayer.get().getPOD() || entity.getNetworkable().isDormant()
            || !entity.isAlive() || !entity.isOtherEnemy(memory, localPlayer.get()))
            continue;
        // m_vecViewOffset у чужих игроков не приходит — глаза примерно на 64 над ногами
        auto enemyEye = entity.getAbsOrigin();
        enemyEye.z += 64.0f;
        const float toUs = (head - enemyEye).toAngle().y;
        const float difference = std::abs(std::remainder(toUs - entity.eyeAngles().y, 360.0f));
        if (difference <= bestFov) {
            bestFov = difference;
            threatYaw = (entity.getAbsOrigin() - head).toAngle().y;
            found = true;
        }
    }
    return found;
}

// Фристенд: куда спрятать голову от врага. Возвращает поворот yaw относительно направления на врага:
// +90 — голова влево, -90 — вправо, 0 — прятать не от чего.
// 1) стена вплотную сбоку — голову к ней;
// 2) лучи к врагу из точек слева и справа от головы: где стена ближе — там укрытие. Так ловится и стена
//    перед нами, которая кончается сбоку: луч с открытой стороны уходит мимо её края;
// 3) укрытие с обеих сторон одинаковое (стоим за стеной, край ещё не дошёл) — смотрим, куда едем:
//    сторона движения — та, что выглянет первой, голова уходит в противоположную;
// 4) иначе — какую сторону враг видит хуже (лучи от его глаз).
float freestandYaw(const csgo::EngineTrace& engineTrace, const csgo::Vector& enemyEye, float yawToEnemy) noexcept
{
    const auto eye = localPlayer.get().getEyePosition();
    const auto sidePoint = [&](float offset, float distance) {
        const float radians = Helpers::deg2rad(yawToEnemy + offset);
        return csgo::Vector{ eye.x + std::cos(radians) * distance, eye.y + std::sin(radians) * distance, eye.z };
    };
    const auto traceWalls = [&](const csgo::Vector& from, const csgo::Vector& to) {
        csgo::Trace trace;
        engineTrace.traceRay({ from, to }, 0x1400B, { localPlayer.get().getPOD() }, trace);
        return trace.startSolid ? 0.0f : trace.fraction;
    };

    // 1) стена сбоку от головы — до ~1.5 м (80 ед.; было 45, и стену в метре фристенд не видел).
    // Смотрим чуть вперёд-вбок тоже (±60°): стена, которая начинается сбоку-спереди, укрывает голову так же.
    constexpr float wallReach = 80.0f;
    const auto wallDistance = [&](float side) {
        const float straight = traceWalls(eye, sidePoint(side * 90.0f, wallReach)) * wallReach;
        const float angled = traceWalls(eye, sidePoint(side * 60.0f, wallReach)) * wallReach;
        return std::min(straight, angled);
    };
    const float leftWall = wallDistance(1.0f);
    const float rightWall = wallDistance(-1.0f);
    if (std::min(leftWall, rightWall) < wallReach - 1.0f && std::abs(leftWall - rightWall) > 6.0f)
        return leftWall < rightWall ? 90.0f : -90.0f;

    // 2) лучи к врагу из точек сбоку от головы (не дальше самого врага)
    const float toEnemyRadians = Helpers::deg2rad(yawToEnemy);
    const csgo::Vector forward{ std::cos(toEnemyRadians), std::sin(toEnemyRadians), 0.0f };
    const float reach = std::clamp((enemyEye - eye).length2D(), 64.0f, 400.0f);
    const auto coverDistance = [&](float offset) {
        float sum = 0.0f;
        for (const float distance : { 20.0f, 32.0f }) {
            const auto from = sidePoint(offset, distance);
            // точка сбоку уже в стене — укрытие вплотную
            if (traceWalls(eye, from) < 0.99f)
                continue;
            sum += traceWalls(from, from + forward * reach) * reach;
        }
        return sum * 0.5f;
    };
    const float leftCover = coverDistance(90.0f);
    const float rightCover = coverDistance(-90.0f);
    if (std::abs(leftCover - rightCover) > 24.0f)
        return leftCover < rightCover ? 90.0f : -90.0f;

    // 3) укрытие одинаковое — прячем голову от стороны, куда едем
    const auto velocity = localPlayer.get().velocity();
    const float leftSpeed = -velocity.x * forward.y + velocity.y * forward.x;   // скорость влево относительно врага
    const bool covered = leftCover < reach * 0.9f && rightCover < reach * 0.9f;
    if (std::abs(leftSpeed) > 30.0f && (covered || std::abs(leftSpeed) > 120.0f))
        return leftSpeed > 0.0f ? -90.0f : 90.0f;

    // 4) какую сторону головы враг видит хуже (луч от его глаз, только стены)
    const auto openness = [&](float offset) {
        float sum = 0.0f;
        for (const float distance : { 15.0f, 30.0f, 45.0f })
            sum += traceWalls(enemyEye, sidePoint(offset, distance));
        return sum / 3.0f;
    };
    const float left = openness(90.0f);
    const float right = openness(-90.0f);
    if (std::abs(left - right) < 0.05f)
        return 0.0f;
    return left < right ? 90.0f : -90.0f;
}


// ---- дефенсив AA ----

// Как в hysteria: своё состояние берётся, только если у него не «Как общее»; «Выкл» — выключает
SnapState selectSnapState(const csgo::UserCmd* cmd, bool haveFreestandRaw) noexcept
{
    const auto& snaps = config.antiAim.snaps;
    const auto custom = [&snaps](SnapState s) { return snaps[static_cast<int>(s)].mode != static_cast<int>(SnapMode::Default); };
    const auto& player = localPlayer.get();
    const int flags = player.flags();
    const bool jumping = !(flags & FL_ONGROUND) || (cmd->buttons & csgo::UserCmd::IN_JUMP);
    const bool ducking = (cmd->buttons & csgo::UserCmd::IN_DUCK) || (flags & FL_DUCKING);
    const bool moving = player.velocity().length2D() > 5.0f;
    if (custom(SnapState::AirCrouch) && jumping && ducking)
        return SnapState::AirCrouch;
    if (custom(SnapState::Air) && jumping)
        return SnapState::Air;
    if (custom(SnapState::EdgeBait) && haveFreestandRaw && !jumping)
        return SnapState::EdgeBait;
    if (custom(SnapState::Sneaking) && !jumping && ducking && moving)
        return SnapState::Sneaking;
    if (custom(SnapState::Crouching) && !jumping && ducking)
        return SnapState::Crouching;
    if (custom(SnapState::Walking) && !jumping && moving && (cmd->buttons & csgo::UserCmd::IN_SPEED))
        return SnapState::Walking;
    return SnapState::Default;
}

const DefensiveSnap* activeSnap(SnapState state) noexcept
{
    const auto& snaps = config.antiAim.snaps;
    if (state != SnapState::Default) {
        const auto& own = snaps[static_cast<int>(state)];
        if (own.mode == static_cast<int>(SnapMode::Off))
            return nullptr;
        if (own.mode == static_cast<int>(SnapMode::Custom))
            return &own;
    }
    const auto& def = snaps[static_cast<int>(SnapState::Default)];
    return def.mode == static_cast<int>(SnapMode::Custom) ? &def : nullptr;
}

[[nodiscard]] float fraction(float value) noexcept
{
    return value - std::floor(value);
}

[[nodiscard]] float lerp(float a, float b, float t) noexcept
{
    return a + (b - a) * t;
}

// Прогресс окна для «Spin Full»: 1 на входе в окно, 0 на его конце
[[nodiscard]] float windowProgress(const DefensiveSnap& s, int left) noexcept
{
    const int length = std::min(snap.apex, std::clamp(s.duration, 1, 13));
    if (length <= 1)
        return 0.0f;
    const int base = snap.apex - (length - 1);
    return std::clamp(static_cast<float>(left - base) / static_cast<float>(length - 1), 0.0f, 1.0f);
}

std::optional<float> snapPitch(const DefensiveSnap& s, float curtime, int left, float cameraPitch, float targetPitch) noexcept
{
    const float a = static_cast<float>(s.pitchAngle);
    const float b = static_cast<float>(s.pitchAngle2);
    switch (static_cast<SnapPitch>(s.pitch)) {
    case SnapPitch::Static: return a;
    case SnapPitch::Jitter: return snap.jitter ? a : b;
    case SnapPitch::Random: return std::uniform_real_distribution<float>{ std::min(a, b), std::max(a, b) }(rng);
    case SnapPitch::RandomStatic:
        if (!snap.haveRandomPitch) {
            snap.randomPitch = std::uniform_real_distribution<float>{ std::min(a, b), std::max(a, b) }(rng);
            snap.haveRandomPitch = true;
        }
        return snap.randomPitch;
    case SnapPitch::Spin: return lerp(a, b, fraction(curtime * static_cast<float>(s.pitchSpeed) * 0.1f));
    case SnapPitch::SpinFull: return lerp(a, b, windowProgress(s, left));
    case SnapPitch::Camera: return cameraPitch;
    case SnapPitch::AtTarget: return targetPitch;
    default: return std::nullopt;
    }
}

// yaw относительно направления на врага: 0 — лицом к нему, 180 — спиной (обычный AA)
std::optional<float> snapYaw(const DefensiveSnap& s, float curtime, int left, std::optional<float> freestandRaw) noexcept
{
    const float angle = static_cast<float>(s.yawAngle);
    const float half = angle * 0.5f;
    const float speed = static_cast<float>(s.yawSpeed);
    switch (static_cast<SnapYaw>(s.yaw)) {
    case SnapYaw::Static: return 360.0f - angle;
    case SnapYaw::Jitter: return 180.0f + (snap.jitter ? half : -half);
    case SnapYaw::Random: return 180.0f + std::uniform_real_distribution<float>{ -half, half }(rng);
    case SnapYaw::RandomJitter:
        return (std::uniform_int_distribution<int>{ 0, 1 }(rng) ? 90.0f : -90.0f) + std::uniform_real_distribution<float>{ -angle * 0.25f, angle * 0.25f }(rng);
    case SnapYaw::RandomStatic:
        if (!snap.haveRandomYaw) {
            snap.randomYaw = std::uniform_real_distribution<float>{ -half, half }(rng);
            snap.haveRandomYaw = true;
        }
        return 180.0f + snap.randomYaw;
    case SnapYaw::Spin: return 180.0f + lerp(-half, half, fraction(curtime * speed * 0.1f));
    case SnapYaw::SpinFull: return 180.0f + lerp(-half, half, windowProgress(s, left));
    case SnapYaw::SpinJitter: return (snap.jitter ? 90.0f : -90.0f) + lerp(-half, half, fraction(curtime * speed * 0.1f));
    case SnapYaw::Way90: {
        const float t = fraction(static_cast<float>(left) / static_cast<float>(std::clamp(s.duration, 1, 13)) * speed * 0.05f);
        return (snap.counter % 2 == 0 ? 90.0f : -90.0f) + lerp(-half, half, t) - 90.0f;
    }
    case SnapYaw::Sway180: return 180.0f + lerp(-half, half, std::sin(curtime * speed * 0.2f) * 0.5f + 0.5f);
    case SnapYaw::Opposite:
        // приманка у стены: на скрытых тиках голова — с открытой стороны (обычный фристенд её прячет)
        if (!freestandRaw)
            return std::nullopt;
        return freestandActive ? 180.0f : -*freestandRaw;
    default: return std::nullopt;
    }
}

// Edge yaw (как в hysteria): ближайшая стена в пределах ~32 ед. от глаз — лицом в неё, голова в стене
std::optional<float> edgeYaw(const csgo::EngineTrace& engineTrace) noexcept
{
    const auto eye = localPlayer.get().getEyePosition();
    const auto trace = [&](float yaw) {
        const float radians = Helpers::deg2rad(yaw);
        const csgo::Vector end{ eye.x + std::cos(radians) * 32.0f, eye.y + std::sin(radians) * 32.0f, eye.z };
        csgo::Trace result;
        engineTrace.traceRay({ eye, end }, 0x1400B, { localPlayer.get().getPOD() }, result);
        return result.startSolid ? 1.0f : result.fraction;
    };
    float best = 1.0f;
    std::optional<float> bestYaw;
    for (float yaw = -180.0f; yaw < 180.0f; yaw += 45.0f) {
        if (const float f = trace(yaw); f < best) {
            best = f;
            bestYaw = yaw;
        }
    }
    if (!bestYaw)
        return std::nullopt;
    const float coarse = *bestYaw;
    for (float yaw = coarse - 30.0f; yaw <= coarse + 30.0f; yaw += 10.0f) {
        if (const float f = trace(yaw); f < best) {
            best = f;
            bestYaw = yaw;
        }
    }
    return std::remainder(*bestYaw, 360.0f);
}

// Safe head (как в hysteria). Обычный: прыжок с ножом и враг не сильно ниже — голова прямо, без десинка.
// Умный: смотрим, под каким углом враг видит голову; в «опасной» полосе (своей для стойки) десинк выключаем,
// чтобы голова не уезжала из-за тела вбок, где её проще задеть.
bool safeHeadActive(const csgo::UserCmd* cmd, bool haveThreat, const csgo::Vector& threatOrigin) noexcept
{
    const auto mode = static_cast<SafeHead>(config.antiAim.safeHead);
    if (mode == SafeHead::Off || !haveThreat)
        return false;
    const auto& player = localPlayer.get();
    const int flags = player.flags();
    const bool onGround = (flags & FL_ONGROUND) && !(cmd->buttons & csgo::UserCmd::IN_JUMP);
    const bool crouching = (cmd->buttons & csgo::UserCmd::IN_DUCK) || (flags & FL_DUCKING);
    const auto weapon = csgo::Entity::from(retSpoofGadgets->client, player.getActiveWeapon());
    const bool knife = weapon.getPOD() && weapon.getWeaponType() == WeaponType::Knife;

    if (mode == SafeHead::Basic)
        return !onGround && knife && player.origin().z - threatOrigin.z > -32.0f;

    const auto head = player.getEyePosition();
    const float threatEyeZ = threatOrigin.z + 64.0f;
    const float distance = std::max((player.origin() - threatOrigin).length(), 1.0f);
    const float ratio = (head.z - threatEyeZ) / distance;
    float low = 0.0f, high = 0.75f;
    if (onGround && !crouching) {
        low = 0.25f;
        high = 0.5f;
    } else if (onGround && crouching) {
        low = -0.05f;
        high = 0.3f;
    } else if (!crouching) {
        low = 0.15f;
        high = 0.75f;
    } else if (knife) {
        low = -0.05f;
        high = 0.55f;
    } else {
        low = 0.1f;
        high = 0.75f;
    }
    return ratio >= low && ratio <= high;
}

// Легит AA: можно ли забрать E себе. Дефьюз и бомбу под ногами у КТ, смотрящего вниз, не трогаем
bool legitAllowed(const csgo::UserCmd* cmd, const ClientInterfaces& clientInterfaces) noexcept
{
    const auto& player = localPlayer.get();
    if (Netvars::get(fnv::hash("CCSPlayer->m_bIsDefusing")) && player.isDefusing())
        return false;
    if (player.getTeamNumber() == csgo::Team::CT && cmd->viewangles.x > 15.0f) {
        const auto list = clientInterfaces.getEntityList();
        const int highest = list.getHighestEntityIndex();
        for (int i = 1; i <= highest; ++i) {
            const auto entity = csgo::Entity::from(retSpoofGadgets->client, list.getEntity(i));
            if (!entity.getPOD() || entity.getNetworkable().isDormant())
                continue;
            const auto clientClass = entity.getNetworkable().getClientClass();
            if (clientClass && clientClass->classId == ClassId::PlantedC4 && (entity.getAbsOrigin() - player.origin()).length() < 61.0f)
                return false;
        }
    }
    return true;
}

}

void updateInput() noexcept
{
    auto& aa = config.antiAim;
    aa.inverter.handleToggle();
    toggleManual(aa.manualLeft, Manual::Left);
    toggleManual(aa.manualRight, Manual::Right);
    toggleManual(aa.manualBack, Manual::Back);
    toggleManual(aa.manualForward, Manual::Forward);
}

bool isFreestanding() noexcept
{
    return freestandActive;
}

const char* stateName() noexcept
{
    const int index = static_cast<int>(activeState);
    return index >= 0 && index < static_cast<int>(aaStateNames.size()) ? aaStateNames[index] : "";
}

bool defensiveActive() noexcept
{
    return snap.active;
}

float rollAngle() noexcept
{
    return currentRoll;
}

void fixRollMovement(csgo::UserCmd* cmd) noexcept
{
    if (currentRoll == 0.0f)
        return;
    // Сервер считает движение по forward/right из углов команды, обнулив z и нормировав.
    // С наклоном (roll) при опущенном pitch вектор «вправо» поворачивается — решаем, какие forward/side
    // дадут то же движение, что без наклона (как movement_fix в hysteria).
    const float yaw = Helpers::deg2rad(cmd->viewangles.y);
    const float pitch = Helpers::deg2rad(cmd->viewangles.x);
    const float roll = Helpers::deg2rad(currentRoll);
    const float cy = std::cos(yaw), sy = std::sin(yaw), sp = std::sin(pitch), cp = std::cos(pitch), sr = std::sin(roll), cr = std::cos(roll);
    // желаемое движение — в базисе без наклона
    const float wantX = cmd->forwardmove * cy + cmd->sidemove * sy;
    const float wantY = cmd->forwardmove * sy - cmd->sidemove * cy;
    float fx = cp * cy, fy = cp * sy;
    float rx = -sr * sp * cy + cr * sy, ry = -sr * sp * sy - cr * cy;
    const float fl = std::hypot(fx, fy), rl = std::hypot(rx, ry);
    if (fl < 1e-4f || rl < 1e-4f)
        return;
    fx /= fl; fy /= fl; rx /= rl; ry /= rl;
    const float det = fx * ry - fy * rx;
    if (std::abs(det) < 1e-4f)
        return;
    cmd->forwardmove = (wantX * ry - wantY * rx) / det;
    cmd->sidemove = (fx * wantY - fy * wantX) / det;
}

Manual manualDirection() noexcept
{
    return manual;
}

int desyncSide() noexcept
{
    return side;
}

static float sentPitch = 0.0f, sentYaw = 0.0f;
static bool hasSent = false;

void storeSentAngles(float pitch, float yaw) noexcept
{
    sentPitch = pitch;
    sentYaw = yaw;
    hasSent = true;
}

bool sentAngles(float& pitch, float& yaw) noexcept
{
    pitch = sentPitch;
    yaw = sentYaw;
    return hasSent;
}

void run(csgo::UserCmd* cmd, const EngineInterfaces& engineInterfaces, const ClientInterfaces& clientInterfaces, const Memory& memory, bool shotThisTick) noexcept
{
    auto& aa = config.antiAim;
    currentRoll = 0.0f;
    snap.active = false;
    if (!aa.enabled || !localPlayer || !localPlayer.get().isAlive())
        return;

    const float realtime = memory.globalVars->realtime;
    const float curtime = memory.globalVars->currenttime;
    const auto camera = cmd->viewangles;

    // ---- анти-брутфорс: триггеры и таймер ----
    if (aa.antiBrute && (aa.antiBruteTriggers & AbEnemyShot))
        processImpacts(engineInterfaces, clientInterfaces, memory);
    else
        pendingImpacts.clear();
    if (aa.antiBrute && (aa.antiBruteTriggers & AbLocalShot) && shotThisTick)
        triggerAntiBrute(realtime);
    if (!aa.antiBrute || (aa.antiBruteReset > 0.0f && lastAntiBrute >= 0.0f && realtime - lastAntiBrute > aa.antiBruteReset))
        if (lastAntiBrute >= 0.0f || haveOriginal)
            resetAntiBrute();

    const auto moveType = localPlayer.get().moveType();
    if (moveType == MoveType::LADDER || moveType == MoveType::NOCLIP)
        return;

    // ---- легит AA на E: первый тик E уходит как есть (дверь, бомба), дальше E забираем себе ----
    bool legit = false;
    if (cmd->buttons & csgo::UserCmd::IN_USE) {
        if (!aa.legitAa)
            return;
        if (!legitWait) {
            legitWait = true;
            return;
        }
        if (!legitAllowed(cmd, clientInterfaces))
            return;
        cmd->buttons &= ~csgo::UserCmd::IN_USE;
        legit = true;
    } else {
        legitWait = false;
    }

    if (const auto weapon = csgo::Entity::from(retSpoofGadgets->client, localPlayer.get().getActiveWeapon()); weapon.getPOD()) {
        // граната: пока зажат бросок, выдернута чека или идёт сам бросок (он происходит через
        // несколько тиков после отпускания кнопки, по m_fThrowTime) — углы не трогаем,
        // иначе граната улетает по антиаимному pitch себе под ноги
        if (weapon.isGrenade()) {
            if (cmd->buttons & (csgo::UserCmd::IN_ATTACK | csgo::UserCmd::IN_ATTACK2))
                return;
            if (Netvars::get(fnv::hash("CBaseCSGrenade->m_bPinPulled")) && weapon.pinPulled())
                return;
            if (Netvars::get(fnv::hash("CBaseCSGrenade->m_fThrowTime")) && weapon.throwTime() > 0.0f)
                return;
        }

        // ручной выстрел, когда оружие готово, — углы камеры не трогаем
        const float serverTime = memory.globalVars->serverTime();
        bool ready = weapon.nextPrimaryAttack() <= serverTime && localPlayer.get().nextAttack() <= serverTime;
        // револьвер с зажатым ЛКМ сначала взводится (и «Prepare revolver» держит ЛКМ почти всегда) —
        // выстрел только когда барабан готов, до этого AA работает
        if (ready && weapon.itemDefinitionIndex() == WeaponId::Revolver && Netvars::get(fnv::hash("CWeaponCSBase->m_flPostponeFireReadyTime"))) {
            const float fireReady = weapon.postponeFireReadyTime();
            ready = fireReady > 0.0f && fireReady <= serverTime;
        }
        if ((cmd->buttons & csgo::UserCmd::IN_ATTACK) && ready) {
            if (aa.antiBrute && (aa.antiBruteTriggers & AbLocalShot))
                triggerAntiBrute(realtime);
            return;
        }
    }

    // тик выстрела не трогаем — там углы рейджбота
    if (shotThisTick)
        return;

    const bool sendPacket = tickbase::sendPacket();

    // ---- угроза: ближайший по прицелу враг ----
    csgo::Vector threatEye{};
    bool haveThreat = false;
    const float toTarget = atTargetsYaw(cmd, engineInterfaces, clientInterfaces, memory, &threatEye, &haveThreat);
    csgo::Vector threatOrigin = threatEye;
    threatOrigin.z -= 64.0f;
    const auto eye = localPlayer.get().getEyePosition();
    const float targetPitch = haveThreat ? (threatEye - eye).toAngle().x : camera.x;

    // ---- состояние и его настройки ----
    activeState = detectState(cmd);
    const AaState settingsState = resolveState(activeState);
    AntiAimState st = blended(settingsState, realtime);
    // анти-брутфорс «Подстройка»: настройки чуть уводим в случайную сторону, его брутфорс промахивается
    if (antiBruteWeight != 0.0f) {
        st.yawOffset += static_cast<int>(std::lround(antiBruteWeight * 15.0f));
        st.degree = std::max(0, static_cast<int>(std::lround(st.degree * (1.0f + antiBruteWeight))));
        const float scale = 1.0f - std::abs(antiBruteWeight) * 0.5f;
        st.desyncLeft = static_cast<int>(std::lround(st.desyncLeft * scale));
        st.desyncRight = static_cast<int>(std::lround(st.desyncRight * scale));
    }

    if (sendPacket && ++packetsSinceSwitch >= switchWait) {
        packetsSinceSwitch = 0;
        switchWait = nextSwitchWait(st);
        ++phase;
        const float spread = static_cast<float>(std::clamp(st.randomize, 0, 90));
        switchRandom = spread > 0.0f ? std::uniform_real_distribution<float>{ -spread, spread }(rng) : 0.0f;
        const float half = static_cast<float>(st.degree) * 0.5f;
        randomYaw = half > 0.0f ? std::uniform_real_distribution<float>{ -half, half }(rng) : 0.0f;
        randomSide = std::uniform_int_distribution<int>{ 0, 1 }(rng) ? 1 : -1;
    }

    // Что «запрещают» особые режимы — как флаги no_offset/no_modifier/force_desync в hysteria
    bool noOffset = false, noModifier = false, noSnap = false;
    std::optional<int> forceDesync;     // 0 — без десинка, ±1 — полный в эту сторону

    // ---- pitch ----
    if (legit) {
        cmd->viewangles.x = camera.x;
    } else {
        switch (static_cast<Pitch>(aa.pitch)) {
        case Pitch::Down: cmd->viewangles.x = 89.0f; break;
        case Pitch::Up: cmd->viewangles.x = -89.0f; break;
        case Pitch::Zero: cmd->viewangles.x = 0.0f; break;
        default: break;
        }
    }

    // ---- база yaw ----
    float yaw = camera.y;
    freestandActive = false;
    if (legit) {
        noOffset = noModifier = noSnap = true;
    } else {
        switch (static_cast<YawBase>(aa.yawBase)) {
        case YawBase::Backward: yaw += 180.0f; break;
        case YawBase::AtTargets: yaw = toTarget + 180.0f; break;
        default: break;
        }
    }

    const int inverterSide = aa.inverter.isToggled() ? 1 : -1;

    // ---- фристенд (сырое направление нужно ещё и дефенсиву «у стены») ----
    std::optional<float> freestandRaw;
    const bool freestandHeld = aa.freestandKey.isSet() && aa.freestandKey.isDown();
    const bool wantFreestand = !legit && aa.freestanding && (freestandHeld || (!aa.freestandKey.isSet() && manual == Manual::None));
    const bool wantEdgeBait = !legit && aa.defensiveAa && aa.snaps[static_cast<int>(SnapState::EdgeBait)].mode != static_cast<int>(SnapMode::Default);
    bool freestandOverridesManual = false;
    if (wantFreestand || wantEdgeBait) {
        csgo::Vector enemyEye;
        bool found = false;
        float toEnemy = atTargetsYaw(cmd, engineInterfaces, clientInterfaces, memory, &enemyEye, &found, true);
        // врагов не видно — считаем, что они там, куда смотрит камера (стену сбоку это не меняет)
        if (!found) {
            toEnemy = camera.y;
            const float radians = Helpers::deg2rad(toEnemy);
            enemyEye = { eye.x + std::cos(radians) * 2000.0f, eye.y + std::sin(radians) * 2000.0f, eye.z };
        }
        float offset = freestandYaw(engineInterfaces.engineTrace(), enemyEye, toEnemy);
        if (offset != 0.0f)
            freestandRaw = offset;
        if (wantFreestand) {
            if (offset != 0.0f)
                freestandHoldTicks = 8;
            // Гистерезис: новую сторону берём, только если она держится 3 тика подряд;
            // пропало, от чего прятать, — ещё 8 тиков держим прошлую
            if (offset != 0.0f && freestandSide != 0.0f && (offset > 0.0f) != (freestandSide > 0.0f)) {
                if (++freestandFlipTicks < 3)
                    offset = freestandSide * 90.0f;
                else
                    freestandFlipTicks = 0;
            } else {
                freestandFlipTicks = 0;
            }
            if (offset == 0.0f && freestandSide != 0.0f && freestandHoldTicks > 0) {
                --freestandHoldTicks;
                offset = freestandSide * 90.0f;
            }
            if (offset != 0.0f) {
                yaw = toEnemy + offset;
                freestandActive = true;
                freestandSide = offset > 0.0f ? 1.0f : -1.0f;
                freestandOverridesManual = freestandHeld;
                if (aa.freestandStatic)
                    noOffset = noModifier = true;
            } else {
                freestandSide = 0.0f;
            }
        }
    }
    if (!wantFreestand) {
        freestandSide = 0.0f;
        freestandHoldTicks = 0;
    }

    // ---- edge yaw: стена вплотную — лицом в неё ----
    const bool edgeWanted = !legit && aa.edgeYaw && (!aa.edgeKey.isSet() || aa.edgeKey.isDown()) && manual == Manual::None && !freestandActive;
    if (edgeWanted) {
        if (const auto edge = edgeYaw(engineInterfaces.engineTrace())) {
            yaw = *edge;
            if (aa.edgeStatic) {
                noOffset = noModifier = true;
                forceDesync = inverterSide;
            }
        }
    }

    // ---- увод от прицела (враг смотрит на нас) ----
    if (!legit && aa.defensive) {
        float threatYaw = 0.0f;
        if (enemyAimingAtUs(engineInterfaces, clientInterfaces, memory, static_cast<float>(aa.defensiveFov), threatYaw)) {
            if (aa.defensiveJitter && sendPacket)
                defensiveFlip = !defensiveFlip;
            const float direction = defensiveFlip ? 1.0f : -1.0f;
            yaw = threatYaw + direction * static_cast<float>(std::clamp(aa.defensiveYaw, 0, 180));
            freestandActive = false;
            freestandOverridesManual = false;
            cmd->viewangles.x = static_cast<float>(std::clamp(aa.defensivePitch, -89, 89));
        }
    }

    // ---- ручной AA ----
    if (!legit) {
        const auto direction = freestandOverridesManual ? Manual::None : manual;
        switch (direction) {
        case Manual::Left: yaw = camera.y + 90.0f; break;
        case Manual::Right: yaw = camera.y - 90.0f; break;
        case Manual::Back: yaw = camera.y + 180.0f; break;
        case Manual::Forward: yaw = camera.y; break;
        default: break;
        }
        if (direction != Manual::None && aa.staticOnManual) {
            noOffset = noModifier = true;
            forceDesync = inverterSide;
        }
    }

    // ---- safe head ----
    if (!legit && manual == Manual::None && safeHeadActive(cmd, haveThreat, threatOrigin)) {
        noOffset = noModifier = true;
        forceDesync = 0;
    }

    // ---- roll ----
    if (!legit && aa.roll != 0) {
        float roll = static_cast<float>(std::clamp(aa.roll, -50, 50)) * (aa.inverter.isToggled() ? -1.0f : 1.0f);
        const bool onGround = localPlayer.get().flags() & FL_ONGROUND;
        if (localPlayer.get().velocity().length2D() > 5.0f || !onGround)
            roll = std::clamp(roll, -40.0f, 40.0f);
        currentRoll = roll;
        noOffset = noModifier = noSnap = true;
        forceDesync = roll > 0.0f ? 1 : -1;
    }

    // ---- дефенсив AA (скрытые углы) ----
    int lcLeft = tickbase::defensiveTicksLeft();
    const DefensiveSnap* chosenSnap = nullptr;
    const bool triggerWants = !legit && aa.lcTriggers && (aa.lcTriggerStates & (1 << static_cast<int>(activeState)));
    if (aa.defensiveAa && !legit) {
        chosenSnap = activeSnap(selectSnapState(cmd, freestandRaw.has_value()));
        // LC включён (триггер этого состояния или Force LC) — скрытые углы держим всегда, а не только
        // 2–3 тика окна после рывка: между рывками окно идёт по кругу «длительность → 1», как настоящее.
        const bool always = chosenSnap && (triggerWants || chosenSnap->forceLc);
        static int synthetic = 0;
        if (always && lcLeft == 0) {
            const int length = std::clamp(chosenSnap->duration, 1, 13);
            synthetic = synthetic > 1 ? synthetic - 1 : length;
            lcLeft = synthetic;
        } else {
            synthetic = 0;
        }
        bool inWindow = chosenSnap && lcLeft > 0;
        if (inWindow) {
            ++snap.ticks;
            inWindow = always || snap.ticks <= std::clamp(chosenSnap->duration, 1, 13);
        } else {
            snap.ticks = 0;
        }
        if (inWindow && !noSnap) {
            if (!snap.active && snap.apex == 0)
                snap.apex = lcLeft;
            snap.active = true;
            if (snap.delayed >= chosenSnap->yawDelay + 1) {
                snap.jitter = !snap.jitter;
                snap.delayed = 0;
            } else if (sendPacket) {
                ++snap.delayed;
            }
            if (const auto pitch = snapPitch(*chosenSnap, curtime, lcLeft, camera.x, targetPitch))
                cmd->viewangles.x = std::clamp(*pitch, -89.0f, 89.0f);
            if (const auto offset = snapYaw(*chosenSnap, curtime, lcLeft, freestandRaw)) {
                yaw = (haveThreat ? toTarget : camera.y) + *offset;
                noOffset = noModifier = true;
            }
        } else if (lcLeft == 0 && snap.apex != 0) {
            // окно закончилось — следующее начнётся с чистого листа
            snap.counter = (snap.counter + 1) & 0xFFFF;
            snap.apex = 0;
            snap.delayed = 0;
            snap.haveRandomPitch = snap.haveRandomYaw = false;
        }
    }

    // ---- ломать лагкомп: Force LC у дефенсива и LC-триггеры по состояниям ----
    if (!legit) {
        const bool snapWants = chosenSnap && chosenSnap->forceLc;
        if (snapWants || triggerWants)
            tickbase::requestBreakLc();
    }

    if (!noOffset)
        yaw += static_cast<float>(st.yawOffset);
    const float degree = static_cast<float>(st.degree) + switchRandom;
    const float modifier = noModifier ? 0.0f : modifierYaw(st, degree);

    // ---- флики ----
    bool flick = false;
    if (aa.flicks && !legit) {
        if (tickbase::isShifting())
            flickAfterShift = 2;
        flick = tickbase::isShifting() || flickAfterShift > 0;
        if (!flick && aa.flickTrigger == static_cast<int>(FlickTrigger::Always) && sendPacket && ++flickCounter >= std::max(aa.flickEvery, 3)) {
            flickCounter = 0;
            flick = true;
        }
        if (!tickbase::isShifting() && sendPacket && flickAfterShift > 0)
            --flickAfterShift;
        // скрытые углы дефенсива главнее
        if (snap.active)
            flick = false;
    }
    if (flick) {
        if (!wasFlicking) {
            flickDirection = aa.flickSide == static_cast<int>(FlickSide::Random)
                ? (std::uniform_int_distribution<int>{ 0, 1 }(rng) ? 1.0f : -1.0f) : -flickDirection;
        }
        yaw += aa.flickSide == static_cast<int>(FlickSide::Custom)
            ? static_cast<float>(std::clamp(aa.flickYaw, -180, 180))
            : flickDirection * static_cast<float>(aa.flickAngle);
        if (aa.flickPitchOverride)
            cmd->viewangles.x = static_cast<float>(std::clamp(aa.flickPitch, -89, 89));
    }
    wasFlicking = flick;

    // ---- десинк: на чокнутых тиках тянем тело в сторону, реальный угол уходит в отправленном ----
    const auto desyncMode = static_cast<DesyncMode>(st.desyncMode);
    const bool desyncOn = aa.desync && tickbase::isInstalled() && (desyncMode != DesyncMode::Off || (forceDesync && *forceDesync != 0));
    if (desyncOn && !(forceDesync && *forceDesync == 0)) {
        side = inverterSide;
        if (st.desyncInvert)
            side = -side;
        if (aa.antiBrute && antiBruteFlip)
            side = -side;
        if (forceDesync) {
            side = *forceDesync;
        } else if (!noModifier) {
            if (desyncMode == DesyncMode::Jitter) {
                // скиттер ведёт сторону своим порядком (минус — другая сторона), остальные — через раз
                if (static_cast<YawModifier>(st.modifier) == YawModifier::Skitter) {
                    if (skitterSequence[phase % std::size(skitterSequence)] < 0)
                        side = -side;
                } else if (phase & 1) {
                    side = -side;
                }
            } else if (desyncMode == DesyncMode::Random) {
                side *= randomSide;
            }
        }
        // при фристенде тело уводим туда же, куда голову: от врага за укрытие
        if (freestandActive)
            side = freestandSide > 0.0f ? 1 : -1;

        // добавка к yaw по стороне: голова идёт за десинком, а не стоит посередине
        yaw += modifier + (noOffset ? 0.0f : static_cast<float>(side > 0 ? st.addLeft : st.addRight));

        // ручной/статика/roll — десинк на полную; иначе — из билдера
        const int limit = forceDesync ? 60 : (side > 0 ? st.desyncLeft : st.desyncRight);
        const float amount = static_cast<float>(std::clamp(limit, 0, 60));
        if (!sendPacket && !flick)
            yaw += side * std::min(amount * 2.0f, 120.0f);

        // микродвижение стоя, чтобы сервер не обновлял LBY
        const int flags = localPlayer.get().flags();
        if ((flags & FL_ONGROUND) && localPlayer.get().velocity().length2D() < 3.0f
            && std::abs(cmd->forwardmove) < 1.0f && std::abs(cmd->sidemove) < 1.0f && !(cmd->buttons & csgo::UserCmd::IN_JUMP)) {
            const float micro = (flags & FL_DUCKING) ? 3.25f : 1.1f;
            cmd->sidemove = (cmd->commandNumber & 1) ? micro : -micro;
        }
    } else {
        yaw += modifier;
    }

    cmd->viewangles.y = std::remainder(yaw, 360.0f);
    // слайдвок — в movement::fixMovement, после того как кнопки выставлены под реальный ввод
}

void updateEventListener(const EngineInterfaces& engineInterfaces, const Memory& memory, bool forceRemove) noexcept
{
    // слушатель bullet_impact только пока он нужен
    const auto& aa = config.antiAim;
    const bool wanted = !forceRemove && aa.enabled && aa.antiBrute && (aa.antiBruteTriggers & AbEnemyShot);
    if (wanted && !impactListenerRegistered) {
        engineInterfaces.getGameEventManager(memory.getEventDescriptor).addListener(&impactListener, csgo::bullet_impact);
        impactListenerRegistered = true;
    } else if (!wanted && impactListenerRegistered) {
        engineInterfaces.getGameEventManager(memory.getEventDescriptor).removeListener(&impactListener);
        impactListenerRegistered = false;
        pendingImpacts.clear();
    }
}

void onRoundStart() noexcept
{
    resetAntiBrute();
}

}

#include <algorithm>
#include <cmath>
#include <limits>
#include <random>
#include <vector>

#include "AntiAim.h"
#include "HvhConfig.h"
#include "Tickbase.h"

#include <Interfaces/ClientInterfaces.h>
#include <Interfaces/EngineInterfaces.h>
#include <Memory.h>
#include <CSGO/Engine.h>
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

namespace hvh::antiaim
{

namespace
{

Manual manual = Manual::None;
int side = 1;
bool jitterFlip = false;
bool desyncFlip = false;

// анти-брутфорс: пуля врага прошла рядом с головой — меняем сторону десинка
struct Impact {
    int userId;
    csgo::Vector position;
};
std::vector<Impact> pendingImpacts;
bool antiBruteFlip = false;
float lastAntiBruteFlip = 0.0f;

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

[[nodiscard]] float distanceToSegment(const csgo::Vector& point, const csgo::Vector& a, const csgo::Vector& b) noexcept
{
    const auto ab = b - a;
    const float lengthSquared = ab.x * ab.x + ab.y * ab.y + ab.z * ab.z;
    float t = 0.0f;
    if (lengthSquared > 0.0f)
        t = std::clamp(((point.x - a.x) * ab.x + (point.y - a.y) * ab.y + (point.z - a.z) * ab.z) / lengthSquared, 0.0f, 1.0f);
    return (point - (a + ab * t)).length();
}

void processImpacts(const EngineInterfaces& engineInterfaces, const ClientInterfaces& clientInterfaces, const Memory& memory) noexcept
{
    if (pendingImpacts.empty())
        return;
    const auto head = localPlayer.get().getEyePosition();
    const int localIndex = localPlayer.get().getNetworkable().index();
    for (const auto& impact : pendingImpacts) {
        const int shooterIndex = engineInterfaces.getEngine().getPlayerForUserID(impact.userId);
        if (shooterIndex <= 0 || shooterIndex == localIndex)
            continue;
        const auto shooter = csgo::Entity::from(retSpoofGadgets->client, clientInterfaces.getEntityList().getEntity(shooterIndex));
        if (!shooter.getPOD() || !shooter.isOtherEnemy(memory, localPlayer.get()))
            continue;
        // m_vecViewOffset у чужих игроков не приходит — глаза примерно на 64 над ногами
        auto shooterEye = shooter.getAbsOrigin();
        shooterEye.z += 64.0f;
        // одна пуля даёт несколько импактов (пробитие) — переключаемся не чаще раза в 0.25 с
        if (distanceToSegment(head, shooterEye, impact.position) < 32.0f && memory.globalVars->realtime - lastAntiBruteFlip > 0.25f) {
            antiBruteFlip = !antiBruteFlip;
            lastAntiBruteFlip = memory.globalVars->realtime;
        }
    }
    pendingImpacts.clear();
}
float spinYaw = 0.0f;
int fiveWayStep = 0;
// флики
int flickCounter = 0;
int flickAfterShift = 0;
float flickDirection = 1.0f;
bool wasFlicking = false;
bool defensiveFlip = false;   // сторона дефенсив-увода, дёргается на каждом отправленном пакете
std::mt19937 rng{ std::random_device{}() };

constexpr int FL_ONGROUND = 1 << 0;
constexpr int FL_DUCKING = 1 << 1;

void toggleManual(const KeyBind& key, Manual direction) noexcept
{
    if (key.isSet() && key.isPressed())
        manual = manual == direction ? Manual::None : direction;
}

// угол на ближайшего по прицелу врага, если врагов нет — текущий угол камеры
bool freestandActive = false;

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

// Фристенд: проверяем, где у головы укрытие от врага — слева или справа (луч от его глаз, только стены).
// Возвращает поворот yaw относительно направления на врага: +90/-90, или 0, если укрытия нет / оно с обеих сторон.
float freestandYaw(const csgo::EngineTrace& engineTrace, const csgo::Vector& enemyEye, float yawToEnemy) noexcept
{
    const auto eye = localPlayer.get().getEyePosition();
    const auto sidePoint = [&](float offset, float distance) {
        const float radians = Helpers::deg2rad(yawToEnemy + offset);
        return csgo::Vector{ eye.x + std::cos(radians) * distance, eye.y + std::sin(radians) * distance, eye.z };
    };

    // 1) стена вплотную сбоку от головы (враг для этого не нужен): голову — к ближней стене
    const auto wallDistance = [&](float offset) {
        csgo::Trace trace;
        engineTrace.traceRay({ eye, sidePoint(offset, 45.0f) }, 0x1400B, { localPlayer.get().getPOD() }, trace);
        return trace.fraction * 45.0f;
    };
    const float leftWall = wallDistance(90.0f);
    const float rightWall = wallDistance(-90.0f);
    if (std::min(leftWall, rightWall) < 44.0f && std::abs(leftWall - rightWall) > 4.0f)
        return leftWall < rightWall ? 90.0f : -90.0f;

    // 2) стены рядом нет — какую сторону головы враг видит хуже (луч от его глаз, только стены)
    const auto openness = [&](float offset) {
        float sum = 0.0f;
        for (const float distance : { 15.0f, 30.0f, 45.0f }) {
            csgo::Trace trace;
            engineTrace.traceRay({ enemyEye, sidePoint(offset, distance) }, 0x1400B, { localPlayer.get().getPOD() }, trace);
            sum += trace.fraction;
        }
        return sum / 3.0f;
    };
    const float left = openness(90.0f);
    const float right = openness(-90.0f);
    if (std::abs(left - right) < 0.05f)
        return 0.0f;
    return left < right ? 90.0f : -90.0f;
}

}

void updateInput() noexcept
{
    auto& aa = config.antiAim;
    aa.inverter.handleToggle();
    toggleManual(aa.manualLeft, Manual::Left);
    toggleManual(aa.manualRight, Manual::Right);
    toggleManual(aa.manualBack, Manual::Back);
}

bool isFreestanding() noexcept
{
    return freestandActive;
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
    const auto& aa = config.antiAim;
    if (!aa.enabled || !localPlayer || !localPlayer.get().isAlive())
        return;

    if (aa.antiBrute)
        processImpacts(engineInterfaces, clientInterfaces, memory);
    else
        pendingImpacts.clear();

    const auto moveType = localPlayer.get().moveType();
    if (moveType == MoveType::LADDER || moveType == MoveType::NOCLIP)
        return;

    if (cmd->buttons & csgo::UserCmd::IN_USE)
        return;

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
        if ((cmd->buttons & csgo::UserCmd::IN_ATTACK) && ready)
            return;
    }

    // тик выстрела не трогаем — там углы рейджбота
    if (shotThisTick)
        return;

    const bool sendPacket = tickbase::sendPacket();

    // pitch
    switch (static_cast<Pitch>(aa.pitch)) {
    case Pitch::Down: cmd->viewangles.x = 89.0f; break;
    case Pitch::Up: cmd->viewangles.x = -89.0f; break;
    case Pitch::Zero: cmd->viewangles.x = 0.0f; break;
    default: break;
    }

    // yaw
    float yaw = cmd->viewangles.y;
    freestandActive = false;
    switch (static_cast<YawBase>(aa.yawBase)) {
    case YawBase::Backward: yaw += 180.0f; break;
    case YawBase::AtTargets: yaw = atTargetsYaw(cmd, engineInterfaces, clientInterfaces, memory) + 180.0f; break;
    default: break;
    }

    // фристенд (ручной AA главнее)
    // зажатая клавиша фристенда главнее ручного AA; без клавиши — ручной главнее
    const bool freestandHeld = aa.freestandKey.isSet() && aa.freestandKey.isDown();
    bool freestandOverridesManual = false;
    if (aa.freestanding && (freestandHeld || (!aa.freestandKey.isSet() && manual == Manual::None))) {
        csgo::Vector enemyEye;
        bool found = false;
        float toEnemy = atTargetsYaw(cmd, engineInterfaces, clientInterfaces, memory, &enemyEye, &found, true);
        // врагов не видно — считаем, что они там, куда смотрит камера (стену сбоку это не меняет)
        if (!found) {
            toEnemy = cmd->viewangles.y;
            const auto eye = localPlayer.get().getEyePosition();
            const float radians = Helpers::deg2rad(toEnemy);
            enemyEye = { eye.x + std::cos(radians) * 2000.0f, eye.y + std::sin(radians) * 2000.0f, eye.z };
        }
        {
            if (const float offset = freestandYaw(engineInterfaces.engineTrace(), enemyEye, toEnemy); offset != 0.0f) {
                yaw = toEnemy + offset;
                freestandActive = true;
                freestandOverridesManual = freestandHeld;
            }
        }
    }

    // Дефенсив AA: враг целится в нас — голова вниз (pitch ниже), тело уведено вбок от его прицела.
    // Сторону дёргаем на каждом отправленном пакете: его резольвер видит разные углы и не успевает
    // зафиксировать настоящий. Фристенд при этом уступает — дефенсив важнее, когда смотрят прямо на нас.
    if (aa.defensive) {
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

    switch (freestandOverridesManual ? Manual::None : manual) {
    case Manual::Left: yaw = cmd->viewangles.y + 90.0f; break;
    case Manual::Right: yaw = cmd->viewangles.y - 90.0f; break;
    case Manual::Back: yaw = cmd->viewangles.y + 180.0f; break;
    default: break;
    }

    yaw += static_cast<float>(aa.yawOffset);

    if (sendPacket)
        jitterFlip = !jitterFlip;

    const float range = static_cast<float>(aa.modifierRange);
    switch (static_cast<YawModifier>(aa.yawModifier)) {
    case YawModifier::Jitter:
        yaw += jitterFlip ? range : 0.0f;
        break;
    case YawModifier::CenterJitter:
        yaw += jitterFlip ? range * 0.5f : -range * 0.5f;
        break;
    case YawModifier::Spin:
        spinYaw = std::remainder(spinYaw + static_cast<float>(aa.spinSpeed), 360.0f);
        yaw += spinYaw;
        break;
    case YawModifier::Random:
        if (sendPacket)
            yaw += std::uniform_real_distribution<float>{ -range * 0.5f, range * 0.5f }(rng);
        break;
    case YawModifier::FiveWay: {
        // пять положений по кругу: -½, -¼, 0, +¼, +½ диапазона, шаг на каждый отправленный пакет
        static constexpr float steps[]{ -0.5f, -0.25f, 0.0f, 0.25f, 0.5f };
        if (sendPacket)
            fiveWayStep = (fiveWayStep + 1) % 5;
        yaw += range * steps[fiveWayStep];
        break;
    }
    default:
        break;
    }

    // Флики: на тиках сдвига тикбейса (дефенсив/телепорт/DT) и пару пакетов после — его лагкомп по нам
    // в это время не работает, а модель на сервере смотрит вбок. В режиме «Всегда» — ещё раз в N пакетов.
    bool flick = false;
    if (aa.flicks) {
        if (tickbase::isShifting())
            flickAfterShift = 2;
        flick = tickbase::isShifting() || flickAfterShift > 0;
        if (!flick && aa.flickTrigger == static_cast<int>(FlickTrigger::Always) && sendPacket && ++flickCounter >= std::max(aa.flickEvery, 3)) {
            flickCounter = 0;
            flick = true;
        }
        if (!tickbase::isShifting() && sendPacket && flickAfterShift > 0)
            --flickAfterShift;
    }
    if (flick) {
        if (!wasFlicking) {
            // новая серия фликов — новая сторона
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

    // десинк: на чокнутых тиках тянем тело в сторону, реальный угол уходит в отправленном
    if (aa.desync && tickbase::isInstalled()) {
        if (aa.desyncJitter && sendPacket)
            desyncFlip = !desyncFlip;

        side = aa.inverter.isToggled() ? 1 : -1;
        if (aa.antiBrute && antiBruteFlip)
            side = -side;
        if (aa.desyncJitter && desyncFlip)
            side = -side;

        const float amount = static_cast<float>(side > 0 ? aa.desyncLeft : aa.desyncRight);
        if (!sendPacket && !flick)
            yaw += side * std::min(amount * 2.0f, 120.0f);

        // микродвижение стоя, чтобы сервер не обновлял LBY
        const int flags = localPlayer.get().flags();
        if ((flags & FL_ONGROUND) && localPlayer.get().velocity().length2D() < 3.0f
            && std::abs(cmd->forwardmove) < 1.0f && std::abs(cmd->sidemove) < 1.0f && !(cmd->buttons & csgo::UserCmd::IN_JUMP)) {
            const float micro = (flags & FL_DUCKING) ? 3.25f : 1.1f;
            cmd->sidemove = (cmd->commandNumber & 1) ? micro : -micro;
        }
    }

    cmd->viewangles.y = std::remainder(yaw, 360.0f);
    // слайдвок — в movement::fixMovement, после того как кнопки выставлены под реальный ввод
}

void updateEventListener(const EngineInterfaces& engineInterfaces, const Memory& memory, bool forceRemove) noexcept
{
    // слушатель bullet_impact только пока он нужен
    const auto& aa = config.antiAim;
    const bool wanted = !forceRemove && aa.enabled && aa.desync && aa.antiBrute;
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
    antiBruteFlip = false;
}

}

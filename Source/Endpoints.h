#pragma once

#include <Platform/Macros/IsPlatform.h>

#if IS_WIN32() || IS_WIN64()
#include "imgui/imgui_impl_dx9.h"
#include "imgui/imgui_impl_win32.h"

#include "Hooks/WindowProcedureHook.h"
#endif

#include <CSGO/NetworkChannel.h>

#include "EventListener.h"
#include "GlobalContext.h"

#include <Platform/Macros/CallStack.h>

#include "Hooks/BspQueryHooks.h"
#include "Hooks/ClientHooks.h"
#include "Hooks/ClientModeHooks.h"
#include "Hooks/ClientStateHooks.h"
#include "Hooks/CSPlayerInventoryHooks.h"
#include "Hooks/EngineHooks.h"
#include "Hooks/EngineSoundHooks.h"
#include "Hooks/InventoryManagerHooks.h"
#include "Hooks/KeyValuesSystemHooks.h"
#include "Hooks/ModelRenderHooks.h"
#include "Hooks/PanoramaMarshallHelperHooks.h"
#include "Hooks/SurfaceHooks.h"
#include "Hooks/SvCheatsHooks.h"
#include "Hooks/ViewRenderHooks.h"

#include "CSGO/Constants/GameEventNames.h"
#include "CSGO/Constants/UserMessages.h"
#include "CSGO/ClientClass.h"
#include "CSGO/GameEvent.h"
#include "CSGO/GlobalVars.h"
#include "CSGO/Input.h"
#include "CSGO/LocalPlayer.h"
#include "CSGO/ModelRender.h"
#include "CSGO/SoundInfo.h"
#include "CSGO/UserCmd.h"
#include "CSGO/ViewSetup.h"
#include "CSGO/EngineTrace.h"
#include <cstring>
#include "GameData.h"
#include "GUI.h"
#include "Hacks/EnginePrediction.h"
#include "Hacks/Sound.h"
#include "Hacks/Triggerbot.h"
#include "Hacks/HvH/AntiAim.h"
#include "Hacks/HvH/GrenadePrediction.h"
#include "Hacks/HvH/HvhConfig.h"
#include "Hacks/HvH/ModelPreview.h"
#include "Hacks/HvH/Movement.h"
#include "Hacks/HvH/Ragebot.h"
#include "Hacks/HvH/Defensive.h"
#include "Hacks/HvH/Smoothing.h"
#include "Hacks/HvH/RadarSpotted.h"
#include "Hacks/HvH/Resolver.h"
#include "Hacks/HvH/Tickbase.h"
#include "Hacks/HvH/HvhVisuals.h"
#include "Hacks/HvH/Scoreboard.h"
#include "Hacks/HvH/Extras.h"
#include "Hooks.h"
#include "Netvars.h"

void fireGameEvent(csgo::GameEventPOD* eventPointer)
{
    const auto event = csgo::GameEvent::from(retSpoofGadgets->client, eventPointer);

    switch (fnv::hashRuntime(event.getName())) {
    case fnv::hash(csgo::round_start):
        hvh::ragebot::onRoundStart();
        hvh::antiaim::onRoundStart();
        hvh::movement::onRoundStart();
        hvh::defensive::onRoundStart();
        GameData::clearProjectileList();
        globalContext->features->misc.preserveKillfeed(true);
        [[fallthrough]];
    case fnv::hash(csgo::round_freeze_end):
        globalContext->features->misc.purchaseList(&event);
        break;
    case fnv::hash(csgo::player_death):
        hvh::ragebot::onPlayerDeath(globalContext->getEngineInterfaces().getEngine().getPlayerForUserID(event.getInt("userid")),
            localPlayer ? localPlayer.get().getNetworkable().index() : 0);
        if (localPlayer && globalContext->getEngineInterfaces().getEngine().getPlayerForUserID(event.getInt("attacker")) == localPlayer.get().getNetworkable().index()
            && globalContext->getEngineInterfaces().getEngine().getPlayerForUserID(event.getInt("userid")) != localPlayer.get().getNetworkable().index())
            hvh::visuals::onKill();
        {
            const auto& engine = globalContext->getEngineInterfaces().getEngine();
            hvh::extras::onDeath(engine, engine.getPlayerForUserID(event.getInt("attacker")), engine.getPlayerForUserID(event.getInt("userid")),
                localPlayer ? localPlayer.get().getNetworkable().index() : 0, event.getInt("headshot") != 0);
        }
        globalContext->features->inventoryChanger.updateStatTrak(event);
        globalContext->features->inventoryChanger.overrideHudIcon(*globalContext->memory, event);
        globalContext->features->misc.killMessage(event);
        globalContext->features->misc.killSound(event);
        break;
    case fnv::hash(csgo::player_hurt): {
        const auto& engine = globalContext->getEngineInterfaces().getEngine();
        hvh::ragebot::onPlayerHurt(engine.getPlayerForUserID(event.getInt("userid")), engine.getPlayerForUserID(event.getInt("attacker")), localPlayer ? localPlayer.get().getNetworkable().index() : 0,
            event.getInt("hitgroup"), event.getInt("dmg_health"), event.getInt("health"));
        if (localPlayer && engine.getPlayerForUserID(event.getInt("attacker")) == localPlayer.get().getNetworkable().index()
            && engine.getPlayerForUserID(event.getInt("userid")) != localPlayer.get().getNetworkable().index())
            hvh::visuals::onDamage(engine.getPlayerForUserID(event.getInt("userid")), event.getInt("dmg_health"), event.getInt("hitgroup") == 1);
    }
        globalContext->features->misc.playHitSound(event);
        globalContext->features->visuals.hitEffect(&event);
        globalContext->features->visuals.hitMarker(&event);
        break;
    case fnv::hash(csgo::vote_cast):
        globalContext->features->misc.voteRevealer(event);
        break;
    case fnv::hash(csgo::round_mvp):
        globalContext->features->inventoryChanger.onRoundMVP(event);
        break;
    case fnv::hash(csgo::item_purchase):
        globalContext->features->misc.purchaseList(&event);
        break;
    case fnv::hash(csgo::bullet_impact):
        globalContext->features->visuals.bulletTracer(event);
        break;
    }
}

void DefaultEventListener::fireGameEvent(csgo::GameEventPOD* eventPointer)
{
    ::fireGameEvent(eventPointer);
}

void EventListener::fireGameEvent(csgo::GameEventPOD* eventPointer)
{
    ::fireGameEvent(eventPointer);
}

void CDECL_CONV viewModelSequence(csgo::recvProxyData* data, void* outStruct, void* arg3) noexcept
{
    const auto viewModel = csgo::Entity::from(retSpoofGadgets->client, static_cast<csgo::EntityPOD*>(outStruct));

    if (localPlayer && ClientInterfaces{ retSpoofGadgets->client, *globalContext->clientInterfaces }.getEntityList().getEntityFromHandle(viewModel.owner()) == localPlayer.get().getPOD()) {
        if (const auto weapon = csgo::Entity::from(retSpoofGadgets->client, ClientInterfaces{ retSpoofGadgets->client, *globalContext->clientInterfaces }.getEntityList().getEntityFromHandle(viewModel.weapon())); weapon.getPOD() != nullptr) {
            if (globalContext->features->visuals.isDeagleSpinnerOn() && weapon.getNetworkable().getClientClass()->classId == ClassId::Deagle && data->value._int == 7)
                data->value._int = 8;

            globalContext->features->inventoryChanger.fixKnifeAnimation(weapon, data->value._int, *globalContext->randomGenerator);
        }
    }

    proxyHooks.viewModelSequence.originalProxy(data, outStruct, arg3);
}

void CDECL_CONV spottedHook(csgo::recvProxyData* data, void* outStruct, void* arg3) noexcept
{
    if (globalContext->features->misc.isRadarHackOn()) {
        data->value._int = 1;

        if (localPlayer) {
            const auto entity = csgo::Entity::from(retSpoofGadgets->client, static_cast<csgo::EntityPOD*>(outStruct));
            if (const auto index = localPlayer.get().getNetworkable().index(); index > 0 && index <= 32)
                entity.spottedByMask() |= 1 << (index - 1);
        }
    }

    proxyHooks.spotted.originalProxy(data, outStruct, arg3);
}

#if IS_WIN32() || IS_WIN64()

LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

LRESULT __stdcall WindowProcedureHook::wndProc(HWND window, UINT msg, WPARAM wParam, LPARAM lParam) noexcept
{
    if (globalContext->state == GlobalContextState::Initialized) {
        ImGui_ImplWin32_WndProcHandler(window, msg, wParam, lParam);
        globalContext->getOtherInterfaces().getInputSystem().enableInput(!gui->isOpen());
    } else if (globalContext->state == GlobalContextState::NotInitialized) {
        globalContext->state = GlobalContextState::Initializing;
        ImGui::CreateContext();
        ImGui_ImplWin32_Init(window);
        globalContext->initialize();
        globalContext->state = GlobalContextState::Initialized;
    }

    return CallWindowProcW(globalContext->windowProcedureHook->originalWndProc, window, msg, wParam, lParam);
}

HRESULT __stdcall reset(IDirect3DDevice9* device, D3DPRESENT_PARAMETERS* params) noexcept
{
    ImGui_ImplDX9_InvalidateDeviceObjects();
    globalContext->features->inventoryChanger.clearItemIconTextures();
    GameData::clearTextures();
    return globalContext->hooks->originalReset(device, params);
}

HRESULT __stdcall present(IDirect3DDevice9* device, const RECT* src, const RECT* dest, HWND windowOverride, const RGNDATA* dirtyRegion) noexcept
{
    [[maybe_unused]] static bool imguiInit{ ImGui_ImplDX9_Init(device) };

    if (globalContext->config->loadScheduledFonts())
        ImGui_ImplDX9_DestroyFontsTexture();

    ImGui_ImplDX9_NewFrame();
    ImGui_ImplWin32_NewFrame();

    globalContext->renderFrame();

    if (device->BeginScene() == D3D_OK) {
        ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
        device->EndScene();
    }

    //
    GameData::clearUnusedAvatars();
    globalContext->features->inventoryChanger.clearUnusedItemIconTextures();
    //

    return globalContext->hooks->originalPresent(device, src, dest, windowOverride, dirtyRegion);
}

#elif IS_LINUX()

int pollEvent(SDL_Event* event) noexcept
{
    return globalContext->pollEventHook(event);
}

void swapWindow(SDL_Window* window) noexcept
{
    globalContext->swapWindowHook(window);
}

#endif

bool FASTCALL_CONV ClientModeHooks::createMove(FASTCALL_THIS(csgo::ClientMode* thisptr), float inputSampleTime, csgo::UserCmd* cmd) noexcept
{
    auto result = globalContext->hooks->clientModeHooks.getOriginalCreateMove()(thisptr, inputSampleTime, cmd);

    if (!cmd->commandNumber)
        return result;

    static auto previousViewAngles{ cmd->viewangles };
    const auto currentViewAngles{ cmd->viewangles };

    globalContext->memory->globalVars->serverTime(cmd);
    auto& features = globalContext->features;
    features->misc.nadePredict();
    features->misc.antiAfkKick(cmd);
    features->misc.fastStop(cmd);
    features->misc.prepareRevolver(cmd);
    features->visuals.removeShadows();
    features->misc.bunnyHop(cmd);
    features->misc.autoStrafe(cmd);
    hvh::movement::bunnyHop(cmd);
    hvh::movement::autoStrafe(cmd, cmd->viewangles.y, globalContext->getOtherInterfaces().getCvar(), globalContext->memory->globalVars->intervalPerTick);
    features->misc.removeCrouchCooldown(cmd);
    features->misc.autoPistol(cmd);
    features->misc.autoReload(cmd);
    features->misc.updateClanTag();
    features->misc.fakeBan();
    features->misc.stealNames();
    features->misc.revealRanks(cmd);
    features->misc.quickReload(cmd);
    features->misc.fixTabletSignal();
    features->misc.slowwalk(cmd);
    hvh::movement::fakeDuck(cmd, globalContext->memory->globalVars->intervalPerTick);

    hvh::movement::noFallDamagePre();
    EnginePrediction::run(ClientInterfaces{ retSpoofGadgets->client, *globalContext->clientInterfaces }, *globalContext->memory, cmd);
    hvh::movement::noFallDamage(cmd, EnginePrediction::getFlags());

    const ClientInterfaces clientInterfaces{ retSpoofGadgets->client, *globalContext->clientInterfaces };
    hvh::tickbase::setActive(localPlayer && localPlayer.get().isAlive());
    if (localPlayer && localPlayer.get().isAlive()) {
        hvh::tickbase::updateLagComp(localPlayer.get().tickBase());
        hvh::tickbase::setMovement((localPlayer.get().flags() & 1) != 0, (cmd->buttons & csgo::UserCmd::IN_JUMP) != 0, localPlayer.get().velocity().z, localPlayer.get().velocity().length2D(), globalContext->memory->globalVars->intervalPerTick);   // 1 — FL_ONGROUND
        // второй пули DT нет у ножа, гранат и оружия с перезарядкой > 0.4 с (скаут, AWP) — там DT дефенсивный.
        // Раньше это ставил рейдж, а с ножом он до этого места не доходил: режим оставался от прошлого оружия
        if (const auto weapon = csgo::Entity::from(retSpoofGadgets->client, localPlayer.get().getActiveWeapon()); weapon.getPOD()) {
            const auto data = weapon.getWeaponData();
            const auto type = weapon.getWeaponType();
            hvh::tickbase::setSlowWeapon(!data || type == WeaponType::Knife || type == WeaponType::Grenade || type == WeaponType::C4 || data->cycletime > 0.4f);
        }
    }
    hvh::movement::exploitTuning(globalContext->getOtherInterfaces().getCvar());
    hvh::movement::autoHideShots(cmd);
    hvh::movement::autoBuy(globalContext->getEngineInterfaces().getEngine());
    hvh::movement::exploits(globalContext->getEngineInterfaces(), clientInterfaces, *globalContext->memory);
    hvh::defensive::update(cmd, globalContext->getEngineInterfaces(), clientInterfaces, *globalContext->memory);

    bool ragebotShot = false;
    if (hvh::config.rage.enabled) {
        // рейджбот заменяет легит-аим, триггер и бектрек
        ragebotShot = hvh::ragebot::run(cmd, globalContext->getEngineInterfaces(), clientInterfaces, globalContext->getOtherInterfaces(), *globalContext->memory, features->backtrack.getLerp());
    } else {
        features->aimbot.run(features->misc, globalContext->getEngineInterfaces(), clientInterfaces, globalContext->getOtherInterfaces(), *globalContext->config, *globalContext->memory, cmd);
        Triggerbot::run(globalContext->getEngineInterfaces().engineTrace(), globalContext->getOtherInterfaces(), *globalContext->memory, *globalContext->config, cmd);
        features->backtrack.run(clientInterfaces, globalContext->getEngineInterfaces(), globalContext->getOtherInterfaces(), *globalContext->memory, cmd);
    }
    // ручной выстрел — только если оружие правда стреляет: зажатая ЛКМ на перезарядке возврат не запускает
    bool manualShot = false;
    if ((cmd->buttons & csgo::UserCmd::IN_ATTACK) && localPlayer && localPlayer.get().isAlive()) {
        const auto weapon = csgo::Entity::from(retSpoofGadgets->client, localPlayer.get().getActiveWeapon());
        const float serverTime = globalContext->memory->globalVars->serverTime();
        manualShot = weapon.getPOD() && weapon.clip() > 0 && weapon.nextPrimaryAttack() <= serverTime && localPlayer.get().nextAttack() <= serverTime;
    }
    hvh::movement::aiPeek(cmd, currentViewAngles.x, currentViewAngles.y, hvh::config.rage.enabled && hvh::ragebot::currentTarget() != 0,
        globalContext->getEngineInterfaces(), clientInterfaces, globalContext->getOtherInterfaces(), *globalContext->memory);
    hvh::movement::autoPeek(cmd, currentViewAngles.y, ragebotShot || manualShot, globalContext->memory->globalVars->intervalPerTick);
    hvh::antiaim::run(cmd, globalContext->getEngineInterfaces(), clientInterfaces, *globalContext->memory, ragebotShot);
    hvh::grenade::update(globalContext->getEngineInterfaces(), *globalContext->memory);
    hvh::visuals::onCreateMove(cmd);
    hvh::visuals::collectGrenades(clientInterfaces, *globalContext->memory);
    hvh::scoreboard::update(*globalContext->memory, globalContext->getOtherInterfaces(), clientInterfaces, globalContext->getEngineInterfaces(),
        cmd->buttons & csgo::UserCmd::IN_SCORE, globalContext->memory->globalVars->realtime);
    hvh::extras::update(globalContext->getOtherInterfaces(), globalContext->memory->globalVars->intervalPerTick);
    features->misc.edgejump(cmd);
    features->misc.moonwalk(cmd);
    features->misc.fastPlant(cmd);

    auto viewAnglesDelta{ cmd->viewangles - previousViewAngles };
    viewAnglesDelta.normalize();
    viewAnglesDelta.x = std::clamp(viewAnglesDelta.x, -features->misc.maxAngleDelta(), features->misc.maxAngleDelta());
    viewAnglesDelta.y = std::clamp(viewAnglesDelta.y, -features->misc.maxAngleDelta(), features->misc.maxAngleDelta());

    cmd->viewangles = previousViewAngles + viewAnglesDelta;

    cmd->viewangles.normalize();
    // с рейджем/антиаимом угол команды почти всегда не совпадает с камерой — движение чиним всегда
    if (hvh::config.rage.enabled || hvh::config.antiAim.enabled) {
        hvh::movement::fixMovement(cmd, currentViewAngles.y);
        hvh::antiaim::fixRollMovement(cmd);
    }
    else
        features->misc.fixMovement(cmd, currentViewAngles.y);
    hvh::movement::fastLadder(cmd, currentViewAngles.x, currentViewAngles.y);

    cmd->viewangles.x = std::clamp(cmd->viewangles.x, -89.0f, 89.0f);
    cmd->viewangles.y = std::clamp(cmd->viewangles.y, -180.0f, 180.0f);
    // roll от антиаима (наклон головы), иначе 0
    cmd->viewangles.z = hvh::config.antiAim.enabled ? hvh::antiaim::rollAngle() : 0.0f;
    cmd->forwardmove = std::clamp(cmd->forwardmove, -450.0f, 450.0f);
    cmd->sidemove = std::clamp(cmd->sidemove, -450.0f, 450.0f);

    previousViewAngles = cmd->viewangles;
    if (hvh::tickbase::sendPacket())
        hvh::antiaim::storeSentAngles(cmd->viewangles.x, cmd->viewangles.y);

    cmd->viewanglesBackup = cmd->viewangles;
    cmd->buttonsBackup = cmd->buttons;

    return false;
}

void FASTCALL_CONV ClientModeHooks::doPostScreenEffects(FASTCALL_THIS(csgo::ClientMode* thisptr), void* param) noexcept
{
    auto& features = globalContext->features;
    if (globalContext->getEngineInterfaces().getEngine().isInGame()) {
        features->visuals.thirdperson();
        features->visuals.inverseRagdollGravity();
        features->visuals.reduceFlashEffect();
        features->visuals.updateBrightness();
        features->visuals.remove3dSky();
        features->glow.render(globalContext->getEngineInterfaces(), ClientInterfaces{ retSpoofGadgets->client, *globalContext->clientInterfaces }, *globalContext->memory);
    }
    globalContext->hooks->clientModeHooks.getOriginalDoPostScreenEffects()(thisptr, param);
}

float FASTCALL_CONV ClientModeHooks::getViewModelFov(FASTCALL_THIS(csgo::ClientMode* thisptr)) noexcept
{
    float additionalFov = globalContext->features->visuals.viewModelFov();
    if (localPlayer) {
        if (const auto activeWeapon = csgo::Entity::from(retSpoofGadgets->client, localPlayer.get().getActiveWeapon()); activeWeapon.getPOD() != nullptr && activeWeapon.getNetworkable().getClientClass()->classId == ClassId::Tablet)
            additionalFov = 0.0f;
    }

    return globalContext->hooks->clientModeHooks.getOriginalGetViewModelFov()(thisptr) + additionalFov;
}

// Модель чужого игрока (и то, что к нему прикреплено: оружие в руках) рисуем в сглаженной позиции (Smoothing.h):
// сдвигаем копию костей, кэш костей игрока не трогаем — рейдж и лагкомп видят настоящую модель.
// сдвиг копии костей: кэш движка не трогаем — рейдж, лагкомп и трассы видят настоящую модель
static csgo::matrix3x4* shiftBones(const csgo::ModelRenderInfo& info, const csgo::matrix3x4* bones, const csgo::Vector& shift) noexcept
{
    const auto studio = globalContext->getEngineInterfaces().getModelInfo().getStudioModel(info.model);
    if (!studio)
        return const_cast<csgo::matrix3x4*>(bones);
    const int count = std::clamp(studio->numBones, 0, MAXSTUDIOBONES);
    static csgo::matrix3x4 shifted[MAXSTUDIOBONES];
    for (int i = 0; i < count; ++i) {
        shifted[i] = bones[i];
        shifted[i].translate(shift);
    }
    return shifted;
}

csgo::matrix3x4* smoothedBones(const csgo::ModelRenderInfo& info, csgo::matrix3x4* bones) noexcept
{
    if (!bones || !localPlayer || !info.model)
        return bones;

    if (info.entityIndex <= 0)
        return bones;
    const ClientInterfaces clientInterfaces{ retSpoofGadgets->client, *globalContext->clientInterfaces };
    auto entity = csgo::Entity::from(retSpoofGadgets->client, clientInterfaces.getEntityList().getEntity(info.entityIndex));
    // Оружие в руках — отдельная сущность, и её кости надо двигать вместе с владельцем: иначе тело
    // уезжает на сглаженную позицию, а ствол остаётся на месте и висит в полуметре сбоку от руки.
    // m_hOwnerEntity объявлен в Entity.h (NETVAR2) и у оружия в руках указывает на игрока.
    static const auto ownerOffset = Netvars::get(fnv::hash("CBaseEntity->m_hOwnerEntity"));
    for (int depth = 0; depth < 2 && ownerOffset && entity.getPOD() != nullptr && !entity.isPlayer(); ++depth)
        entity = csgo::Entity::from(retSpoofGadgets->client, clientInterfaces.getEntityList().getEntityFromHandle(entity.ownerEntity()));
    // только чужие игроки: у союзников-хвхшников модель скачет так же, как у врагов (Smoothing.h).
    // Свою модель и камеру не сглаживаем вовсе — сдвиг у себя давал телепорты и расхождение с руками
    if (entity.getPOD() == nullptr || !entity.isPlayer() || entity.getNetworkable().isDormant() || !entity.isAlive())
        return bones;
    if (entity.getPOD() == localPlayer.get().getPOD())
        return bones;

    const auto raw = entity.getAbsOrigin();
    const auto shift = hvh::smoothing::origin(entity.getNetworkable().index(), raw, entity.velocity(), globalContext->memory->globalVars->realtime) - raw;
    if (shift.squareLength() < 0.01f)
        return bones;
    return shiftBones(info, bones, shift);
}

// «Модель сервера» (3-е лицо): кости своей модели, повёрнутые в позу, которую видит сервер по
// последнему отправленному пакету. Ноги/таз — на угол ног сервера (взгляд + увод десинком, не больше
// макс. десинка на этой скорости), выше по телу поворот плавно переходит к отправленному взгляду
// (так body_yaw закручивает корпус), голова — по взгляду. Затем вся модель наклоняется на roll
// вокруг направления взгляда. nullptr — рисовать нечего (поза совпадает с нашей или выключено).
csgo::matrix3x4* serverModelBones(const csgo::ModelRenderInfo& info, const csgo::matrix3x4* bones) noexcept
{
    float pitch, sentYaw, desync, roll;
    if (!bones || !info.model || !hvh::antiaim::sentServerPose(pitch, sentYaw, desync, roll))
        return nullptr;
    const auto& local = localPlayer.get();
    float maxDesync = local.getMaxDesyncAngle();
    if (maxDesync <= 1.0f)
        maxDesync = 58.0f;
    const float serverFeet = sentYaw + std::clamp(desync, -maxDesync, maxDesync);
    // модель повёрнута по углу ног клиентской анимации, голова клиента смотрит по renderAngles
    const auto* animstate = local.getAnimstate();
    const float clientFeet = animstate ? animstate->footYaw : info.angles.y;
    const float feetDelta = std::remainder(serverFeet - clientFeet, 360.0f);
    const float eyeDelta = std::remainder(sentYaw - local.renderAngles().y, 360.0f);
    if (std::abs(feetDelta) < 1.0f && std::abs(eyeDelta) < 1.0f && std::abs(roll) < 1.0f)
        return nullptr;

    const auto studio = globalContext->getEngineInterfaces().getModelInfo().getStudioModel(info.model);
    if (!studio)
        return nullptr;
    const int count = std::clamp(studio->numBones, 0, MAXSTUDIOBONES);
    if (count == 0)
        return nullptr;

    const auto pivot = info.origin;
    float top = pivot.z + 1.0f;
    for (int i = 0; i < count; ++i)
        top = std::max(top, bones[i][2][3]);
    const float height = top - pivot.z;
    // таз примерно на 55% роста (стоя ~38 из ~70, в приседе пропорция та же)
    const float pelvis = pivot.z + height * 0.55f;

    static csgo::matrix3x4 ghost[MAXSTUDIOBONES];
    const float rollRad = Helpers::deg2rad(roll);
    const float lookYaw = Helpers::deg2rad(sentYaw);
    const csgo::Vector forward{ std::cos(lookYaw), std::sin(lookYaw), 0.0f };
    for (int i = 0; i < count; ++i) {
        ghost[i] = bones[i];
        const float w = std::clamp((bones[i][2][3] - pelvis) / std::max(top - pelvis, 1.0f), 0.0f, 1.0f);
        const float angle = Helpers::deg2rad(feetDelta + (eyeDelta - feetDelta) * w * w * (3.0f - 2.0f * w));
        ghost[i].rotateYaw(pivot, std::cos(angle), std::sin(angle));
        if (rollRad != 0.0f)
            ghost[i].rotateAxis(pivot, forward, std::cos(rollRad), std::sin(rollRad));
    }
    return ghost;
}

// Призрак рисуется до самой модели: он полупрозрачный, а модель сверху перекроет его там, где они совпадают
inline void drawServerModel(void* ctx, void* state, const csgo::ModelRenderInfo& info, csgo::matrix3x4* bones) noexcept
{
    if (!hvh::config.view.serverModel || !hvh::config.antiAim.enabled || !localPlayer || !localPlayer.get().isAlive()
        || info.entityIndex != localPlayer.get().getNetworkable().index()
        || !globalContext->memory->input || !globalContext->memory->input->isCameraInThirdPerson
        || globalContext->getOtherInterfaces().getStudioRender().isForcedMaterialOverride())
        return;
    if (const auto ghost = serverModelBones(info, bones))
        globalContext->features->chams.drawGhost(ctx, state, info, ghost, hvh::config.view.serverModelColor);
}

void FASTCALL_CONV ModelRenderHooks::drawModelExecute(FASTCALL_THIS(csgo::ModelRenderPOD* thisptr), void* ctx, void* state, const csgo::ModelRenderInfo& info, csgo::matrix3x4* customBoneToWorld) noexcept
{
    customBoneToWorld = smoothedBones(info, customBoneToWorld);
    drawServerModel(ctx, state, info, customBoneToWorld);
    if (globalContext->getOtherInterfaces().getStudioRender().isForcedMaterialOverride())
        return globalContext->hooks->modelRenderHooks.getOriginalDrawModelExecute()(thisptr, ctx, state, &info, customBoneToWorld);

    auto& features = globalContext->features;
    if (features->visuals.removeHands(info.model->name) || features->visuals.removeSleeves(info.model->name) || features->visuals.removeWeapons(info.model->name))
        return;

    if (!features->chams.render(features->backtrack, ctx, state, info, customBoneToWorld))
        globalContext->hooks->modelRenderHooks.getOriginalDrawModelExecute()(thisptr, ctx, state, &info, customBoneToWorld);

    globalContext->getOtherInterfaces().getStudioRender().forcedMaterialOverride(nullptr);
}

int FASTCALL_CONV SvCheatsHooks::getInt(csgo::ConVarPOD* thisptr) noexcept
{
    const auto original = globalContext->hooks->svCheatsHooks.getOriginalSvCheatsGetInt()(thisptr);
    if (static const bool debug = std::getenv("NEWERWIN_DEBUG"); debug) {
        static int calls = 0;
        if (++calls % 500 == 1)
            std::fprintf(stderr, "[newerwin] sv_cheats getInt from %p (calls=%d)\n", __builtin_return_address(0), calls);
    }
    if (globalContext->features->visuals.svCheatsGetBoolHook(RETURN_ADDRESS()))
        return 1;
    return original;
}

void FASTCALL_CONV ClientHooks::frameStageNotify(FASTCALL_THIS(csgo::ClientPOD* thisptr), csgo::FrameStage stage) noexcept
{
    auto& features = globalContext->features;

    // Фейкдак: на отправленном тике присед полный (его видят враги), но своя анимация видит, как
    // m_flDuckAmount каждые 14 тиков скачет 0.1↔1, тянется к нему плавно и застревает на полуприседе.
    // На отрисовку кадра даём анимации полный присед, в RENDER_END возвращаем — предсказание этого не видит.
    {
        static csgo::EntityPOD* duckedPlayer = nullptr;
        static float savedDuck = 0.0f;
        static const auto duckOffset = Netvars::get(fnv::hash("CBasePlayer->m_flDuckAmount"));
        if (stage == csgo::FrameStage::RENDER_START && duckOffset && !duckedPlayer && localPlayer && hvh::movement::isFakeDucking()) {
            duckedPlayer = localPlayer.get().getPOD();
            auto& duck = *reinterpret_cast<float*>(std::uintptr_t(duckedPlayer) + duckOffset);
            savedDuck = duck;
            duck = 1.0f;
        } else if (stage == csgo::FrameStage::RENDER_END && duckedPlayer) {
            if (localPlayer && localPlayer.get().getPOD() == duckedPlayer)
                *reinterpret_cast<float*>(std::uintptr_t(duckedPlayer) + duckOffset) = savedDuck;
            duckedPlayer = nullptr;
        }
    }
    if (globalContext->getEngineInterfaces().getEngine().isConnected() && !globalContext->getEngineInterfaces().getEngine().isInGame())
        features->misc.changeName(true, nullptr, 0.0f);

    // снимок для ESP — после отрисовки кадра: позиции и матрица камеры совпадают с тем, что на экране
    // (в START они от прошлого кадра, и при движении ESP дрожит на кадр позади)
    if (stage == csgo::FrameStage::RENDER_END) {
        GameData::update(ClientInterfaces{ retSpoofGadgets->client, *globalContext->clientInterfaces }, globalContext->getEngineInterfaces(), globalContext->getOtherInterfaces(), *globalContext->memory);
    }

    // Сетевые слои анимации живы только здесь: на RENDER_START их уже перезатёрла клиентская анимация
    if (stage == csgo::FrameStage::NET_UPDATE_POSTDATAUPDATE_START) {
        hvh::ragebot::captureNetworkAngles(globalContext->getEngineInterfaces(),
            ClientInterfaces{ retSpoofGadgets->client, *globalContext->clientInterfaces });
        hvh::resolver::onNetUpdate(globalContext->getEngineInterfaces(),
            ClientInterfaces{ retSpoofGadgets->client, *globalContext->clientInterfaces }, *globalContext->memory);
    }

    if (stage == csgo::FrameStage::RENDER_END) {
        hvh::resolver::afterClientAnimation(globalContext->getEngineInterfaces(),
            ClientInterfaces{ retSpoofGadgets->client, *globalContext->clientInterfaces });
    }

    if (stage == csgo::FrameStage::RENDER_START) {
        hvh::movement::keepZoomSensitivity(globalContext->getEngineInterfaces().getEngine(), globalContext->getOtherInterfaces().getCvar());
        hvh::movement::syncRendering(globalContext->getOtherInterfaces().getCvar());
        if (float pitch, yaw; hvh::config.antiAim.enabled && hvh::config.view.showFakeAngles && localPlayer && localPlayer.get().isAlive()
            && globalContext->memory->input->isCameraInThirdPerson && Netvars::get(fnv::hash("CBasePlayer->deadflag")) != 0 && hvh::antiaim::sentAngles(pitch, yaw))
            localPlayer.get().renderAngles() = csgo::Vector{ pitch, yaw, 0.0f };
        features->misc.preserveKillfeed();
        features->misc.disablePanoramablur();
        features->visuals.colorWorld();
        features->misc.updateEventListeners();
        features->visuals.updateEventListeners();
        hvh::antiaim::updateEventListener(globalContext->getEngineInterfaces(), *globalContext->memory);
        hvh::ragebot::updateEventListener(globalContext->getEngineInterfaces(), *globalContext->memory);
    }
    if (globalContext->getEngineInterfaces().getEngine().isInGame()) {
        features->visuals.skybox(stage);
        features->visuals.removeBlur(stage);
        features->misc.oppositeHandKnife(stage);
        features->visuals.removeGrass(stage);
        features->visuals.modifySmoke(stage);
        features->visuals.disablePostProcessing(stage);
        features->visuals.removeVisualRecoil(stage);
        features->visuals.applyZoom(stage);
        features->misc.fixAnimationLOD(stage);
        features->backtrack.update(globalContext->getEngineInterfaces(), ClientInterfaces{ retSpoofGadgets->client, *globalContext->clientInterfaces }, globalContext->getOtherInterfaces(), *globalContext->memory, stage);
    }
    features->inventoryChanger.run(*globalContext->memory, stage);

    globalContext->hooks->clientHooks.getOriginalFrameStageNotify()(thisptr, stage);
    // своя анимация посчитана в оригинале RENDER_START — поверх неё аним брейкер
    if (stage == csgo::FrameStage::RENDER_START)
        hvh::movement::animBreaker(globalContext->memory->globalVars->realtime);
}

int FASTCALL_CONV EngineSoundHooks::emitSound(FASTCALL_THIS(csgo::EngineSoundPOD* thisptr), void* filter, int entityIndex, int channel, const char* soundEntry, unsigned int soundEntryHash, const char* sample, float volume, int seed, int soundLevel, int flags, int pitch, const csgo::Vector& origin, const csgo::Vector& direction, void* utlVecOrigins, bool updatePositions, float soundtime, int speakerentity, void* soundParams) noexcept
{
    globalContext->features->sound.modulateSound(soundEntry, entityIndex, volume);
    globalContext->features->misc.autoAccept(soundEntry);

    volume = std::clamp(volume, 0.0f, 1.0f);
    return globalContext->hooks->engineSoundHooks.getOriginalEmitSound()(thisptr, filter, entityIndex, channel, soundEntry, soundEntryHash, sample, volume, seed, soundLevel, flags, pitch, &origin, &direction, utlVecOrigins, updatePositions, soundtime, speakerentity, soundParams);
}

bool FASTCALL_CONV ClientModeHooks::shouldDrawFog(FASTCALL_THIS(csgo::ClientMode* thisptr)) noexcept
{
#if IS_WIN32()
    if constexpr (std::is_same_v<HookType, MinHook>) {
        if (RETURN_ADDRESS() != globalContext->memory->shouldDrawFogReturnAddress)
            return globalContext->hooks->clientModeHooks.getOriginalShouldDrawFog()(thisptr);
    }
#endif

    return !globalContext->features->visuals.shouldRemoveFog();
}

bool FASTCALL_CONV ClientModeHooks::shouldDrawViewModel(FASTCALL_THIS(csgo::ClientMode* thisptr)) noexcept
{
    if (globalContext->features->visuals.isZoomOn() && localPlayer && localPlayer.get().fov() < 45 && localPlayer.get().fovStart() < 45)
        return false;
    return globalContext->hooks->clientModeHooks.getOriginalShouldDrawViewModel()(thisptr);
}

#if IS_WIN32()
void FASTCALL_CONV SurfaceHooks::lockCursor(FASTCALL_THIS(csgo::SurfacePOD* thisptr)) noexcept
{
    if (gui->isOpen())
        return globalContext->getOtherInterfaces().getSurface().unlockCursor();
    return globalContext->hooks->surfaceHooks.getOriginalLockCursor()(thisptr);
}
#endif

void FASTCALL_CONV SurfaceHooks::setDrawColor(FASTCALL_THIS(csgo::SurfacePOD* thisptr), int r, int g, int b, int a) noexcept
{
    globalContext->features->visuals.setDrawColorHook(RETURN_ADDRESS(), a);
    globalContext->hooks->surfaceHooks.getOriginalSetDrawColor()(thisptr, r, g, b, a);
}

// 3-е лицо: штатная коллизия камеры (CAM_Think) упирается в игроков — подошёл тиммейт, и камера влетает
// в модель. Ставим камеру сами: хулл ±6 ед. от глаз назад, только по стенам (MASK_SOLID_BRUSHONLY).
inline void placeThirdPersonCamera(const csgo::Vector& eye, csgo::ViewSetup* setup) noexcept
{
    const float distance = globalContext->features->visuals.thirdpersonDistance();
    const float pitch = Helpers::deg2rad(setup->angles.x);
    const float yaw = Helpers::deg2rad(setup->angles.y);
    const csgo::Vector forward{ std::cos(pitch) * std::cos(yaw), std::cos(pitch) * std::sin(yaw), -std::sin(pitch) };
    csgo::Ray ray{ eye, eye - forward * distance };
    // под csgo::Ray лежит Ray_t: m_StartOffset +32 (нули — хулл симметричный), m_Extents +48, m_IsRay +72
    static_assert(offsetof(csgo::Ray, isRay) == WIN32_LINUX(68, 72));
    const csgo::Vector extents{ 6.0f, 6.0f, 6.0f };
    std::memcpy(reinterpret_cast<std::byte*>(&ray) + 48, &extents, sizeof(extents));
    ray.isRay = false;
    csgo::Trace trace;
    globalContext->getEngineInterfaces().engineTrace().traceRay(ray, 0x400B, { localPlayer.get().getPOD() }, trace);
    setup->origin = eye - forward * (distance * (trace.startSolid ? 1.0f : trace.fraction));
}

void FASTCALL_CONV ClientModeHooks::overrideView(FASTCALL_THIS(csgo::ClientMode* thisptr), csgo::ViewSetup* setup) noexcept
{
    if (localPlayer && !localPlayer.get().isScoped())
        setup->fov += globalContext->features->visuals.fov();
    else if (localPlayer && localPlayer.get().isScoped() && hvh::config.view.removeScopeZoom) {
        setup->fov = 90.0f + globalContext->features->visuals.fov();
        // второй уровень: увеличение по тангенсу половины угла, «×2» — вдвое крупнее
        if (const float zoom = hvh::movement::scopeZoomFactor(); zoom > 1.0f) {
            const float half = std::clamp(setup->fov, 1.0f, 179.0f) * 0.5f * 3.14159265f / 180.0f;
            setup->fov = 2.0f * std::atan(std::tan(half) / zoom) * 180.0f / 3.14159265f;
        }
    }
    setup->farZ += globalContext->features->visuals.farZ() * 10;
    // 3-е лицо без сглаживания: CAM_Think тянет углы камеры к взгляду с задержкой, ставим их сразу
    if (auto* input = globalContext->memory->input; input && input->isCameraInThirdPerson && localPlayer && localPlayer.get().isAlive()) {
        const auto view = globalContext->getEngineInterfaces().getEngine().getViewAngles();
        input->cameraOffset.x = view.x;
        input->cameraOffset.y = view.y;
    }
    const bool thirdPerson = globalContext->memory->input && globalContext->memory->input->isCameraInThirdPerson && localPlayer && localPlayer.get().isAlive();
    const auto eye = setup->origin;
    globalContext->hooks->clientModeHooks.getOriginalOverrideView()(thisptr, setup);
    if (thirdPerson)
        placeThirdPersonCamera(eye, setup);
    // фейкдак: камера на высоте стоя, хотя модель приседает
    // Высота считается от интерполированных ног, а не от viewOffset последней команды: тот каждые 14 тиков
    // скачет 46↔64, а глаз камеры интерполирован — разница дёргала камеру вверх-вниз.
    if (hvh::movement::isFakeDucking() && localPlayer)
        setup->origin.z += localPlayer.get().getAbsOrigin().z + 64.06f - eye.z;
}

int FASTCALL_CONV BspQueryHooks::listLeavesInBox(FASTCALL_THIS(void* thisptr), const csgo::Vector& mins, const csgo::Vector& maxs, unsigned short* list, int listMax) noexcept
{
    if (const auto newVectors = globalContext->features->misc.listLeavesInBoxHook(RETURN_ADDRESS(), FRAME_ADDRESS()))
        return globalContext->hooks->bspQueryHooks.getOriginalListLeavesInBox()(thisptr, &newVectors->first, &newVectors->second, list, listMax);
    return globalContext->hooks->bspQueryHooks.getOriginalListLeavesInBox()(thisptr, &mins, &maxs, list, listMax);
}

void FASTCALL_CONV ViewRenderHooks::render2dEffectsPreHud(FASTCALL_THIS(csgo::ViewRender* thisptr), void* viewSetup) noexcept
{
    globalContext->features->visuals.applyScreenEffects();
    globalContext->features->visuals.hitEffect();
    globalContext->hooks->viewRenderHooks.getOriginalRender2dEffectsPreHud()(thisptr, viewSetup);
}

const csgo::DemoPlaybackParameters* FASTCALL_CONV EngineHooks::getDemoPlaybackParameters(FASTCALL_THIS(csgo::EnginePOD* thisptr)) noexcept
{
    const auto params = globalContext->hooks->engineHooks.getOriginalGetDemoPlaybackParameters()(thisptr);

    if (params)
        return globalContext->features->misc.getDemoPlaybackParametersHook(RETURN_ADDRESS(), *params);

    return params;
}

bool FASTCALL_CONV EngineHooks::isPlayingDemo(FASTCALL_THIS(csgo::EnginePOD* thisptr)) noexcept
{
    const auto result = globalContext->hooks->engineHooks.getOriginalIsPlayingDemo()(thisptr);

    if (globalContext->features->misc.isPlayingDemoHook(RETURN_ADDRESS(), FRAME_ADDRESS()))
        return true;

    return result;
}

void FASTCALL_CONV ClientModeHooks::updateColorCorrectionWeights(FASTCALL_THIS(csgo::ClientMode* thisptr)) noexcept
{
    globalContext->hooks->clientModeHooks.getOriginalUpdateColorCorrectionWeights()(thisptr);
    globalContext->features->visuals.updateColorCorrectionWeightsHook();
}

float FASTCALL_CONV EngineHooks::getScreenAspectRatio(FASTCALL_THIS(csgo::EnginePOD* thisptr), int width, int height) noexcept
{
    if (globalContext->features->misc.aspectRatio() != 0.0f)
        return globalContext->features->misc.aspectRatio();
    return globalContext->hooks->engineHooks.getOriginalGetScreenAspectRatio()(thisptr, width, height);
}

void FASTCALL_CONV ViewRenderHooks::renderSmokeOverlay(FASTCALL_THIS(csgo::ViewRender* thisptr), bool preViewModel) noexcept
{
    if (!globalContext->features->visuals.renderSmokeOverlayHook())
        globalContext->hooks->viewRenderHooks.getOriginalRenderSmokeOverlay()(thisptr, preViewModel);
}

double FASTCALL_CONV PanoramaMarshallHelperHooks::getArgAsNumber(FASTCALL_THIS(csgo::PanoramaMarshallHelperPOD* thisptr), void* params, int index) noexcept
{
    const auto result = globalContext->hooks->panoramaMarshallHelperHooks.getOriginalGetArgAsNumber()(thisptr, params, index);
    globalContext->features->inventoryChanger.getArgAsNumberHook(static_cast<int>(result), RETURN_ADDRESS());
    return result;
}

const char* FASTCALL_CONV PanoramaMarshallHelperHooks::getArgAsString(FASTCALL_THIS(csgo::PanoramaMarshallHelperPOD* thisptr), void* params, int index) noexcept
{
    const auto result = globalContext->hooks->panoramaMarshallHelperHooks.getOriginalGetArgAsString()(thisptr, params, index);

    if (result)
        globalContext->features->inventoryChanger.getArgAsStringHook(globalContext->hooks->panoramaMarshallHelperHooks, *globalContext->memory, result, RETURN_ADDRESS(), params);

    return result;
}

void FASTCALL_CONV PanoramaMarshallHelperHooks::setResultInt(FASTCALL_THIS(csgo::PanoramaMarshallHelperPOD* thisptr), void* params, int result) noexcept
{
    result = globalContext->features->inventoryChanger.setResultIntHook(RETURN_ADDRESS(), params, result);
    globalContext->hooks->panoramaMarshallHelperHooks.getOriginalSetResultInt()(thisptr, params, result);
}

unsigned FASTCALL_CONV PanoramaMarshallHelperHooks::getNumArgs(FASTCALL_THIS(csgo::PanoramaMarshallHelperPOD* thisptr), void* params) noexcept
{
    const auto result = globalContext->hooks->panoramaMarshallHelperHooks.getOriginalGetNumArgs()(globalContext->memory->panoramaMarshallHelper, params);
    globalContext->features->inventoryChanger.getNumArgsHook(globalContext->hooks->panoramaMarshallHelperHooks, thisptr, result, RETURN_ADDRESS(), params);
    return result;
}

void FASTCALL_CONV InventoryManagerHooks::updateInventoryEquippedState(FASTCALL_THIS(csgo::InventoryManagerPOD* thisptr), std::uintptr_t inventory, csgo::ItemId itemID, csgo::Team team, int slot, bool swap) noexcept
{
    globalContext->features->inventoryChanger.onItemEquip(team, slot, itemID);
    globalContext->hooks->inventoryManagerHooks.getOriginalUpdateInventoryEquippedState()(thisptr, inventory, itemID, team, slot, swap);
}

void FASTCALL_CONV CSPlayerInventoryHooks::soUpdated(FASTCALL_THIS(csgo::CSPlayerInventoryPOD* thisptr), csgo::SOID owner, csgo::SharedObjectPOD* object, int event) noexcept
{
    globalContext->features->inventoryChanger.onSoUpdated(csgo::SharedObject::from(retSpoofGadgets->client, object));
    globalContext->hooks->playerInventoryHooks.getOriginalSoUpdated()(thisptr, owner, object, event);
}

bool FASTCALL_CONV ClientHooks::dispatchUserMessage(FASTCALL_THIS(csgo::ClientPOD* thisptr), csgo::UserMessageType type, int passthroughFlags, int size, const void* data) noexcept
{
    globalContext->features->misc.dispatchUserMessageHook(type, size, data);
    if (static_cast<int>(type) == hvh::radar::spottedEntityUpdate)
        hvh::radar::onUserMessage(data, size, globalContext->memory->globalVars->realtime);
    if (type == csgo::UserMessageType::Text)
        globalContext->features->inventoryChanger.onUserTextMsg(*globalContext->memory, data, size);

    return globalContext->hooks->clientHooks.getOriginalDispatchUserMessage()(thisptr, type, passthroughFlags, size, data);
}

void FASTCALL_CONV ClientStateHooks::packetEnd(FASTCALL_THIS(csgo::ClientState* thisptr)) noexcept
{
    const auto soundMessages = globalContext->memory->soundMessages;
    if (soundMessages->numElements > 0) {
        for (int i = 0; i <= soundMessages->lastAlloc; ++i) {
            if (!soundMessages->isIndexUsed(i))
                continue;

            auto& soundInfo = soundMessages->memory[i].element;
            if (const char* soundName = globalContext->getOtherInterfaces().getSoundEmitter().getSoundName(soundInfo.soundIndex)) {
                globalContext->features->sound.modulateSound(soundName, soundInfo.entityIndex, soundInfo.volume);
                soundInfo.volume = std::clamp(soundInfo.volume, 0.0f, 1.0f);
            }
        }
    }

    FunctionInvoker{ retSpoofGadgets->engine, globalContext->hooks->clientStateHooks.originalPacketEnd }(thisptr);
}

#if IS_WIN32()

void* FASTCALL_CONV KeyValuesSystemHooks::allocKeyValuesMemory(FASTCALL_THIS(csgo::KeyValuesSystemPOD* thisptr), int size) noexcept
{
    if (const auto returnAddress = RETURN_ADDRESS(); returnAddress == globalContext->memory->keyValuesAllocEngine || returnAddress == globalContext->memory->keyValuesAllocClient)
        return nullptr;
    return globalContext->hooks->keyValuesSystemHooks.getOriginalAllocKeyValuesMemory()(thisptr, size);
}

extern "C" BOOL WINAPI _CRT_INIT(HMODULE moduleHandle, DWORD reason, LPVOID reserved);

DWORD WINAPI unload(LPVOID) noexcept
{
    globalContext->windowProcedureHook->uninstall();

    Sleep(100);

    globalContext->getOtherInterfaces().getInputSystem().enableInput(true);
    globalContext->gameEventListener->remove();

    ImGui_ImplDX9_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    const auto moduleHandle = globalContext->moduleHandle;
    _CRT_INIT(moduleHandle, DLL_PROCESS_DETACH, nullptr);

    FreeLibraryAndExitThread(moduleHandle, 0);
}

#endif

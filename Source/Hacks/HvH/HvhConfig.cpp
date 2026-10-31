#include <algorithm>
#include <charconv>
#include <string>

#include "HvhConfig.h"

#include <ConfigStructs.h>
#include <JsonForward.h>

namespace hvh
{

const RageWeapon& weaponSettings(WeaponGroup group) noexcept
{
    const auto& weapon = config.rage.weapons[static_cast<int>(group)];
    return weapon.override ? weapon : config.rage.weapons[static_cast<int>(WeaponGroup::General)];
}

static void writeKey(json& j, const char* name, const KeyBind& key)
{
    if (key.isSet())
        j[name] = key.toString();
}

static void writeStates(json& j, const std::array<AntiAimState, static_cast<int>(AaState::Count)>& states)
{
    for (std::size_t i = 0; i < states.size(); ++i) {
        const auto& st = states[i];
        auto& js = j[i];
        js["Override"] = st.override;
        js["Yaw offset"] = st.yawOffset;
        js["Add left"] = st.addLeft;
        js["Add right"] = st.addRight;
        js["Modifier"] = st.modifier;
        js["Degree"] = st.degree;
        js["Ways"] = st.ways;
        js["Spin speed"] = st.spinSpeed;
        js["Randomize"] = st.randomize;
        js["Desync mode"] = st.desyncMode;
        js["Desync left"] = st.desyncLeft;
        js["Desync right"] = st.desyncRight;
        js["Desync invert"] = st.desyncInvert;
        js["Delay mode"] = st.delayMode;
        js["Delay ticks"] = st.delayTicks;
    }
}

static void readStates(const json& j, std::array<AntiAimState, static_cast<int>(AaState::Count)>& states)
{
    if (!j.is_array())
        return;
    for (std::size_t i = 0; i < std::min(j.size(), states.size()); ++i) {
        const auto& js = j[i];
        auto& st = states[i];
        read(js, "Override", st.override);
        read(js, "Yaw offset", st.yawOffset);
        read(js, "Add left", st.addLeft);
        read(js, "Add right", st.addRight);
        read(js, "Modifier", st.modifier);
        read(js, "Degree", st.degree);
        read(js, "Ways", st.ways);
        read(js, "Spin speed", st.spinSpeed);
        read(js, "Randomize", st.randomize);
        read(js, "Desync mode", st.desyncMode);
        read(js, "Desync left", st.desyncLeft);
        read(js, "Desync right", st.desyncRight);
        read(js, "Desync invert", st.desyncInvert);
        read(js, "Delay mode", st.delayMode);
        read(js, "Delay ticks", st.delayTicks);
        st.modifier = std::clamp(st.modifier, 0, static_cast<int>(YawModifier::Count) - 1);
        st.desyncMode = std::clamp(st.desyncMode, 0, static_cast<int>(DesyncMode::Count) - 1);
        st.delayMode = std::clamp(st.delayMode, 0, static_cast<int>(SwitchDelay::Count) - 1);
        st.ways = std::clamp(st.ways, 3, 8);
        st.delayTicks = std::clamp(st.delayTicks, 1, 16);
    }
}

static void writeSnaps(json& j, const std::array<DefensiveSnap, static_cast<int>(SnapState::Count)>& snaps)
{
    for (std::size_t i = 0; i < snaps.size(); ++i) {
        const auto& sn = snaps[i];
        auto& js = j[i];
        js["Mode"] = sn.mode;
        js["Pitch"] = sn.pitch;
        js["Pitch angle"] = sn.pitchAngle;
        js["Pitch angle 2"] = sn.pitchAngle2;
        js["Pitch speed"] = sn.pitchSpeed;
        js["Yaw"] = sn.yaw;
        js["Yaw angle"] = sn.yawAngle;
        js["Yaw delay"] = sn.yawDelay;
        js["Yaw speed"] = sn.yawSpeed;
        js["Force LC"] = sn.forceLc;
        js["Duration"] = sn.duration;
    }
}

static void readSnaps(const json& j, std::array<DefensiveSnap, static_cast<int>(SnapState::Count)>& snaps)
{
    if (!j.is_array())
        return;
    for (std::size_t i = 0; i < std::min(j.size(), snaps.size()); ++i) {
        const auto& js = j[i];
        auto& sn = snaps[i];
        read(js, "Mode", sn.mode);
        read(js, "Pitch", sn.pitch);
        read(js, "Pitch angle", sn.pitchAngle);
        read(js, "Pitch angle 2", sn.pitchAngle2);
        read(js, "Pitch speed", sn.pitchSpeed);
        read(js, "Yaw", sn.yaw);
        read(js, "Yaw angle", sn.yawAngle);
        read(js, "Yaw delay", sn.yawDelay);
        read(js, "Yaw speed", sn.yawSpeed);
        read(js, "Force LC", sn.forceLc);
        read(js, "Duration", sn.duration);
        sn.mode = std::clamp(sn.mode, 0, 2);
        sn.pitch = std::clamp(sn.pitch, 0, static_cast<int>(SnapPitch::Count) - 1);
        sn.yaw = std::clamp(sn.yaw, 0, static_cast<int>(SnapYaw::Count) - 1);
        sn.duration = std::clamp(sn.duration, 1, 13);
        sn.yawDelay = std::clamp(sn.yawDelay, 0, 14);
    }
}

json toJson() noexcept
{
    json j;

    auto& r = j["Rage"];
    const auto& rage = config.rage;
    r["Enabled"] = rage.enabled;
    r["Silent"] = rage.silent;
    r["Auto fire"] = rage.autoFire;
    r["Autowall"] = rage.autowall;
    r["Force shot"] = rage.forceShot;
    r["Resolver"] = rage.resolver;
    r["Layer resolver"] = rage.layerResolver;
    r["Resolver safe when unsure"] = rage.resolverSafeWhenUnsure;
    r["Resolver steps"] = rage.resolverSteps;
    r["Safe point"] = rage.safePoint;
    r["Knife bot"] = rage.knifeBot;
    r["Knife bot mode"] = rage.knifeBotMode;
    r["Zeus bot"] = rage.zeusBot;
    r["Zeus hitchance"] = rage.zeusHitchance;
    r["FOV"] = rage.fov;
    writeKey(r, "Min damage override key", rage.minDamageOverrideKey);
    writeKey(r, "Force body key", rage.forceBodyKey);
    writeKey(r, "Force shot key", rage.forceShotKey);
    for (std::size_t i = 0; i < rage.weapons.size(); ++i) {
        const auto& w = rage.weapons[i];
        auto& jw = r["Weapons"][i];
        jw["Override"] = w.override;
        jw["Hitboxes"] = w.hitboxes;
        jw["Multipoint"] = w.multipoint;
        jw["Head scale"] = w.headScale;
        jw["Auto scale"] = w.autoScale;
        jw["Body scale"] = w.bodyScale;
        jw["Min damage"] = w.minDamage;
        jw["Min damage override"] = w.minDamageOverride;
        jw["Hitchance"] = w.hitchance;
        jw["Auto stop mode"] = w.autoStop;
        jw["Auto stop early"] = w.autoStopEarly;
        jw["Auto scope"] = w.autoScope;
        jw["Prefer body"] = w.preferBody;
        jw["Body if lethal"] = w.bodyIfLethal;
    }

    auto& a = j["Anti-aim"];
    const auto& aa = config.antiAim;
    a["Enabled"] = aa.enabled;
    a["Pitch"] = aa.pitch;
    a["Yaw base"] = aa.yawBase;
    writeStates(a["States"], aa.states);
    writeSnaps(a["Snaps"], aa.snaps);
    for (std::size_t i = 0; i < aa.presetRack.size(); ++i) {
        writeStates(a["Preset rack"][i]["States"], aa.presetRack[i].states);
        writeSnaps(a["Preset rack"][i]["Snaps"], aa.presetRack[i].snaps);
    }
    a["Defensive AA"] = aa.defensiveAa;
    a["LC triggers"] = aa.lcTriggers;
    a["LC trigger states"] = aa.lcTriggerStates;
    a["Anti bruteforce triggers"] = aa.antiBruteTriggers;
    a["Anti bruteforce mode"] = aa.antiBruteMode;
    a["Anti bruteforce power"] = aa.antiBrutePower;
    a["Anti bruteforce order"] = aa.antiBruteOrder;
    a["Freestand static"] = aa.freestandStatic;
    a["Edge yaw"] = aa.edgeYaw;
    writeKey(a, "Edge key", aa.edgeKey);
    a["Edge static"] = aa.edgeStatic;
    a["Roll"] = aa.roll;
    a["Safe head"] = aa.safeHead;
    a["Legit AA"] = aa.legitAa;
    a["Naturality"] = aa.naturality;
    a["Naturality time"] = aa.naturalityTime;
    writeKey(a, "Manual forward", aa.manualForward);
    a["Desync"] = aa.desync;
    a["Anti bruteforce reset"] = aa.antiBruteReset;
    a["Static on manual"] = aa.staticOnManual;
    a["Defensive"] = aa.defensive;
    a["Defensive fov"] = aa.defensiveFov;
    a["Defensive yaw"] = aa.defensiveYaw;
    a["Defensive pitch"] = aa.defensivePitch;
    a["Defensive jitter"] = aa.defensiveJitter;
    a["Flicks"] = aa.flicks;
    a["Flick trigger"] = aa.flickTrigger;
    a["Flick side"] = aa.flickSide;
    a["Flick angle"] = aa.flickAngle;
    a["Flick yaw"] = aa.flickYaw;
    a["Flick pitch override"] = aa.flickPitchOverride;
    a["Flick pitch value"] = aa.flickPitch;
    a["Flick every"] = aa.flickEvery;
    a["Anti bruteforce"] = aa.antiBrute;
    writeKey(a, "Inverter", aa.inverter);
    a["Freestanding"] = aa.freestanding;
    writeKey(a, "Freestand key", aa.freestandKey);
    writeKey(a, "Manual left", aa.manualLeft);
    writeKey(a, "Manual right", aa.manualRight);
    writeKey(a, "Manual back", aa.manualBack);
    a["Fake lag"] = aa.fakeLag;
    a["Slide walk"] = aa.slideWalk;

    j["View"]["Remove scope zoom"] = config.view.removeScopeZoom;
    j["View"]["Second zoom"] = config.view.secondZoom;
    j["View"]["Second zoom factor"] = config.view.secondZoomFactor;
    j["View"]["Show fake angles"] = config.view.showFakeAngles;
    j["View"]["Grenade prediction"] = config.view.grenadePrediction;
    j["View"]["Dormant time"] = config.view.dormantTime;
    j["View"]["Sync render"] = config.view.syncRender;
    j["View"]["Screen logs"] = config.view.screenLogs;
    j["View"]["Watermark"] = config.view.watermark;
    j["View"]["ESP flags"] = config.view.espFlags;
    j["View"]["Aim dot"] = config.view.aimDot;
    j["View"]["Tab loadout"] = config.view.tabLoadout;
    j["View"]["Damage numbers"] = config.view.damageNumbers;
    j["View"]["Spread circle"] = config.view.spreadCircle;
    j["View"]["Footsteps"] = config.view.footsteps;
    j["View"]["Kill flash"] = config.view.killFlash;
    j["View"]["Grenade timers"] = config.view.grenadeTimers;
    j["View"]["Shot capsules"] = config.view.shotCapsules;

    auto& m = j["Movement"];
    writeKey(m, "Auto peek key", config.movement.autoPeekKey);
    m["Auto peek mode"] = config.movement.autoPeekMode;
    writeKey(m, "Fake duck key", config.movement.fakeDuckKey);
    m["Fast ladder"] = config.movement.fastLadder;
    m["Bunny hop"] = config.movement.bunnyHop;
    m["Auto strafe mode"] = config.movement.autoStrafe;

    auto& b = j["Auto buy"];
    b["Enabled"] = config.autoBuy.enabled;
    b["Primary"] = config.autoBuy.primary;
    b["Secondary"] = config.autoBuy.secondary;
    b["Extras"] = config.autoBuy.extras;

    auto& e = j["Exploits"];
    const auto& ex = config.exploits;
    e["Double tap"] = ex.doubleTap;
    writeKey(e, "Double tap key", ex.doubleTapKey);
    e["Shift ticks"] = ex.shiftTicks;
    writeKey(e, "Teleport key", ex.teleportKey);
    e["Defensive"] = ex.defensive;

    auto& s = j["Skins"];
    const auto& skins = config.skins;
    s["Knife"] = skins.knife;
    s["Gloves"] = skins.gloves;
    s["Agent T"] = skins.agentT;
    s["Agent CT"] = skins.agentCT;
    for (const auto& [id, skin] : skins.weapons) {
        auto& w = s["Weapons"][std::to_string(id)];
        w["Paint kit"] = skin.paintKit;
        w["Wear"] = skin.wear;
        w["Seed"] = skin.seed;
        w["StatTrak"] = skin.statTrak;
    }

    return j;
}

void fromJson(const json& j) noexcept
{
    if (!j.is_object())
        return;

    if (j.contains("Rage") && j["Rage"].is_object()) {
        const auto& r = j["Rage"];
        auto& rage = config.rage;
        read(r, "Enabled", rage.enabled);
        read(r, "Silent", rage.silent);
        read(r, "Auto fire", rage.autoFire);
        read(r, "Autowall", rage.autowall);
        read(r, "Force shot", rage.forceShot);
        read(r, "Resolver", rage.resolver);
        read(r, "Layer resolver", rage.layerResolver);
        read(r, "Resolver safe when unsure", rage.resolverSafeWhenUnsure);
        read(r, "Resolver steps", rage.resolverSteps);
        read(r, "Safe point", rage.safePoint);
        read(r, "Knife bot", rage.knifeBot);
        read(r, "Knife bot mode", rage.knifeBotMode);
        read(r, "Zeus bot", rage.zeusBot);
        read(r, "Zeus hitchance", rage.zeusHitchance);
        read(r, "FOV", rage.fov);
        read(r, "Min damage override key", rage.minDamageOverrideKey);
        read(r, "Force body key", rage.forceBodyKey);
        read(r, "Force shot key", rage.forceShotKey);
        if (r.contains("Weapons") && r["Weapons"].is_array()) {
            for (std::size_t i = 0; i < rage.weapons.size() && i < r["Weapons"].size(); ++i) {
                const auto& jw = r["Weapons"][i];
                if (!jw.is_object())
                    continue;
                auto& w = rage.weapons[i];
                read(jw, "Override", w.override);
                read(jw, "Hitboxes", w.hitboxes);
                read(jw, "Multipoint", w.multipoint);
                read(jw, "Head scale", w.headScale);
                read(jw, "Auto scale", w.autoScale);
                read(jw, "Body scale", w.bodyScale);
                read(jw, "Min damage", w.minDamage);
                read(jw, "Min damage override", w.minDamageOverride);
                read(jw, "Hitchance", w.hitchance);
                read(jw, "Auto stop mode", w.autoStop);
                read(jw, "Auto stop early", w.autoStopEarly);
                read(jw, "Auto scope", w.autoScope);
                read(jw, "Prefer body", w.preferBody);
                read(jw, "Body if lethal", w.bodyIfLethal);
            }
        }
    }

    if (j.contains("Anti-aim") && j["Anti-aim"].is_object()) {
        const auto& a = j["Anti-aim"];
        auto& aa = config.antiAim;
        read(a, "Enabled", aa.enabled);
        read(a, "Pitch", aa.pitch);
        read(a, "Yaw base", aa.yawBase);
        // Старый конфиг (до билдера по состояниям): его углы становятся «Общим» состоянием
        {
            auto& def = aa.states[static_cast<int>(AaState::Default)];
            read(a, "Yaw offset", def.yawOffset);
            read(a, "Yaw modifier", def.modifier);
            read(a, "Modifier range", def.degree);
            read(a, "Spin speed", def.spinSpeed);
            read(a, "Desync left", def.desyncLeft);
            read(a, "Desync right", def.desyncRight);
            bool oldJitter = false;
            read(a, "Desync jitter", oldJitter);
            if (a.contains("Desync jitter"))
                def.desyncMode = static_cast<int>(oldJitter ? DesyncMode::Jitter : DesyncMode::Static);
            // старый «5-way» (±½ и ±¼ диапазона) — ровно N-way на 5 с тем же размахом: ways по умолчанию 5
        }
        if (a.contains("States"))
            readStates(a["States"], aa.states);
        if (a.contains("Snaps"))
            readSnaps(a["Snaps"], aa.snaps);
        aa.presetRack.clear();
        if (a.contains("Preset rack") && a["Preset rack"].is_array()) {
            for (const auto& jp : a["Preset rack"]) {
                AntiAimPreset preset;
                if (jp.contains("States"))
                    readStates(jp["States"], preset.states);
                if (jp.contains("Snaps"))
                    readSnaps(jp["Snaps"], preset.snaps);
                aa.presetRack.push_back(preset);
            }
        }
        read(a, "Defensive AA", aa.defensiveAa);
        read(a, "LC triggers", aa.lcTriggers);
        read(a, "LC trigger states", aa.lcTriggerStates);
        read(a, "Anti bruteforce triggers", aa.antiBruteTriggers);
        read(a, "Anti bruteforce mode", aa.antiBruteMode);
        aa.antiBruteMode = std::clamp(aa.antiBruteMode, 0, 2);
        read(a, "Anti bruteforce power", aa.antiBrutePower);
        read(a, "Anti bruteforce order", aa.antiBruteOrder);
        read(a, "Freestand static", aa.freestandStatic);
        read(a, "Edge yaw", aa.edgeYaw);
        read(a, "Edge key", aa.edgeKey);
        read(a, "Edge static", aa.edgeStatic);
        read(a, "Roll", aa.roll);
        aa.roll = std::clamp(aa.roll, -50, 50);
        read(a, "Safe head", aa.safeHead);
        aa.safeHead = std::clamp(aa.safeHead, 0, 2);
        read(a, "Legit AA", aa.legitAa);
        read(a, "Naturality", aa.naturality);
        read(a, "Naturality time", aa.naturalityTime);
        read(a, "Manual forward", aa.manualForward);
        read(a, "Desync", aa.desync);
        read(a, "Anti bruteforce reset", aa.antiBruteReset);
        read(a, "Static on manual", aa.staticOnManual);
        read(a, "Defensive", aa.defensive);
        read(a, "Defensive fov", aa.defensiveFov);
        read(a, "Defensive yaw", aa.defensiveYaw);
        read(a, "Defensive pitch", aa.defensivePitch);
        read(a, "Defensive jitter", aa.defensiveJitter);
        read(a, "Flicks", aa.flicks);
        read(a, "Flick trigger", aa.flickTrigger);
        read(a, "Flick side", aa.flickSide);
        read(a, "Flick angle", aa.flickAngle);
        read(a, "Flick yaw", aa.flickYaw);
        if (a.contains("Flick pitch value")) {
            read(a, "Flick pitch override", aa.flickPitchOverride);
            read(a, "Flick pitch value", aa.flickPitch);
        } else if (a.contains("Flick pitch")) {
            // старый пресет: 0 — как обычно, 1 — вверх, 2 — ноль
            int preset = 0;
            read(a, "Flick pitch", preset);
            aa.flickPitchOverride = preset == 1 || preset == 2;
            aa.flickPitch = preset == 2 ? 0 : -89;
        }
        read(a, "Flick every", aa.flickEvery);
        read(a, "Anti bruteforce", aa.antiBrute);
        read(a, "Inverter", aa.inverter);
        read(a, "Freestanding", aa.freestanding);
        read(a, "Freestand key", aa.freestandKey);
        read(a, "Manual left", aa.manualLeft);
        read(a, "Manual right", aa.manualRight);
        read(a, "Manual back", aa.manualBack);
        read(a, "Fake lag", aa.fakeLag);
        read(a, "Slide walk", aa.slideWalk);
    }

    if (j.contains("View") && j["View"].is_object()) {
        read(j["View"], "Remove scope zoom", config.view.removeScopeZoom);
        read(j["View"], "Second zoom", config.view.secondZoom);
        read(j["View"], "Second zoom factor", config.view.secondZoomFactor);
        read(j["View"], "Show fake angles", config.view.showFakeAngles);
        read(j["View"], "Grenade prediction", config.view.grenadePrediction);
        read(j["View"], "Dormant time", config.view.dormantTime);
        read(j["View"], "Sync render", config.view.syncRender);
        read(j["View"], "Screen logs", config.view.screenLogs);
        read(j["View"], "Watermark", config.view.watermark);
        read(j["View"], "ESP flags", config.view.espFlags);
        read(j["View"], "Aim dot", config.view.aimDot);
        read(j["View"], "Tab loadout", config.view.tabLoadout);
        read(j["View"], "Damage numbers", config.view.damageNumbers);
        read(j["View"], "Spread circle", config.view.spreadCircle);
        read(j["View"], "Footsteps", config.view.footsteps);
        read(j["View"], "Kill flash", config.view.killFlash);
        read(j["View"], "Grenade timers", config.view.grenadeTimers);
        read(j["View"], "Shot capsules", config.view.shotCapsules);
    }

    if (j.contains("Movement") && j["Movement"].is_object()) {
        const auto& m = j["Movement"];
        read(m, "Auto peek key", config.movement.autoPeekKey);
        // старый конфиг: флаг «возврат по отпусканию WASD» → режим
        bool onRelease = false;
        read(m, "Auto peek on release", onRelease);
        config.movement.autoPeekMode = onRelease ? static_cast<int>(AutoPeekMode::ShotOrRelease) : static_cast<int>(AutoPeekMode::Shot);
        read(m, "Auto peek mode", config.movement.autoPeekMode);
        config.movement.autoPeekMode = std::clamp(config.movement.autoPeekMode, 0, 1);
        read(m, "Fake duck key", config.movement.fakeDuckKey);
        read(m, "Fast ladder", config.movement.fastLadder);
        read(m, "Bunny hop", config.movement.bunnyHop);
        read(m, "Auto strafe mode", config.movement.autoStrafe);
    }

    if (j.contains("Auto buy") && j["Auto buy"].is_object()) {
        const auto& b = j["Auto buy"];
        read(b, "Enabled", config.autoBuy.enabled);
        read(b, "Primary", config.autoBuy.primary);
        read(b, "Secondary", config.autoBuy.secondary);
        read(b, "Extras", config.autoBuy.extras);
    }

    if (j.contains("Exploits") && j["Exploits"].is_object()) {
        const auto& e = j["Exploits"];
        auto& ex = config.exploits;
        read(e, "Double tap", ex.doubleTap);
        read(e, "Double tap key", ex.doubleTapKey);
        read(e, "Shift ticks", ex.shiftTicks);
        read(e, "Teleport key", ex.teleportKey);
        read(e, "Defensive", ex.defensive);
    }

    if (j.contains("Skins") && j["Skins"].is_object()) {
        const auto& s = j["Skins"];
        auto& skins = config.skins;
        read(s, "Knife", skins.knife);
        read(s, "Gloves", skins.gloves);
        read(s, "Agent T", skins.agentT);
        read(s, "Agent CT", skins.agentCT);
        if (s.contains("Weapons") && s["Weapons"].is_object()) {
            for (const auto& [key, w] : s["Weapons"].items()) {
                if (!w.is_object())
                    continue;
                int id = 0;
                if (std::from_chars(key.data(), key.data() + key.size(), id).ec != std::errc{})
                    continue;
                auto& skin = skins.weapons[id];
                read(w, "Paint kit", skin.paintKit);
                read(w, "Wear", skin.wear);
                read(w, "Seed", skin.seed);
                read(w, "StatTrak", skin.statTrak);
            }
        }
    }
}

void resetConfig() noexcept
{
    config = {};
}

}

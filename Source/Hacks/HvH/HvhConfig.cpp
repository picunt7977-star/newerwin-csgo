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
    a["Yaw offset"] = aa.yawOffset;
    a["Yaw modifier"] = aa.yawModifier;
    a["Modifier range"] = aa.modifierRange;
    a["Spin speed"] = aa.spinSpeed;
    a["Desync"] = aa.desync;
    a["Desync left"] = aa.desyncLeft;
    a["Desync right"] = aa.desyncRight;
    a["Desync jitter"] = aa.desyncJitter;
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

    auto& m = j["Movement"];
    writeKey(m, "Auto peek key", config.movement.autoPeekKey);
    m["Auto peek on release"] = config.movement.autoPeekOnRelease;
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
        read(a, "Yaw offset", aa.yawOffset);
        read(a, "Yaw modifier", aa.yawModifier);
        read(a, "Modifier range", aa.modifierRange);
        read(a, "Spin speed", aa.spinSpeed);
        read(a, "Desync", aa.desync);
        read(a, "Desync left", aa.desyncLeft);
        read(a, "Desync right", aa.desyncRight);
        read(a, "Desync jitter", aa.desyncJitter);
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
    }

    if (j.contains("Movement") && j["Movement"].is_object()) {
        const auto& m = j["Movement"];
        read(m, "Auto peek key", config.movement.autoPeekKey);
        read(m, "Auto peek on release", config.movement.autoPeekOnRelease);
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

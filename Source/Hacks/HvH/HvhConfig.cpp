#include <algorithm>
#include <charconv>
#include <string>

#include "HvhConfig.h"
#include "Binds.h"

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
    binds::suspend();
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
    r["Strict shots"] = rage.strictShots;
    r["Knife bot"] = rage.knifeBot;
    r["Knife bot mode"] = rage.knifeBotMode;
    r["Zeus bot"] = rage.zeusBot;
    r["Zeus hitchance"] = rage.zeusHitchance;
    r["Zeus margin"] = rage.zeusMargin;
    r["FOV"] = rage.fov;
    r["Dormant aimbot"] = rage.dormantAim;
    r["Dormant accuracy"] = rage.dormantAccuracy;
    r["Force body"] = rage.forceBody;
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
        jw["Hitchance"] = w.hitchance;
        jw["Auto stop mode"] = w.autoStop;
        jw["Auto stop early"] = w.autoStopEarly;
        jw["Peek stop"] = w.peekStop;
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
    a["Anti bruteforce triggers"] = aa.antiBruteTriggers;
    a["Anti bruteforce mode"] = aa.antiBruteMode;
    a["Anti bruteforce power"] = aa.antiBrutePower;
    a["Anti bruteforce order"] = aa.antiBruteOrder;
    a["Freestand static"] = aa.freestandStatic;
    a["Edge yaw"] = aa.edgeYaw;
    writeKey(a, "Edge key", aa.edgeKey);
    a["Edge static"] = aa.edgeStatic;
    a["Roll"] = aa.roll;
    a["Extended yaw"] = aa.extendedYaw;
    a["Safe head"] = aa.safeHead;
    a["Legit AA"] = aa.legitAa;
    a["Naturality"] = aa.naturality;
    a["Naturality time"] = aa.naturalityTime;
    writeKey(a, "Manual forward", aa.manualForward);
    a["Desync"] = aa.desync;
    a["Anti bruteforce reset"] = aa.antiBruteReset;
    a["Static on manual"] = aa.staticOnManual;
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
    a["Fake lag mode"] = aa.fakeLagMode;
    a["Fake lag air"] = aa.fakeLagAir;
    a["Fake lag standing"] = aa.fakeLagStanding;
    a["Slide walk"] = aa.slideWalk;
    a["Animation breaker"] = aa.animBreaker;
    a["Breaker ground"] = aa.breakerGround;
    a["Breaker air"] = aa.breakerAir;
    a["Breaker land pitch"] = aa.breakerLandPitch;

    j["View"]["Remove scope zoom"] = config.view.removeScopeZoom;
    j["View"]["Second zoom"] = config.view.secondZoom;
    j["View"]["Second zoom factor"] = config.view.secondZoomFactor;
    j["View"]["Show fake angles"] = config.view.showFakeAngles;
    j["View"]["Server model"] = config.view.serverModel;
    j["View"]["Server model color"] = { config.view.serverModelColor[0], config.view.serverModelColor[1], config.view.serverModelColor[2], config.view.serverModelColor[3] };
    j["View"]["Backtrack chams"] = config.view.backtrackChams;
    j["View"]["Backtrack chams hidden"] = config.view.backtrackChamsHidden;
    j["View"]["Backtrack chams color"] = { config.view.backtrackChamsColor[0], config.view.backtrackChamsColor[1], config.view.backtrackChamsColor[2], config.view.backtrackChamsColor[3] };
    j["View"]["Server box"] = config.view.serverBox;
    j["View"]["Server box color"] = { config.view.serverBoxColor[0], config.view.serverBoxColor[1], config.view.serverBoxColor[2], config.view.serverBoxColor[3] };
    j["View"]["Grenade prediction"] = config.view.grenadePrediction;
    j["View"]["Dormant time"] = config.view.dormantTime;
    j["View"]["Sync render"] = config.view.syncRender;
    j["View"]["Screen logs"] = config.view.screenLogs;
    j["View"]["Logs x"] = config.view.logsX;
    j["View"]["Logs y"] = config.view.logsY;
    j["View"]["Watermark"] = config.view.watermark;
    j["View"]["Hotkeys"] = config.view.hotkeys;
    j["View"]["Hotkeys x"] = config.view.hotkeysX;
    j["View"]["Hotkeys y"] = config.view.hotkeysY;
    j["View"]["Aspect ratio"] = config.view.aspectRatio;
    j["View"]["Aspect ratio value"] = config.view.aspectRatioValue;
    j["View"]["Viewmodel"] = config.view.viewmodel;
    j["View"]["Viewmodel fov"] = config.view.viewmodelFov;
    j["View"]["Viewmodel x"] = config.view.viewmodelX;
    j["View"]["Viewmodel y"] = config.view.viewmodelY;
    j["View"]["Viewmodel z"] = config.view.viewmodelZ;
    j["View"]["Custom scope"] = config.view.customScope;
    j["View"]["Scope size"] = config.view.scopeSize;
    j["View"]["Scope gap"] = config.view.scopeGap;
    j["View"]["Scope T"] = config.view.scopeT;
    j["View"]["Scope invert"] = config.view.scopeInvert;
    j["View"]["Scope thickness"] = config.view.scopeThickness;
    j["View"]["Damage indicator"] = config.view.damageIndicator;
    j["View"]["Shot markers"] = config.view.shotMarkers;
    j["View"]["Miss markers"] = config.view.missMarkers;
    j["View"]["Shot marker time"] = config.view.shotMarkerTime;
    j["View"]["Slowdown warning"] = config.view.slowdownWarning;
    j["View"]["Keybind list"] = config.view.keybindList;
    j["View"]["LC indicator"] = config.view.lcIndicator;
    j["View"]["Trash talk"] = config.view.trashTalk;
    j["View"]["Trash talk when"] = config.view.trashTalkWhen;
    j["View"]["ESP flags"] = config.view.espFlags;
    j["View"]["Aim dot"] = config.view.aimDot;
    j["View"]["Tab loadout"] = config.view.tabLoadout;
    j["View"]["Damage numbers"] = config.view.damageNumbers;
    j["View"]["Spread circle"] = config.view.spreadCircle;
    j["View"]["Footsteps"] = config.view.footsteps;
    j["View"]["Kill flash"] = config.view.killFlash;
    j["View"]["Grenade timers"] = config.view.grenadeTimers;
    j["View"]["Shot capsules"] = config.view.shotCapsules;
    j["View"]["Impact sparks"] = config.view.impactSparks;
    j["View"]["Spark color"] = { config.view.sparkColor[0], config.view.sparkColor[1], config.view.sparkColor[2], config.view.sparkColor[3] };
    j["View"]["Spark count"] = config.view.sparkCount;
    j["View"]["Offscreen arrows"] = config.view.offscreenArrows;
    j["View"]["Arrow radius"] = config.view.arrowRadius;
    j["View"]["Hit marker"] = config.view.hitMarker;
    j["View"]["Bullet tracers"] = config.view.bulletTracers;
    j["View"]["Tracer color"] = { config.view.tracerColor[0], config.view.tracerColor[1], config.view.tracerColor[2], config.view.tracerColor[3] };
    j["View"]["Local trail"] = config.view.localTrail;
    j["View"]["Trail color"] = { config.view.trailColor[0], config.view.trailColor[1], config.view.trailColor[2], config.view.trailColor[3] };
    j["View"]["Grenade warning"] = config.view.grenadeWarning;
    j["View"]["Zeus range"] = config.view.zeusRange;
    j["View"]["Seen warning"] = config.view.seenWarning;
    j["View"]["ESP smooth health"] = config.view.espSmoothHealth;
    j["View"]["ESP ammo bar"] = config.view.espAmmoBar;
    j["View"]["ESP distance"] = config.view.espDistance;
    j["View"]["ESP soft fill"] = config.view.espSoftFill;
    j["View"]["ESP glow box"] = config.view.espGlowBox;
    j["View"]["Spark size"] = config.view.sparkSize;

    {
        const auto& w = config.world;
        auto& o = j["World"];
        const auto rgb = [](const float* c) { return json::array({ c[0], c[1], c[2] }); };
        o["Enabled"] = w.enabled;
        o["Preset"] = w.preset;
        o["Wall color"] = rgb(w.wallColor);
        o["Prop color"] = rgb(w.propColor);
        o["Prop alpha"] = w.propAlpha;
        o["Sky color on"] = w.skyColorOn;
        o["Sky color"] = rgb(w.skyColor);
        o["Fog"] = w.fog;
        o["Fog color"] = rgb(w.fogColor);
        o["Fog start"] = w.fogStart;
        o["Fog end"] = w.fogEnd;
        o["Fog density"] = w.fogDensity;
        o["Ambient"] = w.ambient;
        o["Ambient color"] = rgb(w.ambientColor);
        o["Weather"] = w.weather;
        o["Particles"] = w.particles;
        o["Particle color"] = json::array({ w.particleColor[0], w.particleColor[1], w.particleColor[2], w.particleColor[3] });
        o["Particle size"] = w.particleSize;
        o["Particle radius"] = w.particleRadius;
        o["Tonemap"] = w.tonemap;
        o["Bloom"] = w.bloom;
        o["Exposure"] = w.exposure;
        o["Model brightness"] = w.modelBrightness;
        o["Model ambient"] = w.modelAmbient;
        o["Sun override"] = w.sunOverride;
        o["Sun pitch"] = w.sunPitch;
        o["Sun yaw"] = w.sunYaw;
    }

    auto& m = j["Movement"];
    writeKey(m, "Auto peek key", config.movement.autoPeekKey);
    m["Auto peek mode"] = config.movement.autoPeekMode;
    m["Auto peek teleport ticks"] = config.movement.peekTeleportTicks;
    writeKey(m, "Fake duck key", config.movement.fakeDuckKey);
    m["Fast ladder"] = config.movement.fastLadder;
    m["Super toss"] = config.movement.superToss;
    m["No fall damage"] = config.movement.noFallDamage;
    m["AI peek"] = config.movement.aiPeek;
    m["AI peek quality"] = config.movement.aiPeekQuality;
    m["AI peek safety"] = config.movement.aiPeekSafety;
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
    e["Server pack limit"] = ex.serverPackLimit;
    e["Fake latency"] = ex.fakeLatency;
    e["Fake latency ms"] = ex.fakeLatencyMs;
    e["Knife air no lag"] = ex.knifeAirNoLag;

    auto& d = j["Defensive"];
    const auto& df = config.defensive;
    d["Enabled"] = df.enabled;
    d["Triggers"] = df.triggers;
    d["In air"] = df.inAir;
    d["Peek shift"] = df.peekShift;
    writeKey(d, "Key", df.key);
    d["Peek ticks"] = df.peekTicks;
    d["Charge style"] = df.chargeStyle;
    d["Charge ticks"] = df.chargeTicks;
    d["Charge spread"] = df.chargeSpread;
    d["Interval"] = df.interval;
    e["Double tap mode"] = ex.doubleTapMode;
    writeKey(e, "Teleport key", ex.teleportKey);
    e["Hide shots"] = ex.hideShots;
    writeKey(e, "Hide shots key", ex.hideShotsKey);
    e["Lag peek"] = ex.lagPeek;
    writeKey(e, "Lag peek key", ex.lagPeekKey);
    e["Lag peek ticks"] = ex.lagPeekTicks;
    e["Lag peek predict"] = ex.lagPeekPredict;
    e["Auto hide shots"] = ex.autoHideShots;
    e["Auto hide shots rifles"] = ex.autoHsRifles;
    e["Auto hide shots deagle"] = ex.autoHsDeagle;
    e["Auto hide shots autosnipers"] = ex.autoHsAuto;

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

    j["Binds"] = binds::toJson();
    binds::resume();
    return j;
}

void fromJson(const json& j) noexcept
{
    if (!j.is_object())
        return;
    // Загрузка = чистый лист. Пустые бинды и отключённые поля в файл не пишутся, и без сброса
    // оставались от прошлого загруженного конфига (фристенд на X из eclipse-x2 жил бы и в новом)
    config = Config{};
    binds::reset();
    if (j.contains("Binds"))
        binds::fromJson(j["Binds"]);

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
        read(r, "Strict shots", rage.strictShots);
        read(r, "Knife bot", rage.knifeBot);
        read(r, "Knife bot mode", rage.knifeBotMode);
        read(r, "Zeus bot", rage.zeusBot);
        read(r, "Zeus hitchance", rage.zeusHitchance);
        read(r, "Zeus margin", rage.zeusMargin);
        read(r, "FOV", rage.fov);
        read(r, "Dormant aimbot", rage.dormantAim);
        read(r, "Dormant accuracy", rage.dormantAccuracy);
        rage.dormantAccuracy = std::clamp(rage.dormantAccuracy, 25, 95);
        read(r, "Force body", rage.forceBody);
        // Старые оверрайды (клавиша МД, «только в тело», форсшут) — теперь бинды на строки
        KeyBind oldMinDamageKey, oldForceBodyKey, oldForceShotKey;
        read(r, "Min damage override key", oldMinDamageKey);
        read(r, "Force body key", oldForceBodyKey);
        read(r, "Force shot key", oldForceShotKey);
        binds::migrate(&rage.forceBody, binds::Type::Bool, oldForceBodyKey, binds::Mode::Hold, 1.0f, "Только в тело");
        // форсшут был «включён, клавиша выключает» — бинд-переключатель в 0
        if (rage.forceShot)
            binds::migrate(&rage.forceShot, binds::Type::Bool, oldForceShotKey, binds::Mode::Toggle, 0.0f, "Форсшут");
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
                int oldOverride = -1;
                read(jw, "Min damage override", oldOverride);
                if (oldOverride > 0)
                    binds::migrate(&w.minDamage, binds::Type::Int, oldMinDamageKey, binds::Mode::Hold, static_cast<float>(oldOverride), "Мин. урон");
                read(jw, "Hitchance", w.hitchance);
                read(jw, "Auto stop mode", w.autoStop);
                read(jw, "Auto stop early", w.autoStopEarly);
                read(jw, "Peek stop", w.peekStop);
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
        read(a, "Anti bruteforce triggers", aa.antiBruteTriggers);
        read(a, "Anti bruteforce mode", aa.antiBruteMode);
        aa.antiBruteMode = std::clamp(aa.antiBruteMode, 0, 3);
        read(a, "Anti bruteforce power", aa.antiBrutePower);
        read(a, "Anti bruteforce order", aa.antiBruteOrder);
        read(a, "Freestand static", aa.freestandStatic);
        read(a, "Edge yaw", aa.edgeYaw);
        read(a, "Edge key", aa.edgeKey);
        read(a, "Edge static", aa.edgeStatic);
        read(a, "Roll", aa.roll);
        aa.roll = std::clamp(aa.roll, -50, 50);
        read(a, "Extended yaw", aa.extendedYaw);
        aa.extendedYaw = std::clamp(aa.extendedYaw, 0, 180);
        read(a, "Safe head", aa.safeHead);
        aa.safeHead = std::clamp(aa.safeHead, 0, 2);
        read(a, "Legit AA", aa.legitAa);
        read(a, "Naturality", aa.naturality);
        read(a, "Naturality time", aa.naturalityTime);
        read(a, "Manual forward", aa.manualForward);
        read(a, "Desync", aa.desync);
        read(a, "Anti bruteforce reset", aa.antiBruteReset);
        read(a, "Static on manual", aa.staticOnManual);
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
        read(a, "Fake lag mode", aa.fakeLagMode);
        read(a, "Fake lag air", aa.fakeLagAir);
        read(a, "Fake lag standing", aa.fakeLagStanding);
        aa.fakeLagMode = std::clamp(aa.fakeLagMode, 0, static_cast<int>(FakeLagMode::Count) - 1);
        aa.fakeLagAir = std::clamp(aa.fakeLagAir, 0, 14);
        aa.fakeLagStanding = std::clamp(aa.fakeLagStanding, 0, 14);
        read(a, "Slide walk", aa.slideWalk);
        read(a, "Animation breaker", aa.animBreaker);
        read(a, "Breaker ground", aa.breakerGround);
        read(a, "Breaker air", aa.breakerAir);
        read(a, "Breaker land pitch", aa.breakerLandPitch);
        aa.breakerGround = std::clamp(aa.breakerGround, 0, 2);
        aa.breakerAir = std::clamp(aa.breakerAir, 0, 2);
    }

    if (j.contains("View") && j["View"].is_object()) {
        read(j["View"], "Remove scope zoom", config.view.removeScopeZoom);
        read(j["View"], "Second zoom", config.view.secondZoom);
        read(j["View"], "Second zoom factor", config.view.secondZoomFactor);
        read(j["View"], "Show fake angles", config.view.showFakeAngles);
        read(j["View"], "Server model", config.view.serverModel);
        read(j["View"], "Backtrack chams", config.view.backtrackChams);
        read(j["View"], "Backtrack chams hidden", config.view.backtrackChamsHidden);
        if (const auto& color = j["View"]["Backtrack chams color"]; color.is_array() && color.size() == 4) {
            for (std::size_t i = 0; i < 4; ++i) {
                if (color[i].is_number())
                    config.view.backtrackChamsColor[i] = std::clamp(color[i].get<float>(), 0.0f, 1.0f);
            }
        }
        read(j["View"], "Server box", config.view.serverBox);
        if (const auto& color = j["View"]["Server box color"]; color.is_array() && color.size() == 4) {
            for (std::size_t i = 0; i < 4; ++i) {
                if (color[i].is_number())
                    config.view.serverBoxColor[i] = std::clamp(color[i].get<float>(), 0.0f, 1.0f);
            }
        }
        if (const auto& color = j["View"]["Server model color"]; color.is_array() && color.size() == 4) {
            for (std::size_t i = 0; i < 4; ++i) {
                if (color[i].is_number())
                    config.view.serverModelColor[i] = std::clamp(color[i].get<float>(), 0.0f, 1.0f);
            }
        }
        read(j["View"], "Grenade prediction", config.view.grenadePrediction);
        read(j["View"], "Dormant time", config.view.dormantTime);
        read(j["View"], "Sync render", config.view.syncRender);
        read(j["View"], "Screen logs", config.view.screenLogs);
        read(j["View"], "Logs x", config.view.logsX);
        read(j["View"], "Logs y", config.view.logsY);
        config.view.logsX = std::clamp(config.view.logsX, 0.0f, 1.0f);
        config.view.logsY = std::clamp(config.view.logsY, 0.0f, 0.97f);
        read(j["View"], "Watermark", config.view.watermark);
        read(j["View"], "Hotkeys", config.view.hotkeys);
        read(j["View"], "Hotkeys x", config.view.hotkeysX);
        read(j["View"], "Hotkeys y", config.view.hotkeysY);
        config.view.hotkeysX = std::clamp(config.view.hotkeysX, 0.0f, 0.95f);
        config.view.hotkeysY = std::clamp(config.view.hotkeysY, 0.0f, 0.95f);
        read(j["View"], "Aspect ratio", config.view.aspectRatio);
        read(j["View"], "Aspect ratio value", config.view.aspectRatioValue);
        read(j["View"], "Viewmodel", config.view.viewmodel);
        read(j["View"], "Viewmodel fov", config.view.viewmodelFov);
        read(j["View"], "Viewmodel x", config.view.viewmodelX);
        read(j["View"], "Viewmodel y", config.view.viewmodelY);
        read(j["View"], "Viewmodel z", config.view.viewmodelZ);
        read(j["View"], "Custom scope", config.view.customScope);
        read(j["View"], "Scope size", config.view.scopeSize);
        read(j["View"], "Scope gap", config.view.scopeGap);
        read(j["View"], "Scope T", config.view.scopeT);
        read(j["View"], "Scope invert", config.view.scopeInvert);
        read(j["View"], "Scope thickness", config.view.scopeThickness);
        config.view.scopeThickness = std::clamp(config.view.scopeThickness, 1, 12);
        read(j["View"], "Damage indicator", config.view.damageIndicator);
        read(j["View"], "Shot markers", config.view.shotMarkers);
        read(j["View"], "Miss markers", config.view.missMarkers);
        read(j["View"], "Shot marker time", config.view.shotMarkerTime);
        read(j["View"], "Slowdown warning", config.view.slowdownWarning);
        read(j["View"], "Keybind list", config.view.keybindList);
        read(j["View"], "LC indicator", config.view.lcIndicator);
        read(j["View"], "Trash talk", config.view.trashTalk);
        read(j["View"], "Trash talk when", config.view.trashTalkWhen);
        read(j["View"], "ESP flags", config.view.espFlags);
        read(j["View"], "Aim dot", config.view.aimDot);
        read(j["View"], "Tab loadout", config.view.tabLoadout);
        read(j["View"], "Damage numbers", config.view.damageNumbers);
        read(j["View"], "Spread circle", config.view.spreadCircle);
        read(j["View"], "Footsteps", config.view.footsteps);
        read(j["View"], "Kill flash", config.view.killFlash);
        read(j["View"], "Grenade timers", config.view.grenadeTimers);
        read(j["View"], "Shot capsules", config.view.shotCapsules);
        read(j["View"], "Impact sparks", config.view.impactSparks);
        if (const auto& color = j["View"]["Spark color"]; color.is_array() && color.size() == 4) {
            for (std::size_t i = 0; i < 4; ++i) {
                if (color[i].is_number())
                    config.view.sparkColor[i] = std::clamp(color[i].get<float>(), 0.0f, 1.0f);
            }
        }
        read(j["View"], "Spark count", config.view.sparkCount);
        read(j["View"], "Offscreen arrows", config.view.offscreenArrows);
        read(j["View"], "Arrow radius", config.view.arrowRadius);
        config.view.arrowRadius = std::clamp(config.view.arrowRadius, 60, 500);
        read(j["View"], "Hit marker", config.view.hitMarker);
        read(j["View"], "Bullet tracers", config.view.bulletTracers);
        read(j["View"], "Local trail", config.view.localTrail);
        for (const auto& [key, target] : { std::pair{ "Tracer color", config.view.tracerColor }, std::pair{ "Trail color", config.view.trailColor } }) {
            if (const auto& color = j["View"][key]; color.is_array() && color.size() == 4) {
                for (std::size_t i = 0; i < 4; ++i) {
                    if (color[i].is_number())
                        target[i] = std::clamp(color[i].get<float>(), 0.0f, 1.0f);
                }
            }
        }
        read(j["View"], "Grenade warning", config.view.grenadeWarning);
        read(j["View"], "Zeus range", config.view.zeusRange);
        read(j["View"], "Seen warning", config.view.seenWarning);
        read(j["View"], "ESP smooth health", config.view.espSmoothHealth);
        read(j["View"], "ESP ammo bar", config.view.espAmmoBar);
        read(j["View"], "ESP distance", config.view.espDistance);
        read(j["View"], "ESP soft fill", config.view.espSoftFill);
        read(j["View"], "ESP glow box", config.view.espGlowBox);
        config.view.sparkCount = std::clamp(config.view.sparkCount, 4, 60);
        read(j["View"], "Spark size", config.view.sparkSize);
        config.view.sparkSize = std::clamp(config.view.sparkSize, 0.3f, 3.0f);
    }

    if (j.contains("World") && j["World"].is_object()) {
        auto& w = config.world;
        const auto& o = j["World"];
        const auto rgb = [&o](const char* name, float* c) {
            if (const auto it = o.find(name); it != o.end() && it->is_array() && it->size() == 3) {
                for (std::size_t i = 0; i < 3; ++i) {
                    if ((*it)[i].is_number())
                        c[i] = std::clamp((*it)[i].get<float>(), 0.0f, 1.0f);
                }
            }
        };
        read(o, "Enabled", w.enabled);
        read(o, "Preset", w.preset);
        rgb("Wall color", w.wallColor);
        rgb("Prop color", w.propColor);
        read(o, "Prop alpha", w.propAlpha);
        w.propAlpha = std::clamp(w.propAlpha, 0.1f, 1.0f);
        read(o, "Sky color on", w.skyColorOn);
        rgb("Sky color", w.skyColor);
        read(o, "Fog", w.fog);
        rgb("Fog color", w.fogColor);
        read(o, "Fog start", w.fogStart);
        read(o, "Fog end", w.fogEnd);
        read(o, "Fog density", w.fogDensity);
        w.fogDensity = std::clamp(w.fogDensity, 0.0f, 1.0f);
        read(o, "Ambient", w.ambient);
        rgb("Ambient color", w.ambientColor);
        read(o, "Weather", w.weather);
        w.weather = std::clamp(w.weather, 0, 4);
        read(o, "Particles", w.particles);
        w.particles = std::clamp(w.particles, 20, 800);
        if (const auto it = o.find("Particle color"); it != o.end() && it->is_array() && it->size() == 4) {
            for (std::size_t i = 0; i < 4; ++i) {
                if ((*it)[i].is_number())
                    w.particleColor[i] = std::clamp((*it)[i].get<float>(), 0.0f, 1.0f);
            }
        }
        read(o, "Particle size", w.particleSize);
        w.particleSize = std::clamp(w.particleSize, 0.3f, 4.0f);
        read(o, "Particle radius", w.particleRadius);
        w.particleRadius = std::clamp(w.particleRadius, 250.0f, 2000.0f);
        read(o, "Tonemap", w.tonemap);
        read(o, "Bloom", w.bloom);
        w.bloom = std::clamp(w.bloom, 0.0f, 10.0f);
        read(o, "Exposure", w.exposure);
        w.exposure = std::clamp(w.exposure, 0.1f, 4.0f);
        read(o, "Model brightness", w.modelBrightness);
        read(o, "Model ambient", w.modelAmbient);
        w.modelAmbient = std::clamp(w.modelAmbient, 0.0f, 5.0f);
        read(o, "Sun override", w.sunOverride);
        read(o, "Sun pitch", w.sunPitch);
        w.sunPitch = std::clamp(w.sunPitch, 5.0f, 90.0f);
        read(o, "Sun yaw", w.sunYaw);
        w.sunYaw = std::clamp(w.sunYaw, -180.0f, 180.0f);
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
        read(m, "Auto peek teleport ticks", config.movement.peekTeleportTicks);
        read(m, "Fake duck key", config.movement.fakeDuckKey);
        read(m, "Fast ladder", config.movement.fastLadder);
        read(m, "Super toss", config.movement.superToss);
        read(m, "No fall damage", config.movement.noFallDamage);
        read(m, "AI peek", config.movement.aiPeek);
        read(m, "AI peek quality", config.movement.aiPeekQuality);
        read(m, "AI peek safety", config.movement.aiPeekSafety);
        config.movement.aiPeekQuality = std::clamp(config.movement.aiPeekQuality, 30, 100);
        config.movement.aiPeekSafety = std::clamp(config.movement.aiPeekSafety, 1, 16);
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
        read(e, "Server pack limit", ex.serverPackLimit);
        read(e, "Fake latency", ex.fakeLatency);
        read(e, "Fake latency ms", ex.fakeLatencyMs);
        ex.fakeLatencyMs = std::clamp(ex.fakeLatencyMs, 0, 200);
        ex.serverPackLimit = ex.serverPackLimit <= 0 ? 0 : std::clamp(ex.serverPackLimit, 2, 16);
        read(e, "Knife air no lag", ex.knifeAirNoLag);
        read(e, "Double tap mode", ex.doubleTapMode);
        read(e, "Teleport key", ex.teleportKey);
        read(e, "Hide shots", ex.hideShots);
        read(e, "Hide shots key", ex.hideShotsKey);
        read(e, "Lag peek", ex.lagPeek);
        read(e, "Lag peek key", ex.lagPeekKey);
        read(e, "Lag peek ticks", ex.lagPeekTicks);
        ex.lagPeekTicks = std::clamp(ex.lagPeekTicks, 2, 14);
        read(e, "Lag peek predict", ex.lagPeekPredict);
        ex.lagPeekPredict = std::clamp(ex.lagPeekPredict, 2, 16);
        read(e, "Auto hide shots", ex.autoHideShots);
        read(e, "Auto hide shots rifles", ex.autoHsRifles);
        read(e, "Auto hide shots deagle", ex.autoHsDeagle);
        read(e, "Auto hide shots autosnipers", ex.autoHsAuto);
    }

    if (j.contains("Defensive") && j["Defensive"].is_object()) {
        const auto& d = j["Defensive"];
        auto& df = config.defensive;
        read(d, "Enabled", df.enabled);
        read(d, "Triggers", df.triggers);
        read(d, "In air", df.inAir);
        read(d, "Peek shift", df.peekShift);
        read(d, "Key", df.key);
        read(d, "Peek ticks", df.peekTicks);
        df.peekTicks = std::clamp(df.peekTicks, 2, 16);
        read(d, "Charge style", df.chargeStyle);
        df.chargeStyle = std::clamp(df.chargeStyle, 0, static_cast<int>(ChargeStyle::Count) - 1);
        read(d, "Charge ticks", df.chargeTicks);
        df.chargeTicks = std::clamp(df.chargeTicks, 4, 14);
        read(d, "Charge spread", df.chargeSpread);
        df.chargeSpread = std::clamp(df.chargeSpread, 1, 6);
        read(d, "Interval", df.interval);
        df.interval = std::clamp(df.interval, 1, 64);
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
    binds::reset();
}

}

// ================= бинды на любую настройку =================
namespace hvh::binds
{

namespace
{
std::vector<Bind> list;

[[nodiscard]] char* fieldOf(const Bind& bind) noexcept
{
    return reinterpret_cast<char*>(&config) + bind.offset;
}

[[nodiscard]] std::size_t sizeOf(Type type) noexcept
{
    return type == Type::Bool ? sizeof(bool) : type == Type::Float ? sizeof(float) : sizeof(int);
}
}

bool bindable(const void* field) noexcept
{
    const auto* p = static_cast<const char*>(field);
    const auto* begin = reinterpret_cast<const char*>(&config);
    return p >= begin && p < begin + sizeof(Config);
}

Bind* find(std::uint32_t id) noexcept
{
    for (auto& b : list)
        if (b.id == id)
            return &b;
    return nullptr;
}

void migrate(void* field, Type type, const KeyBind& key, Mode mode, float value, const char* label) noexcept
{
    if (!key.isSet() || !bindable(field))
        return;
    const auto offset = static_cast<std::uint32_t>(static_cast<char*>(field) - reinterpret_cast<char*>(&config));
    for (const auto& b : list) {
        if (b.offset == offset)
            return;
    }
    Bind fresh;
    fresh.id = offset * 2654435761u;
    fresh.row = 0;
    fresh.label = label;
    fresh.type = type;
    fresh.offset = offset;
    fresh.resolved = true;
    fresh.key = key;
    fresh.mode = mode;
    fresh.value = value;
    list.push_back(std::move(fresh));
}

Bind* attach(std::uint32_t row, const char* label, Type type, void* field, bool create) noexcept
{
    if (!bindable(field))
        return nullptr;
    const auto offset = static_cast<std::uint32_t>(static_cast<char*>(field) - reinterpret_cast<char*>(&config));
    if (offset + sizeOf(type) > sizeof(Config))
        return nullptr;
    const std::uint32_t id = row ^ (offset * 2654435761u);
    Bind* bind = find(id);
    if (!bind) {
        // конфиг из другой сборки: бинд этой строки ещё не привязан — забираем его
        for (auto& b : list) {
            // row 0 — перенесён из старого оверрайда: строки ещё не знал, только поле
            if ((!b.resolved && b.row == row && b.type == type) || (b.row == 0 && b.offset == offset && b.type == type)) {
                b.row = row;
                b.id = id;
                b.offset = offset;
                b.resolved = true;
                bind = &b;
                break;
            }
        }
    }
    if (!bind) {
        if (!create)
            return nullptr;
        Bind fresh;
        fresh.id = id;
        fresh.row = row;
        fresh.type = type;
        fresh.offset = offset;
        fresh.resolved = true;
        fresh.label = label;
        // по умолчанию бинд делает противоположное текущему (для тоггла) или то же значение
        const float now = type == Type::Bool ? (*reinterpret_cast<bool*>(field) ? 1.0f : 0.0f)
            : type == Type::Float ? *reinterpret_cast<float*>(field) : static_cast<float>(*reinterpret_cast<int*>(field));
        fresh.value = type == Type::Bool ? (now != 0.0f ? 0.0f : 1.0f) : now;
        list.push_back(std::move(fresh));
        return &list.back();
    }
    if (label && bind->label != label)
        bind->label = label;
    return bind;
}

void remove(std::uint32_t id) noexcept
{
    for (auto it = list.begin(); it != list.end(); ++it) {
        if (it->id != id)
            continue;
        if (it->active && it->resolved)
            write(*it, it->base);
        list.erase(it);
        return;
    }
}

std::vector<Bind>& all() noexcept
{
    return list;
}

float read(const Bind& bind) noexcept
{
    char* field = fieldOf(bind);
    switch (bind.type) {
    case Type::Bool: return *reinterpret_cast<bool*>(field) ? 1.0f : 0.0f;
    case Type::Float: return *reinterpret_cast<float*>(field);
    default: return static_cast<float>(*reinterpret_cast<int*>(field));
    }
}

void write(const Bind& bind, float value) noexcept
{
    char* field = fieldOf(bind);
    switch (bind.type) {
    case Type::Bool: *reinterpret_cast<bool*>(field) = value != 0.0f; break;
    case Type::Float: *reinterpret_cast<float*>(field) = value; break;
    default: *reinterpret_cast<int*>(field) = static_cast<int>(value); break;
    }
}

void update() noexcept
{
    for (auto& b : list) {
        if (!b.resolved)
            continue;
        bool want = false;
        if (b.key.isSet()) {
            switch (b.mode) {
            case Mode::Hold: want = b.key.isDown(); break;
            case Mode::HoldOff: want = !b.key.isDown(); break;
            case Mode::Toggle:
                if (b.key.isPressed())
                    b.toggled = !b.toggled;
                want = b.toggled;
                break;
            }
        }
        if (want && !b.active) {
            b.base = read(b);
            write(b, b.value);
            b.active = true;
        } else if (want) {
            b.value = read(b);          // поменяли в меню, пока бинд держится, — это новое значение бинда
        } else if (b.active) {
            write(b, b.base);
            b.active = false;
        }
    }
}

void suspend() noexcept
{
    for (const auto& b : list)
        if (b.active && b.resolved)
            write(b, b.base);
}

void resume() noexcept
{
    for (const auto& b : list)
        if (b.active && b.resolved)
            write(b, b.value);
}

void reset() noexcept
{
    list.clear();
}

json toJson() noexcept
{
    json j = json::array();
    for (const auto& b : list) {
        if (!b.key.isSet())
            continue;   // открыли окно бинда и не назначили клавишу — не сохраняем
        json e;
        e["Id"] = b.id;
        e["Row"] = b.row;
        e["Label"] = b.label;
        e["Type"] = static_cast<int>(b.type);
        e["Offset"] = b.offset;
        e["Layout"] = sizeof(Config);
        e["Mode"] = static_cast<int>(b.mode);
        e["Value"] = b.value;
        if (b.key.isSet())
            e["Key"] = b.key.toString();
        j.push_back(std::move(e));
    }
    return j;
}

void fromJson(const json& j) noexcept
{
    if (!j.is_array())
        return;
    for (const auto& e : j) {
        if (!e.is_object() || !e.contains("Id"))
            continue;
        Bind b;
        b.id = e.value("Id", 0u);
        b.row = e.value("Row", 0u);
        b.label = e.value("Label", std::string{});
        b.type = static_cast<Type>(std::clamp(e.value("Type", 0), 0, 3));
        b.offset = e.value("Offset", 0u);
        b.mode = static_cast<Mode>(std::clamp(e.value("Mode", 0), 0, 2));
        b.value = e.value("Value", 1.0f);
        if (e.contains("Key") && e["Key"].is_string())
            b.key = KeyBind{ e["Key"].get<std::string>() };
        // та же раскладка Config — адрес верный сразу, иначе ждём, пока строка нарисуется в меню
        b.resolved = e.value("Layout", std::size_t{ 0 }) == sizeof(Config) && b.offset + sizeOf(b.type) <= sizeof(Config);
        if (b.id && !find(b.id))
            list.push_back(std::move(b));
    }
}

const char* modeName(Mode mode) noexcept
{
    switch (mode) {
    case Mode::Hold: return "Зажатие";
    case Mode::Toggle: return "Переключение";
    case Mode::HoldOff: return "Выкл. на зажатие";
    }
    return "";
}

}

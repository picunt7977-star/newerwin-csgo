#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <initializer_list>
#include <array>
#include <fstream>
#include <iterator>
#include <string>
#include <unordered_map>
#include <tuple>
#include <vector>

#include "Platform/Macros/IsPlatform.h"

#if IS_WIN32()
#include <ShlObj.h>
#include <Windows.h>
#endif

#include "imgui/imgui.h"
#include "imgui/imgui_stdlib.h"

#include "imguiCustom.h"

#include "GUI.h"
#include "Config.h"
#include "ConfigStructs.h"
#include "Hacks/Misc.h"
#include "InventoryChanger/InventoryChanger.h"
#include "Helpers.h"
#include "CSGO/InputSystem.h"
#include "Hacks/Visuals.h"
#include "Hacks/Chams.h"
#include "Hacks/Glow.h"
#include "Hacks/Backtrack.h"
#include "Hacks/Sound.h"
#include "Hacks/StreamProofESP.h"
#include <Config/ResetConfigurator.h>
#include "Hacks/Features.h"
#include "Hacks/HvH/Ragebot.h"
#include "Hacks/HvH/AntiAim.h"
#include "Hacks/HvH/HvhConfig.h"
#include "Hacks/HvH/HvhGui.h"
#include "Hacks/HvH/ModelPreview.h"

#include "Hooks.h"

constexpr auto windowFlags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize
| ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;

static ImFont* addFontFromVFONT(const std::string& path, float size, const ImWchar* glyphRanges, bool merge) noexcept
{
    auto file = Helpers::loadBinaryFile(path);
    if (!Helpers::decodeVFONT(file))
        return nullptr;

    ImFontConfig cfg;
    cfg.FontData = file.data();
    cfg.FontDataSize = file.size();
    cfg.FontDataOwnedByAtlas = false;
    cfg.MergeMode = merge;
    cfg.GlyphRanges = glyphRanges;
    cfg.SizePixels = size;

    return ImGui::GetIO().Fonts->AddFont(&cfg);
}

GUI::GUI() noexcept
{
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();

    style.ScrollbarSize = 9.0f;

    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;

    ImFontConfig cfg;
    cfg.SizePixels = 15.0f;

#if IS_WIN32()
    if (PWSTR pathToFonts; SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Fonts, 0, nullptr, &pathToFonts))) {
        const std::filesystem::path path{ pathToFonts };
        CoTaskMemFree(pathToFonts);

        fonts.normal15px = io.Fonts->AddFontFromFileTTF((path / "tahoma.ttf").string().c_str(), 15.0f, &cfg, Helpers::getFontGlyphRanges());
        if (!fonts.normal15px)
            io.Fonts->AddFontDefault(&cfg);

        cfg.MergeMode = true;
        static constexpr ImWchar symbol[]{
            0x2605, 0x2605, // ★
            0
        };
        io.Fonts->AddFontFromFileTTF((path / "seguisym.ttf").string().c_str(), 15.0f, &cfg, symbol);
        cfg.MergeMode = false;
    }
#else
    fonts.normal15px = addFontFromVFONT("csgo/panorama/fonts/notosans-regular.vfont", 15.0f, Helpers::getFontGlyphRanges(), false);
#endif
    if (!fonts.normal15px)
        io.Fonts->AddFontDefault(&cfg);
    addFontFromVFONT("csgo/panorama/fonts/notosanskr-regular.vfont", 15.0f, io.Fonts->GetGlyphRangesKorean(), true);
    addFontFromVFONT("csgo/panorama/fonts/notosanssc-regular.vfont", 17.0f, io.Fonts->GetGlyphRangesChineseFull(), true);

    // шрифты меню: обычный, жирный для заголовков, крупный для логотипа
    hvh::ui::fonts.regular = fonts.normal15px;
    hvh::ui::fonts.bold = addFontFromVFONT("csgo/panorama/fonts/notosans-bold.vfont", 15.0f, Helpers::getFontGlyphRanges(), false);
    hvh::ui::fonts.logo = addFontFromVFONT("csgo/panorama/fonts/notosans-bold.vfont", 28.0f, io.Fonts->GetGlyphRangesDefault(), false);
    hvh::ui::applyStyle();
}

void GUI::render(Hooks& hooks, const EngineInterfaces& engineInterfaces, const ClientInterfaces& clientInterfaces, const OtherInterfaces& interfaces, const Memory& memory, Config& config) noexcept
{
    auto& features = config.getFeatures();
    renderGuiStyle2(engineInterfaces, clientInterfaces, interfaces, memory, config);
    if (features.misc.unhook) {
        hvh::antiaim::updateEventListener(engineInterfaces, memory, true);
        hvh::ragebot::updateEventListener(engineInterfaces, memory, true);
        hooks.uninstall(features.misc, features.glow, features.visuals, features.inventoryChanger);
        features.misc.unhook = false;
    }
}

void GUI::updateColors(Config&) const noexcept
{
    hvh::ui::applyStyle();
}

void GUI::handleToggle(Misc& misc, const OtherInterfaces& interfaces) noexcept
{
    if (misc.isMenuKeyPressed()) {
        open = !open;
        if (!open)
            interfaces.getInputSystem().resetInputState();
#if !IS_WIN32()
        ImGui::GetIO().MouseDrawCursor = open;
#endif
    }
}

static void menuBarItem(const char* name, bool& enabled) noexcept
{
    if (ImGui::MenuItem(name)) {
        enabled = true;
        ImGui::SetWindowFocus(name);
        ImGui::SetWindowPos(name, { 100.0f, 100.0f });
    }
}

void GUI::renderMenuBar(Features& features) noexcept
{
    if (ImGui::BeginMainMenuBar()) {
        menuBarItem("Aimbot", window.aimbot);
        menuBarItem("Triggerbot", window.triggerbot);
        features.backtrack.menuBarItem();
        features.glow.menuBarItem();
        menuBarItem("Chams", window.chams);
        StreamProofESP::menuBarItem();
        features.visuals.menuBarItem();
        features.inventoryChanger.menuBarItem();
        features.sound.menuBarItem();
        menuBarItem("Style", window.style);
        features.misc.menuBarItem();
        menuBarItem("Config", window.config);
        ImGui::EndMainMenuBar();   
    }
}

void GUI::renderAimbotWindow(Config& config, bool contentOnly) noexcept
{
    if (!contentOnly) {
        if (!window.aimbot)
            return;
        ImGui::SetNextWindowSize({ 600.0f, 0.0f });
        ImGui::Begin("Aimbot", &window.aimbot, windowFlags);
    }
    ImGui::Checkbox("On key", &config.aimbotOnKey);
    ImGui::SameLine();
    ImGui::PushID("Aimbot Key");
    ImGui::hotkey("", config.aimbotKey);
    ImGui::PopID();
    ImGui::SameLine();
    ImGui::PushID(2);
    ImGui::PushItemWidth(70.0f);
    ImGui::Combo("", &config.aimbotKeyMode, "Hold\0Toggle\0");
    ImGui::PopItemWidth();
    ImGui::PopID();
    ImGui::Separator();
    static int currentCategory{ 0 };
    ImGui::PushItemWidth(110.0f);
    ImGui::PushID(0);
    ImGui::Combo("", &currentCategory, "All\0Pistols\0Heavy\0SMG\0Rifles\0");
    ImGui::PopID();
    ImGui::SameLine();
    static int currentWeapon{ 0 };
    ImGui::PushID(1);

    switch (currentCategory) {
    case 0:
        currentWeapon = 0;
        ImGui::NewLine();
        break;
    case 1: {
        static int currentPistol{ 0 };
        static constexpr const char* pistols[]{ "All", "Glock-18", "P2000", "USP-S", "Dual Berettas", "P250", "Tec-9", "Five-Seven", "CZ-75", "Desert Eagle", "Revolver" };

        ImGui::Combo("", &currentPistol, [](void* config, int idx, const char** out_text) {
            if (static_cast<Config*>(config)->aimbot[idx ? idx : 35].enabled) {
                static std::string name;
                name = pistols[idx];
                *out_text = name.append(" *").c_str();
            } else {
                *out_text = pistols[idx];
            }
            return true;
            }, &config, IM_ARRAYSIZE(pistols));

        currentWeapon = currentPistol ? currentPistol : 35;
        break;
    }
    case 2: {
        static int currentHeavy{ 0 };
        static constexpr const char* heavies[]{ "All", "Nova", "XM1014", "Sawed-off", "MAG-7", "M249", "Negev" };

        ImGui::Combo("", &currentHeavy, [](void* config, int idx, const char** out_text) {
            if (static_cast<Config*>(config)->aimbot[idx ? idx + 10 : 36].enabled) {
                static std::string name;
                name = heavies[idx];
                *out_text = name.append(" *").c_str();
            } else {
                *out_text = heavies[idx];
            }
            return true;
            }, &config, IM_ARRAYSIZE(heavies));

        currentWeapon = currentHeavy ? currentHeavy + 10 : 36;
        break;
    }
    case 3: {
        static int currentSmg{ 0 };
        static constexpr const char* smgs[]{ "All", "Mac-10", "MP9", "MP7", "MP5-SD", "UMP-45", "P90", "PP-Bizon" };

        ImGui::Combo("", &currentSmg, [](void* config, int idx, const char** out_text) {
            if (static_cast<Config*>(config)->aimbot[idx ? idx + 16 : 37].enabled) {
                static std::string name;
                name = smgs[idx];
                *out_text = name.append(" *").c_str();
            } else {
                *out_text = smgs[idx];
            }
            return true;
            }, &config, IM_ARRAYSIZE(smgs));

        currentWeapon = currentSmg ? currentSmg + 16 : 37;
        break;
    }
    case 4: {
        static int currentRifle{ 0 };
        static constexpr const char* rifles[]{ "All", "Galil AR", "Famas", "AK-47", "M4A4", "M4A1-S", "SSG-08", "SG-553", "AUG", "AWP", "G3SG1", "SCAR-20" };

        ImGui::Combo("", &currentRifle, [](void* config, int idx, const char** out_text) {
            if (static_cast<Config*>(config)->aimbot[idx ? idx + 23 : 38].enabled) {
                static std::string name;
                name = rifles[idx];
                *out_text = name.append(" *").c_str();
            } else {
                *out_text = rifles[idx];
            }
            return true;
            }, &config, IM_ARRAYSIZE(rifles));

        currentWeapon = currentRifle ? currentRifle + 23 : 38;
        break;
    }
    }
    ImGui::PopID();
    ImGui::SameLine();
    ImGui::Checkbox("Enabled", &config.aimbot[currentWeapon].enabled);
    ImGui::Columns(2, nullptr, false);
    ImGui::SetColumnOffset(1, 220.0f);
    ImGui::Checkbox("Aimlock", &config.aimbot[currentWeapon].aimlock);
    ImGui::Checkbox("Silent", &config.aimbot[currentWeapon].silent);
    ImGui::Checkbox("Friendly fire", &config.aimbot[currentWeapon].friendlyFire);
    ImGui::Checkbox("Visible only", &config.aimbot[currentWeapon].visibleOnly);
    ImGui::Checkbox("Scoped only", &config.aimbot[currentWeapon].scopedOnly);
    ImGui::Checkbox("Ignore flash", &config.aimbot[currentWeapon].ignoreFlash);
    ImGui::Checkbox("Ignore smoke", &config.aimbot[currentWeapon].ignoreSmoke);
    ImGui::Checkbox("Auto shot", &config.aimbot[currentWeapon].autoShot);
    ImGui::Checkbox("Auto scope", &config.aimbot[currentWeapon].autoScope);
    ImGui::Combo("Bone", &config.aimbot[currentWeapon].bone, "Nearest\0Best damage\0Head\0Neck\0Sternum\0Chest\0Stomach\0Pelvis\0");
    ImGui::NextColumn();
    ImGui::PushItemWidth(240.0f);
    ImGui::SliderFloat("Fov", &config.aimbot[currentWeapon].fov, 0.0f, 255.0f, "%.2f", ImGuiSliderFlags_Logarithmic);
    ImGui::SliderFloat("Smooth", &config.aimbot[currentWeapon].smooth, 1.0f, 100.0f, "%.2f");
    ImGui::SliderFloat("Max aim inaccuracy", &config.aimbot[currentWeapon].maxAimInaccuracy, 0.0f, 1.0f, "%.5f", ImGuiSliderFlags_Logarithmic);
    ImGui::SliderFloat("Max shot inaccuracy", &config.aimbot[currentWeapon].maxShotInaccuracy, 0.0f, 1.0f, "%.5f", ImGuiSliderFlags_Logarithmic);
    ImGui::InputInt("Min damage", &config.aimbot[currentWeapon].minDamage);
    config.aimbot[currentWeapon].minDamage = std::clamp(config.aimbot[currentWeapon].minDamage, 0, 250);
    ImGui::Checkbox("Killshot", &config.aimbot[currentWeapon].killshot);
    ImGui::Checkbox("Between shots", &config.aimbot[currentWeapon].betweenShots);
    ImGui::Columns(1);
    if (!contentOnly)
        ImGui::End();
}

void GUI::renderTriggerbotWindow(Config& config, bool contentOnly) noexcept
{
    if (!contentOnly) {
        if (!window.triggerbot)
            return;
        ImGui::SetNextWindowSize({ 0.0f, 0.0f });
        ImGui::Begin("Triggerbot", &window.triggerbot, windowFlags);
    }
    static int currentCategory{ 0 };
    ImGui::PushItemWidth(110.0f);
    ImGui::PushID(0);
    ImGui::Combo("", &currentCategory, "All\0Pistols\0Heavy\0SMG\0Rifles\0Zeus x27\0");
    ImGui::PopID();
    ImGui::SameLine();
    static int currentWeapon{ 0 };
    ImGui::PushID(1);
    switch (currentCategory) {
    case 0:
        currentWeapon = 0;
        ImGui::NewLine();
        break;
    case 5:
        currentWeapon = 39;
        ImGui::NewLine();
        break;

    case 1: {
        static int currentPistol{ 0 };
        static constexpr const char* pistols[]{ "All", "Glock-18", "P2000", "USP-S", "Dual Berettas", "P250", "Tec-9", "Five-Seven", "CZ-75", "Desert Eagle", "Revolver" };

        ImGui::Combo("", &currentPistol, [](void* config, int idx, const char** out_text) {
            if (static_cast<Config*>(config)->triggerbot[idx ? idx : 35].enabled) {
                static std::string name;
                name = pistols[idx];
                *out_text = name.append(" *").c_str();
            } else {
                *out_text = pistols[idx];
            }
            return true;
            }, &config, IM_ARRAYSIZE(pistols));

        currentWeapon = currentPistol ? currentPistol : 35;
        break;
    }
    case 2: {
        static int currentHeavy{ 0 };
        static constexpr const char* heavies[]{ "All", "Nova", "XM1014", "Sawed-off", "MAG-7", "M249", "Negev" };

        ImGui::Combo("", &currentHeavy, [](void* config, int idx, const char** out_text) {
            if (static_cast<Config*>(config)->triggerbot[idx ? idx + 10 : 36].enabled) {
                static std::string name;
                name = heavies[idx];
                *out_text = name.append(" *").c_str();
            } else {
                *out_text = heavies[idx];
            }
            return true;
            }, &config, IM_ARRAYSIZE(heavies));

        currentWeapon = currentHeavy ? currentHeavy + 10 : 36;
        break;
    }
    case 3: {
        static int currentSmg{ 0 };
        static constexpr const char* smgs[]{ "All", "Mac-10", "MP9", "MP7", "MP5-SD", "UMP-45", "P90", "PP-Bizon" };

        ImGui::Combo("", &currentSmg, [](void* config, int idx, const char** out_text) {
            if (static_cast<Config*>(config)->triggerbot[idx ? idx + 16 : 37].enabled) {
                static std::string name;
                name = smgs[idx];
                *out_text = name.append(" *").c_str();
            } else {
                *out_text = smgs[idx];
            }
            return true;
            }, &config, IM_ARRAYSIZE(smgs));

        currentWeapon = currentSmg ? currentSmg + 16 : 37;
        break;
    }
    case 4: {
        static int currentRifle{ 0 };
        static constexpr const char* rifles[]{ "All", "Galil AR", "Famas", "AK-47", "M4A4", "M4A1-S", "SSG-08", "SG-553", "AUG", "AWP", "G3SG1", "SCAR-20" };

        ImGui::Combo("", &currentRifle, [](void* config, int idx, const char** out_text) {
            if (static_cast<Config*>(config)->triggerbot[idx ? idx + 23 : 38].enabled) {
                static std::string name;
                name = rifles[idx];
                *out_text = name.append(" *").c_str();
            } else {
                *out_text = rifles[idx];
            }
            return true;
            }, &config, IM_ARRAYSIZE(rifles));

        currentWeapon = currentRifle ? currentRifle + 23 : 38;
        break;
    }
    }
    ImGui::PopID();
    ImGui::SameLine();
    ImGui::Checkbox("Enabled", &config.triggerbot[currentWeapon].enabled);
    ImGui::Separator();
    ImGui::hotkey("Hold Key", config.triggerbotHoldKey);
    ImGui::Checkbox("Friendly fire", &config.triggerbot[currentWeapon].friendlyFire);
    ImGui::Checkbox("Scoped only", &config.triggerbot[currentWeapon].scopedOnly);
    ImGui::Checkbox("Ignore flash", &config.triggerbot[currentWeapon].ignoreFlash);
    ImGui::Checkbox("Ignore smoke", &config.triggerbot[currentWeapon].ignoreSmoke);
    ImGui::SetNextItemWidth(85.0f);
    ImGui::Combo("Hitgroup", &config.triggerbot[currentWeapon].hitgroup, "All\0Head\0Chest\0Stomach\0Left arm\0Right arm\0Left leg\0Right leg\0");
    ImGui::PushItemWidth(220.0f);
    ImGui::SliderInt("Shot delay", &config.triggerbot[currentWeapon].shotDelay, 0, 250, "%d ms");
    ImGui::InputInt("Min damage", &config.triggerbot[currentWeapon].minDamage);
    config.triggerbot[currentWeapon].minDamage = std::clamp(config.triggerbot[currentWeapon].minDamage, 0, 250);
    ImGui::Checkbox("Killshot", &config.triggerbot[currentWeapon].killshot);
    ImGui::SliderFloat("Burst Time", &config.triggerbot[currentWeapon].burstTime, 0.0f, 0.5f, "%.3f s");

    if (!contentOnly)
        ImGui::End();
}

void GUI::renderChamsWindow(Config& config, bool contentOnly) noexcept
{
    if (!contentOnly) {
        if (!window.chams)
            return;
        ImGui::SetNextWindowSize({ 0.0f, 0.0f });
        ImGui::Begin("Chams", &window.chams, windowFlags);
    }

    ImGui::hotkey("Toggle Key", config.getFeatures().chams.toggleKey, 80.0f);
    ImGui::hotkey("Hold Key", config.getFeatures().chams.holdKey, 80.0f);
    ImGui::Separator();

    static int currentCategory{ 0 };
    ImGui::PushItemWidth(110.0f);
    ImGui::PushID(0);

    static int material = 1;

    if (ImGui::Combo("", &currentCategory, "Allies\0Enemies\0Planting\0Defusing\0Local player\0Weapons\0Hands\0Backtrack\0Sleeves\0"))
        material = 1;

    ImGui::PopID();

    ImGui::SameLine();

    if (material <= 1)
        ImGuiCustom::arrowButtonDisabled("##left", ImGuiDir_Left);
    else if (ImGui::ArrowButton("##left", ImGuiDir_Left))
        --material;

    ImGui::SameLine();
    ImGui::Text("%d", material);
    ImGui::SameLine();

    if (material >= int(config.getFeatures().chams.chamsMaterials[currentCategory].materials.size()))
        ImGuiCustom::arrowButtonDisabled("##right", ImGuiDir_Right);
    else if (ImGui::ArrowButton("##right", ImGuiDir_Right))
        ++material;

    ImGui::SameLine();

    auto& chams{ config.getFeatures().chams.chamsMaterials[currentCategory].materials[material - 1] };

    ImGui::Checkbox("Enabled", &chams.enabled);
    ImGui::Separator();
    ImGui::Checkbox("Health based", &chams.healthBased);
    ImGui::Checkbox("Blinking", &chams.blinking);

    int material_ = static_cast<int>(chams.material);
    ImGui::Combo("Material", &material_, [](void*, int idx, const char** out_text) {
        *out_text = toString(static_cast<ChamsMaterial>(idx)).data();
        return true;
    }, nullptr, ChamsMaterials::numberOfMaterials);
    chams.material = static_cast<ChamsMaterial>(material_);

    ImGui::Checkbox("Wireframe", &chams.wireframe);
    ImGui::Checkbox("Cover", &chams.cover);
    ImGui::Checkbox("Ignore-Z", &chams.ignorez);
    ImGuiCustom::colorPicker("Color", chams.color);

    if (!contentOnly) {
        ImGui::End();
    }
}

void GUI::renderStyleWindow(Config& config, bool contentOnly) noexcept
{
    if (!contentOnly) {
        if (!window.style)
            return;
        ImGui::SetNextWindowSize({ 0.0f, 0.0f });
        ImGui::Begin("Style", &window.style, windowFlags);
    }

    ImGui::PushItemWidth(150.0f);
    if (ImGui::Combo("Menu style", &config.style.menuStyle, "Classic\0One window\0"))
        window = { };
    if (ImGui::Combo("Menu colors", &config.style.menuColors, "Dark\0Light\0Classic\0Custom\0"))
        updateColors(config);
    ImGui::PopItemWidth();

    if (config.style.menuColors == 3) {
        ImGuiStyle& style = ImGui::GetStyle();
        for (int i = 0; i < ImGuiCol_COUNT; i++) {
            if (i && i & 3) ImGui::SameLine(220.0f * (i & 3));

            ImGuiCustom::colorPicker(ImGui::GetStyleColorName(i), (float*)&style.Colors[i], &style.Colors[i].w);
        }
    }

    if (!contentOnly)
        ImGui::End();
}

void GUI::renderConfigWindow(const OtherInterfaces& interfaces, const Memory& memory, Config& config, bool contentOnly) noexcept
{
    if (!contentOnly) {
        if (!window.config)
            return;
        ImGui::SetNextWindowSize({ 320.0f, 0.0f });
        if (!ImGui::Begin("Config", &window.config, windowFlags)) {
            ImGui::End();
            return;
        }
    }

    ImGui::Columns(2, nullptr, false);
    ImGui::SetColumnOffset(1, 170.0f);

    static bool incrementalLoad = false;
    ImGui::Checkbox("Incremental Load", &incrementalLoad);

    ImGui::PushItemWidth(160.0f);

    auto& configItems = config.getConfigs();
    static int currentConfig = -1;

    static std::u8string buffer;

    timeToNextConfigRefresh -= ImGui::GetIO().DeltaTime;
    if (timeToNextConfigRefresh <= 0.0f) {
        config.listConfigs();
        if (const auto it = std::find(configItems.begin(), configItems.end(), buffer); it != configItems.end())
            currentConfig = std::distance(configItems.begin(), it);
        timeToNextConfigRefresh = 0.1f;
    }

    if (static_cast<std::size_t>(currentConfig) >= configItems.size())
        currentConfig = -1;

    if (ImGui::ListBox("", &currentConfig, [](void* data, int idx, const char** out_text) {
        auto& vector = *static_cast<std::vector<std::u8string>*>(data);
        *out_text = (const char*)vector[idx].c_str();
        return true;
        }, &configItems, configItems.size(), 5) && currentConfig != -1)
            buffer = configItems[currentConfig];

        ImGui::PushID(0);
        if (ImGui::InputTextWithHint("", "config name", &buffer, ImGuiInputTextFlags_EnterReturnsTrue)) {
            if (currentConfig != -1)
                config.rename(currentConfig, buffer);
        }
        ImGui::PopID();
        ImGui::NextColumn();

        ImGui::PushItemWidth(100.0f);

        if (ImGui::Button("Open config directory"))
            config.openConfigDir();

        if (ImGui::Button("Create config", { 100.0f, 25.0f }))
            config.add(interfaces, memory, buffer.c_str());

        if (ImGui::Button("Reset config", { 100.0f, 25.0f }))
            ImGui::OpenPopup("Config to reset");

        if (ImGui::BeginPopup("Config to reset")) {
            static constexpr const char* names[]{ "Whole", "Aimbot", "Triggerbot", "Backtrack", "Glow", "Chams", "ESP", "Visuals", "Inventory Changer", "Sound", "Style", "Misc" };
            for (int i = 0; i < IM_ARRAYSIZE(names); i++) {
                if (i == 1) ImGui::Separator();

                ResetConfigurator configurator;

                if (ImGui::Selectable(names[i])) {
                    switch (i) {
                    case 0: config.reset(interfaces, memory); updateColors(config); config.getFeatures().misc.updateClanTag(true); config.getFeatures().inventoryChanger.scheduleHudUpdate(); break;
                    case 1: config.aimbot = { }; break;
                    case 2: config.triggerbot = { }; break;
                    case 3: config.getFeatures().backtrack.configure(configurator); break;
                    case 4: config.getFeatures().glow.resetConfig(); break;
                    case 5: config.getFeatures().chams.configure(configurator); break;
                    case 6: config.streamProofESP = { }; break;
                    case 7: config.getFeatures().visuals.resetConfig(); break;
                    case 8: config.getFeatures().inventoryChanger.reset(memory); config.getFeatures().inventoryChanger.scheduleHudUpdate(); break;
                    case 9: config.getFeatures().sound.configure(configurator); break;
                    case 10: config.style = { }; updateColors(config); break;
                    case 11: config.getFeatures().misc.resetConfig(); config.getFeatures().misc.updateClanTag(true); break;
                    }
                }
            }
            ImGui::EndPopup();
        }
        if (currentConfig != -1) {
            if (ImGui::Button("Load selected", { 100.0f, 25.0f })) {
                config.load(interfaces, memory, currentConfig, incrementalLoad);
                updateColors(config);
                config.getFeatures().inventoryChanger.scheduleHudUpdate();
                config.getFeatures().misc.updateClanTag(true);
            }
            if (ImGui::Button("Save selected", { 100.0f, 25.0f }))
                config.save(interfaces, memory, currentConfig);
            if (ImGui::Button("Delete selected", { 100.0f, 25.0f })) {
                config.remove(currentConfig);

                if (static_cast<std::size_t>(currentConfig) < configItems.size())
                    buffer = configItems[currentConfig];
                else
                    buffer.clear();
            }
        }
        ImGui::Columns(1);
        if (!contentOnly)
            ImGui::End();
}

namespace
{

enum class Tab { Rage, AntiAim, Legit, Players, World, Misc, Skins, Configs };

struct TabInfo {
    Tab tab;
    const char* name;
    const char* subtitle;
    hvh::ui::Icon icon;
};

constexpr TabInfo tabs[]{
    { Tab::Rage, "Рейджбот", "аим, хитшанс, автоволл, DT", hvh::ui::Icon::Rage },
    { Tab::AntiAim, "Антиаимы", "углы, десинк, фейклаг", hvh::ui::Icon::AntiAim },
    { Tab::Legit, "Легитбот", "аимбот, триггер, бектрек", hvh::ui::Icon::Legit },
    { Tab::Players, "Игроки", "ESP, чамсы, глоу — с предпросмотром", hvh::ui::Icon::Players },
    { Tab::World, "Мир", "визуалы, скоуп, гранаты", hvh::ui::Icon::World },
    { Tab::Misc, "Разное", "движение, автопик, фейкдак, звук", hvh::ui::Icon::Misc },
    { Tab::Skins, "Скины", "скины и ножи с предпросмотром", hvh::ui::Icon::Skins },
    { Tab::Configs, "Конфиги", "сохранение и загрузка", hvh::ui::Icon::Configs },
};

Tab currentTab = Tab::Rage;

void sectionLabel(const char* text) noexcept
{
    ImGui::Dummy({ 0.0f, 4.0f });
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(IM_COL32(80, 86, 102, 255)));
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 6.0f);
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
}

// старые вкладки Osiris внутри группы новой темы
template <typename Draw>
void legacyGroup(const char* title, Draw draw) noexcept
{
    if (hvh::ui::beginGroup(title, ImGui::GetContentRegionAvail())) {
        ImGui::PushItemWidth(200.0f);
        draw();
        ImGui::PopItemWidth();
    }
    hvh::ui::endGroup();
}

template <typename Draw>
void legacyGroupSized(const char* title, const ImVec2& size, Draw draw) noexcept
{
    if (hvh::ui::beginGroup(title, size)) {
        ImGui::PushItemWidth(200.0f);
        draw();
        ImGui::PopItemWidth();
    }
    hvh::ui::endGroup();
}

[[nodiscard]] ImU32 toU32(const std::array<float, 4>& c, float alphaScale = 1.0f) noexcept
{
    return ImGui::ColorConvertFloat4ToU32({ c[0], c[1], c[2], c[3] * alphaScale });
}

// Упрощённые чамсы: слой 1 — сквозь стены (цвет 1), слой 2 — видимая часть (цвет 2), один материал
template <typename Advanced>
void simpleChams(Config& config, Advanced advancedUi) noexcept
{
    using namespace hvh::ui;
    static int target = 1;
    if (pill("Враги", target == 1)) target = 1;
    ImGui::SameLine(0.0f, 6.0f);
    if (pill("Союзники", target == 0)) target = 0;
    ImGui::Dummy({ 0.0f, 4.0f });

    auto& layers = config.getFeatures().chams.chamsMaterials[target].materials;
    auto& hidden = layers[0];
    auto& visible = layers[1];

    // два слоя включаются отдельно: раньше общий переключатель показывал «вкл», когда работал только один
    toggle("Сквозь стены", &hidden.enabled);
    toggle("Видимые", &visible.enabled);
    hidden.ignorez = true;
    visible.ignorez = false;

    static constexpr const char* materials[]{ "Обычный", "Плоский", "Анимированный", "Платина", "Стекло", "Кристалл", "Хром", "Серебро", "Золото", "Пластик", "Свечение", "Перламутр", "Металлик" };
    int material = static_cast<int>(visible.material);
    if (combo("Вид чамсов", &material, materials, IM_ARRAYSIZE(materials)))
        hidden.material = visible.material = static_cast<ChamsMaterial>(material);
    hidden.material = visible.material;

    colorEdit("Цвет 1 (за стеной)", hidden.color.color.data());
    colorEdit("Цвет 2 (видимый)", visible.color.color.data());

    ImGui::Dummy({ 0.0f, 6.0f });
    static bool advanced = false;
    toggle("Расширенные слои", &advanced);
    if (advanced) {
        advancedUi();
    }
}

// Предпросмотр: силуэт игрока с текущими настройками ESP врагов и чамсов
void playerPreview(Config& config) noexcept
{
    auto* d = ImGui::GetWindowDrawList();
    const auto origin = ImGui::GetCursorScreenPos();
    const auto avail = ImGui::GetContentRegionAvail();
    const ImVec2 center{ origin.x + avail.x * 0.5f, origin.y + avail.y * 0.5f + 6.0f };

    const auto& esp = config.streamProofESP.enemies["All"];
    const auto& layers = config.getFeatures().chams.chamsMaterials[1].materials;

    // настоящая модель (капсулы хитбоксов из костей игрока), если мы в игре; иначе — нарисованный силуэт
    ImVec2 boxMin, boxMax;
    const ImU32 visibleColor = layers[1].enabled ? toU32(layers[1].color.color) : 0;
    const ImU32 hiddenColor = layers[0].enabled ? toU32(layers[0].color.color) : 0;
    const bool realModel = hvh::preview::draw(d, { origin.x + 8.0f, origin.y + 26.0f }, { origin.x + avail.x - 8.0f, origin.y + avail.y - 26.0f }, visibleColor, hiddenColor, boxMin, boxMax, visibleColor == 0);

    // размеры силуэта (и для скелета ESP в запасном режиме)
    const float h = std::min(avail.y - 90.0f, 330.0f);
    const float top = center.y - h * 0.5f;
    const float unit = h / 8.0f;
    const ImVec2 head{ center.x, top + unit * 0.55f };
    const ImVec2 neck{ center.x, top + unit * 1.15f };
    const ImVec2 pelvis{ center.x, top + unit * 4.0f };
    const float shoulder = unit * 0.95f;

    if (!realModel) {
        // силуэт: голова, торс, руки, ноги
        const auto bodyColor = [&](float shade) {
            if (layers[1].enabled)
                return toU32(layers[1].color.color, shade);
            if (layers[0].enabled)
                return toU32(layers[0].color.color, shade);
            return IM_COL32(70, 76, 92, static_cast<int>(255 * shade));
        };
        const auto limb = [&](ImVec2 a, ImVec2 b, float w, float shade) {
            d->AddLine(a, b, bodyColor(shade), w);
            d->AddCircleFilled(a, w * 0.5f, bodyColor(shade));
            d->AddCircleFilled(b, w * 0.5f, bodyColor(shade));
        };
        // за стеной: полупрозрачная копия со сдвигом, чтобы было видно цвет 1
        if (layers[0].enabled && layers[1].enabled) {
            const ImVec2 off{ unit * 0.35f, 0.0f };
            d->AddCircleFilled({ head.x + off.x, head.y }, unit * 0.5f, toU32(layers[0].color.color, 0.5f));
            d->AddRectFilled({ center.x - shoulder + off.x, neck.y }, { center.x + shoulder + off.x, pelvis.y }, toU32(layers[0].color.color, 0.5f), unit * 0.4f);
        }
        limb({ center.x - shoulder, neck.y + unit * 0.2f }, { center.x - shoulder - unit * 0.3f, top + unit * 3.8f }, unit * 0.42f, 0.85f);
        limb({ center.x + shoulder, neck.y + unit * 0.2f }, { center.x + shoulder + unit * 0.3f, top + unit * 3.8f }, unit * 0.42f, 0.85f);
        limb({ center.x - unit * 0.45f, pelvis.y }, { center.x - unit * 0.6f, top + h }, unit * 0.5f, 0.9f);
        limb({ center.x + unit * 0.45f, pelvis.y }, { center.x + unit * 0.6f, top + h }, unit * 0.5f, 0.9f);
        d->AddRectFilled({ center.x - shoulder, neck.y }, { center.x + shoulder, pelvis.y + unit * 0.2f }, bodyColor(1.0f), unit * 0.4f);
        d->AddCircleFilled(head, unit * 0.5f, bodyColor(1.0f));
        boxMin = { center.x - shoulder - unit * 0.9f, top - unit * 0.15f };
        boxMax = { center.x + shoulder + unit * 0.9f, top + h + unit * 0.3f };
    }

    if (!esp.enabled) {
        const char* text = "ESP врагов выключен";
        const auto size = ImGui::CalcTextSize(text);
        d->AddText({ center.x - size.x * 0.5f, origin.y + avail.y - size.y - 4.0f }, hvh::ui::color::textDim, text);
        ImGui::InvisibleButton("##preview", avail);  // тянуть мышью — поворот модели, окно не двигается
        return;
    }

    const float boxPad = 4.0f;
    boxMin = { boxMin.x - boxPad, boxMin.y - boxPad };
    boxMax = { boxMax.x + boxPad, boxMax.y + boxPad };
    const float boxCenterX = (boxMin.x + boxMax.x) * 0.5f;

    if (esp.box.enabled) {
        const auto col = toU32(esp.box.asColor4().color);
        if (esp.box.fill.enabled)
            d->AddRectFilled(boxMin, boxMax, toU32(esp.box.fill.asColor4().color), esp.box.rounding);
        if (esp.box.type == Box::_2dCorners || esp.box.type == Box::_3dCorners) {
            const float cw = (boxMax.x - boxMin.x) * 0.25f, ch = (boxMax.y - boxMin.y) * 0.15f;
            for (const auto& [p, sx, sy] : { std::tuple{ boxMin, 1.0f, 1.0f }, std::tuple{ ImVec2{ boxMax.x, boxMin.y }, -1.0f, 1.0f }, std::tuple{ ImVec2{ boxMin.x, boxMax.y }, 1.0f, -1.0f }, std::tuple{ boxMax, -1.0f, -1.0f } }) {
                d->AddLine(p, { p.x + cw * sx, p.y }, col, 1.5f);
                d->AddLine(p, { p.x, p.y + ch * sy }, col, 1.5f);
            }
        } else {
            d->AddRect({ boxMin.x - 1, boxMin.y - 1 }, { boxMax.x + 1, boxMax.y + 1 }, IM_COL32(0, 0, 0, 160), esp.box.rounding);
            d->AddRect(boxMin, boxMax, col, esp.box.rounding);
        }
    }
    if (esp.name.enabled) {
        const char* name = "Enemy";
        const auto size = ImGui::CalcTextSize(name);
        d->AddText({ boxCenterX - size.x * 0.5f, boxMin.y - size.y - 3.0f }, toU32(esp.name.asColor4().color), name);
    }
    if (esp.healthBar.enabled) {
        const float x = boxMin.x - 6.0f;
        const float hp = 0.72f;
        d->AddRectFilled({ x - 3.0f, boxMin.y - 1.0f }, { x + 1.0f, boxMax.y + 1.0f }, IM_COL32(0, 0, 0, 170));
        const float y = boxMax.y - (boxMax.y - boxMin.y) * hp;
        const ImU32 col = esp.healthBar.type == HealthBar::Solid ? toU32(esp.healthBar.asColor4().color) : IM_COL32(120, 220, 90, 255);
        d->AddRectFilled({ x - 2.0f, y }, { x, boxMax.y }, col);
    }
    if (esp.weapon.enabled) {
        const char* weapon = "AWP";
        const auto size = ImGui::CalcTextSize(weapon);
        d->AddText({ boxCenterX - size.x * 0.5f, boxMax.y + 3.0f }, toU32(esp.weapon.asColor4().color), weapon);
    }
    if (!realModel && esp.skeleton.asColorToggle().enabled) {
        const auto col = toU32(esp.skeleton.asColorToggle().asColor4().color);
        const float t = esp.skeleton.thickness;
        d->AddLine(head, pelvis, col, t);
        d->AddLine({ center.x - shoulder, neck.y + unit * 0.2f }, { center.x + shoulder, neck.y + unit * 0.2f }, col, t);
        d->AddLine({ center.x - shoulder, neck.y + unit * 0.2f }, { center.x - shoulder - unit * 0.3f, top + unit * 3.8f }, col, t);
        d->AddLine({ center.x + shoulder, neck.y + unit * 0.2f }, { center.x + shoulder + unit * 0.3f, top + unit * 3.8f }, col, t);
        d->AddLine(pelvis, { center.x - unit * 0.6f, top + h }, col, t);
        d->AddLine(pelvis, { center.x + unit * 0.6f, top + h }, col, t);
    }
    ImGui::InvisibleButton("##preview", avail);  // тянуть мышью — поворот модели, окно не двигается
}

int subTabs(std::initializer_list<const char*> names, int& selected) noexcept
{
    int i = 0;
    for (const auto name : names) {
        if (i)
            ImGui::SameLine(0.0f, 6.0f);
        if (hvh::ui::pill(name, selected == i))
            selected = i;
        ++i;
    }
    ImGui::Dummy({ 0.0f, 4.0f });
    return selected;
}

}

void GUI::renderGuiStyle2(const EngineInterfaces& engineInterfaces, const ClientInterfaces& clientInterfaces, const OtherInterfaces& interfaces, const Memory& memory, Config& config) noexcept
{
    using namespace hvh::ui;

    const ImVec2 size{ 920.0f, 620.0f };
    constexpr float sidebarWidth = 210.0f;
    constexpr float headerHeight = 60.0f;

    const auto& display = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);
    ImGui::SetNextWindowPos({ (display.x - size.x) * 0.5f, (display.y - size.y) * 0.5f }, ImGuiCond_FirstUseEver);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, { 0.0f, 0.0f });
    ImGui::PushStyleColor(ImGuiCol_WindowBg, { 0.0f, 0.0f, 0.0f, 0.0f });
    ImGui::Begin("##eclipse", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBackground);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();

    auto* d = ImGui::GetWindowDrawList();
    const auto pos = ImGui::GetWindowPos();
    const ImVec2 max{ pos.x + size.x, pos.y + size.y };

    // мягкая тень и фон
    for (int i = 1; i <= 8; ++i)
        d->AddRect({ pos.x - i * 1.5f, pos.y - i * 1.5f }, { max.x + i * 1.5f, max.y + i * 1.5f }, IM_COL32(0, 0, 0, 36 - i * 4), 10.0f + i * 1.5f, ImDrawFlags_None, 2.0f);
    d->AddRectFilled(pos, max, color::windowBg, 12.0f);
    d->AddRectFilled(pos, { pos.x + sidebarWidth, max.y }, color::sidebarBg, 12.0f, ImDrawFlags_RoundCornersLeft);
    d->AddLine({ pos.x + sidebarWidth, pos.y }, { pos.x + sidebarWidth, max.y }, color::groupLine);
    d->AddLine({ pos.x + sidebarWidth, pos.y + headerHeight }, { max.x, pos.y + headerHeight }, color::groupLine);
    // тонкая акцентная полоса сверху
    d->AddRectFilledMultiColor({ pos.x + sidebarWidth, pos.y }, { max.x - 12.0f, pos.y + 2.0f }, color::accent, color::accent2, color::accent2, color::accent);

    // логотип со свечением
    {
        const char* logo = "ECLIPSE";
        const ImVec2 logoPos{ pos.x + 24.0f, pos.y + 20.0f };
        const float logoSize = 28.0f;
        for (int i = 3; i >= 1; --i)
            d->AddText(hvh::ui::fonts.logo, logoSize, { logoPos.x, logoPos.y + i * 0.5f }, IM_COL32(77, 163, 255, 25 * i), logo);
        d->AddText(hvh::ui::fonts.logo, logoSize, logoPos, color::accent, logo);
        const auto logoWidth = (hvh::ui::fonts.logo ? hvh::ui::fonts.logo : ImGui::GetFont())->CalcTextSizeA(logoSize, FLT_MAX, 0.0f, logo).x;
        d->AddCircleFilled({ logoPos.x + logoWidth + 6.0f, logoPos.y + logoSize - 8.0f }, 3.0f, color::accent2);
        d->AddText({ logoPos.x + 1.0f, logoPos.y + logoSize + 2.0f }, IM_COL32(80, 86, 102, 255), "legacy  ·  hvh");
    }

    // навигация
    ImGui::SetCursorPos({ 12.0f, 86.0f });
    ImGui::PushStyleColor(ImGuiCol_ChildBg, { 0.0f, 0.0f, 0.0f, 0.0f });
    ImGui::BeginChild("##sidebar", { sidebarWidth - 24.0f, size.y - 86.0f - 76.0f }, false, ImGuiWindowFlags_NoScrollbar);
    for (const auto& info : tabs) {
        if (info.tab == Tab::Rage) sectionLabel("AIMBOT");
        if (info.tab == Tab::Players) sectionLabel("VISUALS");
        if (info.tab == Tab::Misc) sectionLabel("MISC");
        if (sidebarTab(info.name, info.icon, currentTab == info.tab))
            currentTab = info.tab;
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();

    // профиль внизу
    {
        const ImVec2 footer{ pos.x + 14.0f, max.y - 62.0f };
        d->AddRectFilled(footer, { pos.x + sidebarWidth - 14.0f, max.y - 14.0f }, IM_COL32(18, 21, 28, 255), 10.0f);
        const ImVec2 avatar{ footer.x + 24.0f, footer.y + 24.0f };
        d->AddCircleFilled(avatar, 15.0f, color::accent);
        d->AddCircleFilled({ avatar.x + 4.0f, avatar.y + 4.0f }, 11.0f, IM_COL32(0, 212, 255, 120));
        const char* user = std::getenv("USER");
        char initial[2]{ static_cast<char>(user && *user ? std::toupper(*user) : 'U'), 0 };
        ImGui::PushFont(hvh::ui::fonts.bold);
        const auto initialSize = ImGui::CalcTextSize(initial);
        d->AddText({ avatar.x - initialSize.x * 0.5f, avatar.y - initialSize.y * 0.5f }, IM_COL32(255, 255, 255, 255), initial);
        d->AddText({ footer.x + 48.0f, footer.y + 8.0f }, color::text, user && *user ? user : "user");
        ImGui::PopFont();
        d->AddCircleFilled({ footer.x + 52.0f, footer.y + 33.0f }, 3.0f, color::good);
        d->AddText({ footer.x + 60.0f, footer.y + 25.0f }, color::textDim, "-insecure");
    }

    // шапка
    const auto& info = tabs[static_cast<int>(currentTab)];
    ImGui::PushFont(hvh::ui::fonts.bold);
    d->AddText({ pos.x + sidebarWidth + 24.0f, pos.y + 13.0f }, color::text, info.name);
    ImGui::PopFont();
    d->AddText({ pos.x + sidebarWidth + 24.0f, pos.y + 32.0f }, color::textDim, info.subtitle);

    // статус справа в шапке
    {
        const bool dtOn = hvh::config.exploits.doubleTap && (!hvh::config.exploits.doubleTapKey.isSet() || hvh::config.exploits.doubleTapKey.isToggled());
        const char* chips[]{ "RAGE", "AA", "DT" };
        const bool states[]{ hvh::config.rage.enabled, hvh::config.antiAim.enabled, dtOn };
        float x = max.x - 24.0f;
        for (int i = 2; i >= 0; --i) {
            const auto textSize = ImGui::CalcTextSize(chips[i]);
            const ImVec2 chipMin{ x - textSize.x - 26.0f, pos.y + 17.0f };
            const ImVec2 chipMax{ x, pos.y + 43.0f };
            d->AddRectFilled(chipMin, chipMax, states[i] ? IM_COL32(77, 163, 255, 40) : color::frame, 13.0f);
            d->AddCircleFilled({ chipMin.x + 11.0f, pos.y + 30.0f }, 3.0f, states[i] ? color::accent : IM_COL32(80, 86, 102, 255));
            d->AddText({ chipMin.x + 19.0f, pos.y + 30.0f - textSize.y * 0.5f }, states[i] ? color::text : color::textDim, chips[i]);
            x = chipMin.x - 8.0f;
        }
    }

    // содержимое вкладки
    ImGui::SetCursorPos({ sidebarWidth + 16.0f, headerHeight + 14.0f });
    ImGui::PushStyleColor(ImGuiCol_ChildBg, { 0.0f, 0.0f, 0.0f, 0.0f });
    ImGui::BeginChild("##content", { size.x - sidebarWidth - 32.0f, size.y - headerHeight - 30.0f }, false, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleColor();

    auto& features = config.getFeatures();
    switch (currentTab) {
    case Tab::Rage:
        hvh::gui::drawRageTab();
        break;
    case Tab::AntiAim:
        hvh::gui::drawAntiAimTab();
        break;
    case Tab::Legit: {
        static int sub = 0;
        switch (subTabs({ "Аимбот", "Триггербот", "Бектрек" }, sub)) {
        case 0: legacyGroup("Аимбот", [&] { renderAimbotWindow(config, true); }); break;
        case 1: legacyGroup("Триггербот", [&] { renderTriggerbotWindow(config, true); }); break;
        case 2: legacyGroup("Бектрек", [&] { features.backtrack.drawGUI(true); }); break;
        }
        break;
    }
    case Tab::Players: {
        static int sub = 0;
        subTabs({ "ESP", "Чамсы", "Глоу" }, sub);
        constexpr float previewWidth = 240.0f;
        const auto avail = ImGui::GetContentRegionAvail();
        const ImVec2 left{ avail.x - previewWidth - 12.0f, avail.y };
        switch (sub) {
        case 0: legacyGroupSized("ESP", left, [&] { StreamProofESP::drawGUI(config, true); }); break;
        case 1: legacyGroupSized("Чамсы", left, [&] { simpleChams(config, [&] { renderChamsWindow(config, true); }); }); break;
        case 2: legacyGroupSized("Глоу", left, [&] { features.glow.drawGUI(true); }); break;
        }
        ImGui::SameLine(0.0f, 12.0f);
        // всегда выбранный агент в базовой позе — и в меню, и в игре
        features.inventoryChanger.updatePreviewAgent(memory);
        hvh::preview::captureMenu();
        if (beginGroup("Предпросмотр", { previewWidth, avail.y }))
            playerPreview(config);
        endGroup();
        break;
    }
    case Tab::World: {
        static int sub = 0;
        switch (subTabs({ "Визуалы", "Скоуп и гранаты" }, sub)) {
        case 0: legacyGroup("Визуалы", [&] { features.visuals.drawGUI(true); }); break;
        case 1: {
            const auto avail = ImGui::GetContentRegionAvail();
            if (beginGroup("Скоуп и гранаты", { (avail.x - 12.0f) * 0.5f, avail.y }))
                hvh::gui::drawWorldGroup();
            endGroup();
            break;
        }
        }
        break;
    }
    case Tab::Misc: {
        static int sub = 0;
        switch (subTabs({ "Основное", "Движение", "Автобай", "Звук" }, sub)) {
        case 0: legacyGroup("Разное", [&] { features.misc.drawGUI(features.visuals, features.inventoryChanger, features.glow, true); }); break;
        case 1: hvh::gui::drawMovementTab(); break;
        case 2: hvh::gui::drawAutoBuyTab(); break;
        case 3: legacyGroup("Звук", [&] { features.sound.drawGUI(true); }); break;
        }
        break;
    }
    case Tab::Skins: {
        static int sub = 0;
        switch (subTabs({ "Скинченджер", "Инвентарь (износ, наклейки)" }, sub)) {
        case 0: legacyGroup("Скины", [&] { features.inventoryChanger.drawSkinChanger(memory); }); break;
        case 1: legacyGroup("Инвентарь", [&] { features.inventoryChanger.drawGUI(memory, true); }); break;
        }
        break;
    }
    case Tab::Configs:
        legacyGroup("Конфиги", [&] { renderConfigWindow(interfaces, memory, config, true); });
        break;
    }

    ImGui::EndChild();
    ImGui::End();
}

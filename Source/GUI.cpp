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
#include "Hacks/HvH/HvhVisuals.h"
#include "Hacks/HvH/ModelPreview.h"

#include "Hooks.h"
#include "GameData.h"

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
    // меню в стиле gamesense — мелкий шрифт рисунка Verdana (DejaVu Sans из неё и вырос); нет файла — обычный
    if (std::filesystem::exists("/usr/share/fonts/TTF/DejaVuSans.ttf"))
        hvh::ui::fonts.menu = io.Fonts->AddFontFromFileTTF("/usr/share/fonts/TTF/DejaVuSans.ttf", 12.0f, nullptr, Helpers::getFontGlyphRanges());
    if (std::filesystem::exists("/usr/share/fonts/TTF/DejaVuSans-Bold.ttf"))
        hvh::ui::fonts.menuBold = io.Fonts->AddFontFromFileTTF("/usr/share/fonts/TTF/DejaVuSans-Bold.ttf", 12.0f, nullptr, Helpers::getFontGlyphRanges());
    // Шрифт меню и HUD — Noto Sans одним семейством (раньше поля Inter не заполнялись вовсе,
    // и меню рисовалось мелким DejaVu вперемешку с жирным Noto 15)
    {
        const auto ranges = Helpers::getFontGlyphRanges();
        const auto load = [&](const char* file, float size) -> ImFont* {
            const std::string path = std::string{ "/usr/share/fonts/noto/" } + file;
            return std::filesystem::exists(path) ? io.Fonts->AddFontFromFileTTF(path.c_str(), size, nullptr, ranges) : nullptr;
        };
        hvh::ui::fonts.ui = load("NotoSans-Medium.ttf", 13.0f);
        hvh::ui::fonts.uiBold = load("NotoSans-Bold.ttf", 13.0f);
        hvh::ui::fonts.uiSmall = load("NotoSans-Medium.ttf", 11.0f);
        hvh::ui::fonts.uiTitle = load("NotoSans-Bold.ttf", 16.0f);
    }
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
    const char* section;
    const char* name;
    hvh::ui::Icon icon;
    const char* const* subs = nullptr;   // подпункты в сайдбаре под выбранной вкладкой
    int subCount = 0;
};

constexpr const char* sectionRage = "Агрессия";
constexpr const char* sectionVisuals = "Визуалы";
constexpr const char* sectionMisc = "Разное";

constexpr const char* subsAntiAim[]{ "Углы", "Билдер", "Десинк", "Дефенсив", "Анти-брут" };
constexpr const char* subsLegit[]{ "Аимбот", "Триггербот", "Бектрек" };
constexpr const char* subsPlayers[]{ "ESP", "Чамсы", "Глоу" };
constexpr const char* subsWorld[]{ "Визуалы", "Прицел и HUD", "Приколюхи", "Мир" };
constexpr const char* subsMisc[]{ "Основное", "Движение", "Автобай", "Звук" };
constexpr const char* subsSkins[]{ "Скинченджер", "Инвентарь" };

constexpr TabInfo tabs[]{
    { Tab::Rage, sectionRage, "Рейджбот", hvh::ui::Icon::Rage },
    { Tab::AntiAim, sectionRage, "Антиаимы", hvh::ui::Icon::AntiAim, subsAntiAim, IM_ARRAYSIZE(subsAntiAim) },
    { Tab::Legit, sectionRage, "Легитбот", hvh::ui::Icon::Legit, subsLegit, IM_ARRAYSIZE(subsLegit) },
    { Tab::Players, sectionVisuals, "Игроки", hvh::ui::Icon::Players, subsPlayers, IM_ARRAYSIZE(subsPlayers) },
    { Tab::World, sectionVisuals, "Мир", hvh::ui::Icon::World, subsWorld, IM_ARRAYSIZE(subsWorld) },
    { Tab::Misc, sectionMisc, "Разное", hvh::ui::Icon::Misc, subsMisc, IM_ARRAYSIZE(subsMisc) },
    { Tab::Skins, sectionMisc, "Скины", hvh::ui::Icon::Skins, subsSkins, IM_ARRAYSIZE(subsSkins) },
    { Tab::Configs, sectionMisc, "Конфиги", hvh::ui::Icon::Configs },
};

// выбранный подпункт каждой вкладки (у AA — своя переменная в HvhGui, её же читает вкладка)
int subPages[IM_ARRAYSIZE(tabs)]{};

int& subPageOf(std::size_t index) noexcept
{
    return tabs[index].tab == Tab::AntiAim ? hvh::gui::antiAimPage() : subPages[index];
}

Tab currentTab = Tab::Rage;

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

// ---------- «Игроки → ESP» на виджетах нового меню ----------
// Основное — для врагов и союзников («All» в настройках Osiris); всё остальное (оружие на земле,
// гранаты, ящики) — старый экран Osiris в карточке «Предметы и объекты».
void espPage(Config& config, float width) noexcept
{
    using namespace hvh::ui;
    static int target = 0;
    beginSegments();
    if (pill("Враги", target == 0)) target = 0;
    ImGui::SameLine(0.0f, 2.0f);
    if (pill("Союзники", target == 1)) target = 1;
    endSegments();
    ImGui::Dummy({ 0.0f, 4.0f });

    auto& esp = config.streamProofESP;
    Player& p = (target == 0 ? esp.enemies : esp.allies)["All"];
    ImGui::PushID(target);

    if (beginGroup("Основное", { width, 0.0f })) {
        toggle("Включить", &p.enabled);
        colorToggle("Бокс", &p.box.enabled, p.box.asColor4().color.data());
        if (p.box.enabled) {
            static constexpr const char* boxTypes[]{ "Рамка", "Уголки", "3D", "3D уголки" };
            combo("Вид бокса", &p.box.type, boxTypes, IM_ARRAYSIZE(boxTypes));
            colorToggle("Заливка", &p.box.fill.enabled, p.box.fill.asColor4().color.data());
            sliderFloat("Скругление", &p.box.rounding, 0.0f, 6.0f, "%.0f");
        }
        colorToggle("Ник", &p.name.enabled, p.name.asColor4().color.data());
        colorToggle("Оружие", &p.weapon.enabled, p.weapon.asColor4().color.data());
        toggle("Полоска хп", &p.healthBar.enabled);
        if (p.healthBar.enabled) {
            static constexpr const char* hpTypes[]{ "Градиент", "Свой цвет", "По хп" };
            combo("Цвет полоски", &p.healthBar.type, hpTypes, IM_ARRAYSIZE(hpTypes));
            if (p.healthBar.type == HealthBar::Solid)
                colorEdit("Цвет", p.healthBar.asColor4().color.data());
        }
    }
    endGroup();
    ImGui::Dummy({ 0.0f, 10.0f });

    if (beginGroup("Модель", { width, 0.0f })) {
        auto& skeleton = p.skeleton.asColorToggle();
        colorToggle("Скелет", &skeleton.enabled, skeleton.asColor4().color.data());
        if (skeleton.enabled)
            sliderFloat("Толщина скелета", &p.skeleton.thickness, 1.0f, 3.0f, "%.1f");
        colorToggle("Бокс головы", &p.headBox.enabled, p.headBox.asColor4().color.data());
        colorToggle("Ослепление", &p.flashDuration.enabled, p.flashDuration.asColor4().color.data());
        auto& snap = p.snapline.asColorToggle();
        colorToggle("Линия до игрока", &snap.enabled, snap.asColor4().color.data());
        if (snap.enabled) {
            static constexpr const char* snapTypes[]{ "Снизу", "Сверху", "От прицела" };
            combo("Откуда линия", &p.snapline.type, snapTypes, IM_ARRAYSIZE(snapTypes));
        }
    }
    endGroup();
    ImGui::Dummy({ 0.0f, 10.0f });

    if (target == 0) {
        hvh::gui::drawEspRageGroup({ width, 0.0f });
        ImGui::Dummy({ 0.0f, 10.0f });
    }

    if (beginGroup("Фильтры", { width, 0.0f })) {
        toggle("Только слышимых", &p.audibleOnly);
        toggle("Только замеченных", &p.spottedOnly);
        sliderFloat("Текст до, м", &p.textCullDistance, 0.0f, 200.0f, p.textCullDistance > 0.0f ? "%.0f" : "везде");
        keybind("Вкл/выкл ESP", esp.toggleKey);
        keybind("ESP на зажатие", esp.holdKey);
    }
    endGroup();
    ImGui::PopID();
    ImGui::Dummy({ 0.0f, 10.0f });

    static bool advanced = false;
    if (beginGroup("Предметы и объекты", { width, 0.0f })) {
        toggle("Показать настройки", &advanced);
        if (advanced) {
            ImGui::Dummy({ 0.0f, 4.0f });
            ImGui::PushItemWidth(170.0f);
            StreamProofESP::drawGUI(config, true);
            ImGui::PopItemWidth();
        }
    }
    endGroup();
}

// ---------- «Игроки → Глоу» ----------
void glowPage(Glow& glow, float width) noexcept
{
    using namespace hvh::ui;
    static constexpr const char* playerNames[]{ "Enemies", "Allies", "Local Player", "Planting", "Defusing" };
    static constexpr const char* playerTitles[]{ "Враги", "Союзники", "Я", "Ставит бомбу", "Дефьюзит" };
    static constexpr const char* styles[]{ "Обычное", "Объём (Rim)", "Контур", "Контур с пульсом" };

    const auto item = [&](const char* title, Glow::GlowItem& g) {
        colorToggle(title, &g.enabled, g.color.data());
        if (g.enabled) {
            ImGui::PushID(title);
            combo("Стиль", &g.style, styles, IM_ARRAYSIZE(styles));
            toggle("Цвет по хп", &g.healthBased);
            ImGui::PopID();
        }
    };

    if (beginGroup("Игроки", { width, 0.0f })) {
        for (int i = 0; i < IM_ARRAYSIZE(playerNames); ++i)
            item(playerTitles[i], glow.playerGlow(playerNames[i]).all);
    }
    endGroup();
    ImGui::Dummy({ 0.0f, 10.0f });

    static constexpr const char* itemNames[]{ "Weapons", "C4", "Planted C4", "Projectiles", "Defuse Kits", "Chickens", "Hostages", "Ragdolls" };
    static constexpr const char* itemTitles[]{ "Оружие на земле", "Бомба", "Бомба (поставлена)", "Гранаты в полёте", "Киты", "Курицы", "Заложники", "Трупы" };
    if (beginGroup("Предметы", { width, 0.0f })) {
        for (int i = 0; i < IM_ARRAYSIZE(itemNames); ++i)
            item(itemTitles[i], glow.glowItem(itemNames[i]));
    }
    endGroup();
    ImGui::Dummy({ 0.0f, 10.0f });

    if (beginGroup("Клавиши", { width, 0.0f })) {
        keybind("Вкл/выкл свечение", glow.toggleKey());
        keybind("Свечение на зажатие", glow.holdKey());
    }
    endGroup();
}

[[nodiscard]] ImU32 toU32(const std::array<float, 4>& c, float alphaScale = 1.0f) noexcept
{
    return ImGui::ColorConvertFloat4ToU32({ c[0], c[1], c[2], c[3] * alphaScale });
}

// Упрощённые чамсы. Слои: 0 — сквозь стены, 1 — видимая часть (вместо обычной модели),
// 2 — свечение поверх видимой, 3 — свечение сквозь стены. У стены и видимой части свой материал.
template <typename Advanced>
void simpleChams(Config& config, Advanced advancedUi) noexcept
{
    using namespace hvh::ui;
    static int target = 1;
    beginSegments();
    if (pill("Враги", target == 1)) target = 1;
    ImGui::SameLine(0.0f, 2.0f);
    if (pill("Союзники", target == 0)) target = 0;
    endSegments();
    ImGui::Dummy({ 0.0f, 4.0f });

    auto& layers = config.getFeatures().chams.chamsMaterials[target].materials;
    auto& hidden = layers[0];
    auto& visible = layers[1];
    auto& glow = layers[2];
    auto& glowHidden = layers[3];
    hidden.ignorez = true;
    visible.ignorez = false;
    glow.ignorez = false;
    glowHidden.ignorez = true;
    glow.material = glowHidden.material = ChamsMaterial::Glow;
    // свечение без видимого слоя рисуется поверх обычной модели, а не вместо неё
    glow.cover = !visible.enabled;

    const auto setColor = [](ChamsLayer& layer, float r, float g, float b, float a) {
        layer.color.color = { r, g, b, a };
        layer.color.rainbow = false;
        layer.healthBased = false;
        layer.wireframe = false;
        layer.blinking = false;
    };
    // Готовые наборы — чтобы не собирать вид руками из 13 материалов
    ImGui::TextDisabled("Пресет");
    beginSegments();
    if (pill("Неон", false)) {
        hidden.enabled = visible.enabled = glow.enabled = true;
        glowHidden.enabled = false;
        hidden.material = ChamsMaterial::Flat;
        visible.material = ChamsMaterial::Metallic;
        setColor(hidden, 0.55f, 0.35f, 1.0f, 0.55f);
        setColor(visible, 0.62f, 0.45f, 1.0f, 1.0f);
        setColor(glow, 0.80f, 0.60f, 1.0f, 0.85f);
    }
    ImGui::SameLine(0.0f, 2.0f);
    if (pill("Мягкий", false)) {
        hidden.enabled = visible.enabled = glow.enabled = true;
        glowHidden.enabled = false;
        hidden.material = ChamsMaterial::Flat;
        visible.material = ChamsMaterial::Normal;
        setColor(hidden, 0.30f, 0.62f, 1.0f, 0.40f);
        setColor(visible, 0.85f, 0.92f, 1.0f, 1.0f);
        setColor(glow, 0.45f, 0.75f, 1.0f, 0.60f);
    }
    ImGui::SameLine(0.0f, 2.0f);
    if (pill("Призрак", false)) {
        hidden.enabled = false;
        visible.enabled = glow.enabled = glowHidden.enabled = true;
        visible.material = ChamsMaterial::Glass;
        setColor(visible, 0.70f, 0.90f, 1.0f, 0.35f);
        setColor(glow, 0.55f, 0.85f, 1.0f, 0.90f);
        setColor(glowHidden, 0.55f, 0.85f, 1.0f, 0.60f);
    }
    ImGui::SameLine(0.0f, 2.0f);
    if (pill("Классика", false)) {
        hidden.enabled = visible.enabled = true;
        glow.enabled = glowHidden.enabled = false;
        hidden.material = visible.material = ChamsMaterial::Flat;
        setColor(hidden, 1.0f, 0.0f, 0.0f, 1.0f);
        setColor(visible, 0.25f, 1.0f, 0.0f, 1.0f);
    }
    endSegments();
    ImGui::Dummy({ 0.0f, 4.0f });

    static constexpr const char* materials[]{ "Обычный", "Плоский", "Анимированный", "Платина", "Стекло", "Кристалл", "Хром", "Серебро", "Золото", "Пластик", "Свечение", "Перламутр", "Металлик" };
    const auto materialCombo = [&](const char* label, ChamsLayer& layer) {
        int material = static_cast<int>(layer.material);
        if (combo(label, &material, materials, IM_ARRAYSIZE(materials)))
            layer.material = static_cast<ChamsMaterial>(material);
    };

    toggle("Сквозь стены", &hidden.enabled);
    if (hidden.enabled) {
        materialCombo("Материал за стеной", hidden);
        colorEdit("Цвет за стеной", hidden.color.color.data());
    }
    toggle("Видимые", &visible.enabled);
    if (visible.enabled) {
        materialCombo("Материал видимых", visible);
        colorEdit("Цвет видимых", visible.color.color.data());
    }
    toggle("Свечение поверх", &glow.enabled);
    if (glow.enabled)
        colorEdit("Цвет свечения (прозрачность = сила)", glow.color.color.data());
    toggle("Свечение сквозь стены", &glowHidden.enabled);
    if (glowHidden.enabled)
        colorEdit("Цвет свечения за стеной", glowHidden.color.color.data());

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

    // флаги HvH рисуются и без обычного ESP — как в игре
    hvh::visuals::drawPreviewFlags(d, { boxMax.x + 6.0f, boxMin.y });

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


}

// Кнопка-поле шапки: текст и шеврон вниз (ширина по тексту, не меньше minWidth). true — нажали
static bool toolbarField(const char* id, const char* text, float minWidth, ImVec2* popupPos) noexcept
{
    using namespace hvh::ui;
    ImFont* font = ImGui::GetFont();
    const float textW = font->CalcTextSizeA(font->FontSize, FLT_MAX, 0.0f, text).x;
    const ImVec2 size{ std::max(minWidth, textW + 46.0f), 30.0f };
    const auto pos = ImGui::GetCursorScreenPos();
    const bool pressed = ImGui::InvisibleButton(id, size);
    const bool hovered = ImGui::IsItemHovered();
    auto* d = ImGui::GetWindowDrawList();
    const ImVec2 max{ pos.x + size.x, pos.y + size.y };
    d->AddRectFilled(pos, max, hovered ? color::frameHover : color::frame, 7.0f);
    d->AddText(font, font->FontSize, { pos.x + 14.0f, pos.y + std::floor((size.y - font->FontSize) * 0.5f) }, color::text, text);
    const ImVec2 c{ max.x - 16.0f, pos.y + size.y * 0.5f };
    const ImU32 col = hovered ? color::text : color::textOff;
    d->AddLine({ c.x - 3.5f, c.y - 1.5f }, { c.x, c.y + 2.0f }, col, 1.5f);
    d->AddLine({ c.x, c.y + 2.0f }, { c.x + 3.5f, c.y - 1.5f }, col, 1.5f);
    if (popupPos)
        *popupPos = { pos.x, max.y + 6.0f };
    return pressed;
}

static void pushPopupStyle() noexcept
{
    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, 10.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, { 6.0f, 6.0f });
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, { 6.0f, 2.0f });
    ImGui::PushStyleVar(ImGuiStyleVar_SelectableTextAlign, { 0.0f, 0.5f });
}

void GUI::renderGuiStyle2(const EngineInterfaces& engineInterfaces, const ClientInterfaces& clientInterfaces, const OtherInterfaces& interfaces, const Memory& memory, Config& config) noexcept
{
    using namespace hvh::ui;

    const ImVec2 size{ 940.0f, 700.0f };
    constexpr float sidebarWidth = 186.0f;
    constexpr float toolbarHeight = 56.0f;
    constexpr float radius = 14.0f;
    constexpr float inset = 4.0f;          // правая панель отступает от края окна

    const auto& display = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);
    ImGui::SetNextWindowPos({ (display.x - size.x) * 0.5f, (display.y - size.y) * 0.5f }, ImGuiCond_FirstUseEver);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, { 0.0f, 0.0f });
    ImGui::PushStyleColor(ImGuiCol_WindowBg, { 0.0f, 0.0f, 0.0f, 0.0f });
    ImGui::Begin("##eclipse", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBackground);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
    if (hvh::ui::fonts.ui)
        ImGui::PushFont(hvh::ui::fonts.ui);
    else if (hvh::ui::fonts.menu)
        ImGui::PushFont(hvh::ui::fonts.menu);

    auto* d = ImGui::GetWindowDrawList();
    const auto pos = ImGui::GetWindowPos();
    const ImVec2 max{ pos.x + size.x, pos.y + size.y };

    // окно (оно же сайдбар) — тёмное стекло; справа отдельная панель посветлее с рамкой
    drawBackdrop(d, pos, max, radius);
    const ImVec2 panelMin{ pos.x + sidebarWidth, pos.y + inset };
    const ImVec2 panelMax{ max.x - inset, max.y - inset };
    d->AddRectFilled(panelMin, panelMax, color::panelBg, radius - inset);
    d->AddRect(panelMin, panelMax, color::groupLine, radius - inset);
    d->AddLine({ panelMin.x + 1.0f, panelMin.y + toolbarHeight }, { panelMax.x - 1.0f, panelMin.y + toolbarHeight }, color::groupLine);

    drawLogo(d, { pos.x + 20.0f, pos.y + 20.0f });
    d->AddLine({ pos.x + 14.0f, pos.y + 66.0f }, { pos.x + sidebarWidth - 14.0f, pos.y + 66.0f }, IM_COL32(255, 255, 255, 14));

    // навигация: разделы, вкладки, у выбранной — её подпункты
    constexpr float profileHeight = 64.0f;
    ImGui::SetCursorPos({ 12.0f, 72.0f });
    ImGui::PushStyleColor(ImGuiCol_ChildBg, { 0.0f, 0.0f, 0.0f, 0.0f });
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, { 0.0f, 0.0f });
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, { 0.0f, 0.0f });
    ImGui::BeginChild("##sidebar", { sidebarWidth - 24.0f, size.y - 72.0f - profileHeight - 6.0f }, false, ImGuiWindowFlags_NoScrollbar);
    const char* section = nullptr;
    std::size_t activeIndex = 0;
    for (std::size_t i = 0; i < std::size(tabs); ++i) {
        const auto& info = tabs[i];
        if (info.section != section) {
            section = info.section;
            sidebarSection(section);
        }
        if (sidebarTab(info.name, info.icon, currentTab == info.tab))
            currentTab = info.tab;
        if (currentTab != info.tab)
            continue;
        activeIndex = i;
        int& sub = subPageOf(i);
        sub = std::clamp(sub, 0, std::max(info.subCount - 1, 0));
        ImGui::PushID(static_cast<int>(i));
        for (int k = 0; k < info.subCount; ++k) {
            if (sidebarSubTab(info.subs[k], sub == k))
                sub = k;
        }
        ImGui::PopID();
    }
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
    const auto& activeInfo = tabs[activeIndex];
    const int currentSub = activeInfo.subCount ? subPageOf(activeIndex) : 0;

    // профиль внизу: ник в игре, вне матча — пользователь системы
    {
        char nick[64] = "";
        {
            GameData::Lock lock;
            if (const auto& local = GameData::local(); local.exists)
                std::snprintf(nick, sizeof(nick), "%s", local.name);
        }
        if (!*nick) {
            const char* user = std::getenv("USER");
            std::snprintf(nick, sizeof(nick), "%s", user && *user ? user : "user");
        }
        drawProfile(d, { pos.x + 12.0f, max.y - profileHeight }, sidebarWidth - 24.0f, nick, "сборка " __DATE__);
    }

    // ---- шапка ----
    {
        static int toolbarConfig = -1;
        static float savedFlash = 0.0f;
        auto& configs = config.getConfigs();
        if (toolbarConfig >= static_cast<int>(configs.size()))
            toolbarConfig = -1;
        const char* configName = toolbarConfig >= 0 ? reinterpret_cast<const char*>(configs[toolbarConfig].c_str()) : "Конфиг";

        // [дискета | имя конфига v] — одно поле из двух кнопок, как в NL
        const float barY = inset + (toolbarHeight - 30.0f) * 0.5f;
        ImGui::SetCursorPos({ sidebarWidth + 14.0f, barY });
        const ImVec2 boxMin = ImGui::GetCursorScreenPos();
        constexpr float saveW = 34.0f, boxW = 176.0f;
        const bool savePressed = ImGui::InvisibleButton("##saveConfig", { saveW, 30.0f });
        const bool saveHovered = ImGui::IsItemHovered();
        if (saveHovered)
            ImGui::SetTooltip(toolbarConfig >= 0 ? "Сохранить конфиг" : "Сначала выбери конфиг");
        ImGui::SameLine(0.0f, 0.0f);
        const bool listPressed = ImGui::InvisibleButton("##configName", { boxW - saveW, 30.0f });
        const bool listHovered = ImGui::IsItemHovered();
        const ImVec2 boxMax{ boxMin.x + boxW, boxMin.y + 30.0f };
        d->AddRectFilled(boxMin, boxMax, color::frame, 7.0f);
        if (saveHovered)
            d->AddRectFilled(boxMin, { boxMin.x + saveW, boxMax.y }, color::frameHover, 7.0f, ImDrawFlags_RoundCornersLeft);
        if (listHovered)
            d->AddRectFilled({ boxMin.x + saveW, boxMin.y }, boxMax, color::frameHover, 7.0f, ImDrawFlags_RoundCornersRight);
        d->AddLine({ boxMin.x + saveW, boxMin.y + 6.0f }, { boxMin.x + saveW, boxMax.y - 6.0f }, color::outline);
        drawIcon(d, Icon::Save, { boxMin.x + saveW * 0.5f, boxMin.y + 15.0f }, 12.0f, saveHovered ? color::text : color::textMid);
        {
            ImFont* font = ImGui::GetFont();
            d->PushClipRect({ boxMin.x + saveW, boxMin.y }, { boxMax.x - 26.0f, boxMax.y }, true);
            d->AddText(font, font->FontSize, { boxMin.x + saveW + 12.0f, boxMin.y + std::floor((30.0f - font->FontSize) * 0.5f) },
                toolbarConfig >= 0 ? color::text : color::textOff, configName);
            d->PopClipRect();
            const ImVec2 c{ boxMax.x - 15.0f, boxMin.y + 15.0f };
            const ImU32 col = listHovered ? color::text : color::textOff;
            d->AddLine({ c.x - 3.5f, c.y - 1.5f }, { c.x, c.y + 2.0f }, col, 1.5f);
            d->AddLine({ c.x, c.y + 2.0f }, { c.x + 3.5f, c.y - 1.5f }, col, 1.5f);
        }
        if (savePressed) {
            if (toolbarConfig >= 0) {
                config.save(interfaces, memory, toolbarConfig);
                savedFlash = 1.6f;
            } else {
                config.listConfigs();
                ImGui::OpenPopup("##configList");
            }
        }
        if (listPressed) {
            config.listConfigs();
            ImGui::OpenPopup("##configList");
        }

        ImGui::SetNextWindowPos({ boxMin.x, boxMax.y + 6.0f });
        ImGui::SetNextWindowSizeConstraints({ boxW, 0.0f }, { 280.0f, 320.0f });
        pushPopupStyle();
        if (ImGui::BeginPopup("##configList")) {
            if (configs.empty())
                ImGui::TextDisabled("Конфигов нет — создай во вкладке «Конфиги»");
            for (int i = 0; i < static_cast<int>(configs.size()); ++i) {
                ImGui::PushID(i);
                const bool current = i == toolbarConfig;
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(current ? color::text : color::textMid));
                if (ImGui::Selectable(reinterpret_cast<const char*>(configs[i].c_str()), current, 0, { 0.0f, 26.0f })) {
                    toolbarConfig = i;
                    config.load(interfaces, memory, i, false);
                    updateColors(config);
                    config.getFeatures().inventoryChanger.scheduleHudUpdate();
                    config.getFeatures().misc.updateClanTag(true);
                }
                ImGui::PopStyleColor();
                ImGui::PopID();
            }
            ImGui::Separator();
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(color::textOff));
            if (ImGui::Selectable("Открыть папку", false, 0, { 0.0f, 24.0f }))
                config.openConfigDir();
            ImGui::PopStyleColor();
            ImGui::EndPopup();
        }
        ImGui::PopStyleVar(4);

        // рейдж: выбор группы оружия — поле рядом с конфигом (как «Global» в NL)
        if (currentTab == Tab::Rage) {
            int& weapon = hvh::gui::rageWeapon();
            weapon = std::clamp(weapon, 0, static_cast<int>(hvh::weaponGroupNames.size()) - 1);
            ImGui::SetCursorPos({ sidebarWidth + 14.0f + boxW + 8.0f, barY });
            ImVec2 popupPos;
            if (toolbarField("##weapon", hvh::weaponGroupNames[weapon], 110.0f, &popupPos))
                ImGui::OpenPopup("##weaponList");
            ImGui::SetNextWindowPos(popupPos);
            ImGui::SetNextWindowSizeConstraints({ 150.0f, 0.0f }, { 260.0f, 360.0f });
            pushPopupStyle();
            if (ImGui::BeginPopup("##weaponList")) {
                for (int i = 0; i < static_cast<int>(hvh::weaponGroupNames.size()); ++i) {
                    ImGui::PushID(i);
                    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(i == weapon ? color::text : color::textMid));
                    if (ImGui::Selectable(hvh::weaponGroupNames[i], i == weapon, 0, { 0.0f, 26.0f }))
                        weapon = i;
                    ImGui::PopStyleColor();
                    ImGui::PopID();
                }
                ImGui::EndPopup();
            }
            ImGui::PopStyleVar(4);
        }

        if (savedFlash > 0.0f) {
            savedFlash -= ImGui::GetIO().DeltaTime;
            const float a = std::min(1.0f, savedFlash * 2.0f);
            d->AddText({ panelMax.x - 90.0f, boxMin.y + 15.0f - ImGui::GetFontSize() * 0.5f },
                ImGui::ColorConvertFloat4ToU32({ 0.43f, 0.80f, 0.47f, a }), "Сохранено");
        } else {
            // справа: на AA — что уходит на сервер, иначе — где мы («Мир / Скоуп и гранаты»)
            char status[128];
            const char* right = status;
            if (currentTab == Tab::AntiAim && hvh::gui::antiAimStatus(status, sizeof(status))) {
            } else if (activeInfo.subCount) {
                std::snprintf(status, sizeof(status), "%s  /  %s", activeInfo.name, activeInfo.subs[currentSub]);
            } else {
                right = activeInfo.name;
            }
            ImFont* font = ImGui::GetFont();
            const float w = font->CalcTextSizeA(font->FontSize, FLT_MAX, 0.0f, right).x;
            d->AddText(font, font->FontSize, { panelMax.x - 18.0f - w, boxMin.y + std::floor((30.0f - font->FontSize) * 0.5f) }, color::textOff, right);
        }
    }

    // ---- содержимое: крутится колёсиком целиком, карточки — по своей высоте ----
    constexpr float pad = 16.0f;
    ImGui::SetCursorPos({ sidebarWidth + pad, inset + toolbarHeight + 10.0f });
    ImGui::PushStyleColor(ImGuiCol_ChildBg, { 0.0f, 0.0f, 0.0f, 0.0f });
    ImGui::BeginChild("##content", { size.x - sidebarWidth - inset - pad * 2.0f, size.y - inset * 2.0f - toolbarHeight - 10.0f - 12.0f }, false, ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleColor();

    auto& features = config.getFeatures();
    const int sub = currentSub;
    switch (currentTab) {
    case Tab::Rage:
        hvh::gui::drawRageTab();
        break;
    case Tab::AntiAim:
        hvh::gui::drawAntiAimTab();
        break;
    case Tab::Legit:
        switch (sub) {
        case 0: legacyGroup("Аимбот", [&] { renderAimbotWindow(config, true); }); break;
        case 1: legacyGroup("Триггербот", [&] { renderTriggerbotWindow(config, true); }); break;
        case 2: legacyGroup("Бектрек", [&] { features.backtrack.drawGUI(true); }); break;
        }
        break;
    case Tab::Players: {
        constexpr float previewWidth = 240.0f;
        const auto avail = ImGui::GetContentRegionAvail();
        const ImVec2 left{ avail.x - previewWidth - 12.0f, avail.y };
        // слева — карточки (крутятся колёсиком), справа — предпросмотр, всегда на месте
        ImGui::BeginChild("##playersLeft", left, false, ImGuiWindowFlags_NoScrollbar);
        switch (sub) {
        case 0: espPage(config, left.x); break;
        case 1: legacyGroup("Чамсы", [&] { simpleChams(config, [&] { renderChamsWindow(config, true); }); }); break;
        case 2: glowPage(features.glow, left.x); break;
        }
        ImGui::EndChild();
        ImGui::SameLine(0.0f, 12.0f);
        // всегда выбранный агент в базовой позе — и в меню, и в игре
        features.inventoryChanger.updatePreviewAgent(memory);
        hvh::preview::captureMenu();
        if (beginGroup("Предпросмотр", { previewWidth, avail.y }))
            playerPreview(config);
        endGroup();
        break;
    }
    case Tab::World:
        switch (sub) {
        case 3: {
            auto& sky = features.visuals.skyboxSettings();
            hvh::gui::drawWorldLookTab(&sky.enabled, &sky.skybox, SkyboxChanger::skyboxList.data(), static_cast<int>(SkyboxChanger::skyboxList.size()));
            break;
        }
        case 0: features.visuals.drawMenuPage(); break;
        case 2: hvh::gui::drawVisualsTab(); break;
        case 1: hvh::gui::drawWorldGroup(); break;
        }
        break;
    case Tab::Misc:
        switch (sub) {
        case 0: legacyGroup("Разное", [&] { features.misc.drawGUI(features.visuals, features.inventoryChanger, features.glow, true); }); break;
        case 1: hvh::gui::drawMovementTab(); break;
        case 2: hvh::gui::drawAutoBuyTab(); break;
        case 3: legacyGroup("Звук", [&] { features.sound.drawGUI(true); }); break;
        }
        break;
    case Tab::Skins:
        switch (sub) {
        case 0: legacyGroup("Скины", [&] { features.inventoryChanger.drawSkinChanger(memory); }); break;
        case 1: legacyGroup("Инвентарь", [&] { features.inventoryChanger.drawGUI(memory, true); }); break;
        }
        break;
    case Tab::Configs:
        legacyGroup("Конфиги", [&] { renderConfigWindow(interfaces, memory, config, true); });
        break;
    }

    ImGui::EndChild();
    if (hvh::ui::fonts.ui || hvh::ui::fonts.menu)
        ImGui::PopFont();
    ImGui::End();
}

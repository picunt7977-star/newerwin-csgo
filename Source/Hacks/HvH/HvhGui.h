#pragma once

#include <imgui/imgui.h>

class KeyBind;

namespace hvh::ui
{

// Палитра меню
namespace color
{
    inline constexpr ImU32 windowBg   = IM_COL32(13, 15, 20, 250);
    inline constexpr ImU32 sidebarBg  = IM_COL32(9, 10, 14, 255);
    inline constexpr ImU32 groupBg    = IM_COL32(18, 21, 28, 255);
    inline constexpr ImU32 groupLine  = IM_COL32(32, 37, 48, 255);
    inline constexpr ImU32 frame      = IM_COL32(28, 32, 42, 255);
    inline constexpr ImU32 frameHover = IM_COL32(36, 41, 54, 255);
    inline constexpr ImU32 text       = IM_COL32(230, 233, 240, 255);
    inline constexpr ImU32 textDim    = IM_COL32(124, 130, 147, 255);
    inline constexpr ImU32 accent     = IM_COL32(77, 163, 255, 255);
    inline constexpr ImU32 accent2    = IM_COL32(0, 212, 255, 255);
    inline constexpr ImU32 good       = IM_COL32(84, 214, 140, 255);
    inline constexpr ImU32 bad        = IM_COL32(255, 92, 108, 255);
}

struct Fonts {
    ImFont* regular = nullptr;
    ImFont* bold = nullptr;
    ImFont* logo = nullptr;
};
inline Fonts fonts;

void applyStyle() noexcept;

// плавное значение 0..1 по id, для анимаций
float animate(ImGuiID id, bool target, float speed = 12.0f) noexcept;
ImU32 lerpColor(ImU32 a, ImU32 b, float t) noexcept;

bool beginGroup(const char* title, const ImVec2& size) noexcept;
void endGroup() noexcept;

bool toggle(const char* label, bool* value) noexcept;
bool sliderInt(const char* label, int* value, int min, int max, const char* format = "%d") noexcept;
bool sliderFloat(const char* label, float* value, float min, float max, const char* format = "%.2f") noexcept;
bool combo(const char* label, int* value, const char* const items[], int count) noexcept;
bool multiCombo(const char* label, int* flags, const char* const items[], int count) noexcept;
void keybind(const char* label, KeyBind& key) noexcept;
bool pill(const char* label, bool selected) noexcept;
bool colorEdit(const char* label, float* rgba) noexcept;

enum class Icon { Rage, AntiAim, Legit, Players, Models, World, Misc, Skins, Configs };
void drawIcon(ImDrawList* drawList, Icon icon, const ImVec2& center, float size, ImU32 col) noexcept;
bool sidebarTab(const char* label, Icon icon, bool selected) noexcept;

}

namespace hvh::gui
{

void updateInput() noexcept;
void drawIndicators(ImDrawList* drawList) noexcept;

void drawRageTab() noexcept;
void drawAntiAimTab() noexcept;
void drawWorldGroup() noexcept;
void drawMovementTab() noexcept;
void drawAutoBuyTab() noexcept;

}

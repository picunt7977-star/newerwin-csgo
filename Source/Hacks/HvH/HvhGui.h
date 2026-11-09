#pragma once

#include <imgui/imgui.h>

class KeyBind;

namespace hvh::ui
{

// Палитра меню
namespace color
{
    // gamesense: серые фоны, двойные рамки (чёрная снаружи, серая внутри), зелёный акцент
    inline constexpr ImU32 windowBg   = IM_COL32(19, 19, 19, 255);
    inline constexpr ImU32 sidebarBg  = IM_COL32(12, 12, 12, 255);
    inline constexpr ImU32 groupBg    = IM_COL32(23, 23, 23, 255);
    inline constexpr ImU32 groupLine  = IM_COL32(44, 44, 44, 255);
    inline constexpr ImU32 outline    = IM_COL32(0, 0, 0, 255);
    inline constexpr ImU32 frame      = IM_COL32(35, 35, 35, 255);
    inline constexpr ImU32 frameHover = IM_COL32(45, 45, 45, 255);
    inline constexpr ImU32 text       = IM_COL32(205, 205, 205, 255);
    inline constexpr ImU32 textDim    = IM_COL32(130, 130, 130, 255);
    inline constexpr ImU32 accent     = IM_COL32(154, 197, 39, 255);
    inline constexpr ImU32 accent2    = IM_COL32(110, 145, 22, 255);   // нижний край градиента акцента
    inline constexpr ImU32 good       = IM_COL32(84, 214, 140, 255);
    inline constexpr ImU32 bad        = IM_COL32(255, 92, 108, 255);
}

struct Fonts {
    ImFont* regular = nullptr;
    ImFont* bold = nullptr;
    ImFont* logo = nullptr;
    ImFont* menu = nullptr;      // мелкий шрифт меню (DejaVu Sans — тот же рисунок, что у Verdana)
    ImFont* menuBold = nullptr;
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
// ватермарка в стиле gamesense (правый верхний угол, рисуется и вне игры)
void drawWatermark(ImDrawList* drawList) noexcept;

void drawRageTab() noexcept;
void drawAntiAimTab() noexcept;
void drawWorldGroup() noexcept;
void drawMovementTab() noexcept;
void drawVisualsTab() noexcept;
// «Мир»: пресеты, цвет стен/пропов/неба, скайбокс (поля SkyboxChanger Osiris), туман, подсветка, искры
void drawWorldLookTab(bool* skyboxEnabled, int* skybox, const char* const* skyboxNames, int skyboxCount) noexcept;
// флаги ESP, точка прицела, скелет при выстреле, Tab — во вкладке «Визуалы» рядом с визуалами Osiris
void drawEspRageGroup(const ImVec2& size) noexcept;
// флаги справа от ESP врага — и в HvH-визуалах, и во вкладке ESP игроков
void drawEspFlagsSetting() noexcept;
void drawAutoBuyTab() noexcept;

}

#pragma once

#include <cstddef>

#include <imgui/imgui.h>

class KeyBind;

namespace hvh::ui
{

// Палитра меню: тёмное стекло поверх размытой игры (как меню NL под CS2), один синий акцент
namespace color
{
    inline constexpr ImU32 windowBg   = IM_COL32(11, 13, 18, 238);     // окно и сайдбар (поверх размытия)
    inline constexpr ImU32 sidebarBg  = IM_COL32(11, 13, 18, 0);       // сайдбар — цвет окна
    inline constexpr ImU32 panelBg    = IM_COL32(15, 17, 23, 236);     // правая панель (шапка + контент)
    inline constexpr ImU32 groupBg    = IM_COL32(20, 23, 30, 250);     // карточка
    inline constexpr ImU32 groupLine  = IM_COL32(34, 38, 49, 255);     // рамка карточки
    inline constexpr ImU32 divider    = IM_COL32(30, 34, 44, 255);     // линия между строками
    inline constexpr ImU32 outline    = IM_COL32(42, 47, 59, 255);     // рамки полей, край окна
    inline constexpr ImU32 frame      = IM_COL32(30, 34, 43, 255);     // поля
    inline constexpr ImU32 frameHover = IM_COL32(37, 42, 53, 255);     // наведение
    inline constexpr ImU32 selected   = IM_COL32(37, 41, 51, 255);     // выбранная плашка
    inline constexpr ImU32 switchOff  = IM_COL32(43, 47, 58, 255);     // выключенный тоггл, дорожка
    inline constexpr ImU32 popupBg    = IM_COL32(17, 20, 28, 250);     // выпадающие списки
    inline constexpr ImU32 text       = IM_COL32(241, 243, 247, 255);
    inline constexpr ImU32 textMid    = IM_COL32(200, 205, 214, 255);
    inline constexpr ImU32 textOff    = IM_COL32(128, 135, 150, 255);  // выключенная функция, вкладка
    inline constexpr ImU32 textDim    = IM_COL32(96, 103, 118, 255);   // подписи разделов
    inline constexpr ImU32 accent     = IM_COL32(77, 124, 246, 255);
    inline constexpr ImU32 accent2    = IM_COL32(77, 124, 246, 255);
    inline constexpr ImU32 accentSoft = IM_COL32(132, 138, 152, 255);  // кнопка выключенного тоггла
    inline constexpr ImU32 accentText = IM_COL32(226, 231, 240, 255);  // значения
    inline constexpr ImU32 good       = IM_COL32(110, 205, 120, 255);
    inline constexpr ImU32 bad        = IM_COL32(235, 87, 87, 255);
}

struct Fonts {
    ImFont* regular = nullptr;
    ImFont* bold = nullptr;
    ImFont* logo = nullptr;
    ImFont* menu = nullptr;      // мелкий шрифт для ESP/таблицы (DejaVu Sans 12)
    ImFont* menuBold = nullptr;
    // Noto Sans из системы: меню и HUD одним семейством
    ImFont* ui = nullptr;        // Medium 13
    ImFont* uiBold = nullptr;    // Bold 13 — заголовки групп
    ImFont* uiSmall = nullptr;   // Medium 11 — разделы сайдбара, клавиши
    ImFont* uiTitle = nullptr;   // Bold 15 — логотип
    ImFont* uiLogo = nullptr;
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
// Сегментный переключатель: pill() между ними (SameLine с отступом 2) — общая подложка под ними
void beginSegments() noexcept;
void endSegments() noexcept;
bool colorEdit(const char* label, float* rgba) noexcept;
// тоггл с квадратом цвета слева от переключателя (клик по квадрату — палитра)
bool colorToggle(const char* label, bool* value, float* rgba) noexcept;

enum class Icon { Rage, AntiAim, Legit, Players, Models, World, Misc, Skins, Configs, User, Speed, Clock, Ping, Save, Keyboard, Chevron };
void drawIcon(ImDrawList* drawList, Icon icon, const ImVec2& center, float size, ImU32 col) noexcept;
bool sidebarTab(const char* label, Icon icon, bool selected) noexcept;
// вложенный пункт сайдбара (подвкладка выбранного раздела)
bool sidebarSubTab(const char* label, bool selected) noexcept;
// подпись раздела в сайдбаре («АГРЕССИЯ», «ВИЗУАЛЫ»)
void sidebarSection(const char* label) noexcept;
// подпись заглавными (кириллица тоже); буфер общий — использовать сразу
const char* upperCaption(const char* text) noexcept;

// Подложка окна: мягкая тень, размытая игра под стеклом, тонировка и рамка
void drawBackdrop(ImDrawList* drawList, const ImVec2& min, const ImVec2& max, float rounding) noexcept;
// Логотип: затмение (диск акцента, перекрытый тёмным) + название и подпись
void drawLogo(ImDrawList* drawList, const ImVec2& pos) noexcept;
// Поле-кнопка шапки: иконка, текст, шевроном вниз. true — нажали
bool selectButton(const char* id, Icon icon, const char* text, float width) noexcept;
// Квадратная кнопка с иконкой (шапка)
bool iconButton(const char* id, Icon icon, const char* tooltip) noexcept;
// Профиль внизу сайдбара: аватар-кружок, ник, подпись
void drawProfile(ImDrawList* drawList, const ImVec2& min, float width, const char* name, const char* caption) noexcept;

}

namespace hvh::gui
{

void updateInput() noexcept;
void drawIndicators(ImDrawList* drawList) noexcept;
// HUD: ватермарка сверху справа и панель клавиш с активными биндами
void drawWatermark(ImDrawList* drawList) noexcept;
void drawHotkeys(ImDrawList* drawList, bool menuOpen) noexcept;

void drawRageTab() noexcept;
// выбранная группа оружия рейджа (выпадашка в шапке) и страница AA (подпункты сайдбара)
int& rageWeapon() noexcept;
int& antiAimPage() noexcept;
// короткая сводка AA для шапки; false — показывать нечего
bool antiAimStatus(char* buffer, std::size_t size) noexcept;
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

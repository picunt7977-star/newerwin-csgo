#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <string>
#include <unordered_map>

#include <imgui/imgui.h>
#include <imgui/imgui_internal.h>

#include "AntiAim.h"
#include "HvhConfig.h"
#include "HvhGui.h"
#include "Movement.h"
#include "Ragebot.h"
#include "Resolver.h"
#include "Defensive.h"
#include "Tickbase.h"
#include "Blur.h"
#include "Binds.h"

// прототипы GL 3 (glCreateShader и т.п.) — как в бэкенде ImGui
#ifndef GL_GLEXT_PROTOTYPES
#define GL_GLEXT_PROTOTYPES
#endif
#include <SDL2/SDL_opengl.h>

#include <InputUtil.h>
#include <CSGO/LocalPlayer.h>
#include <GameData.h>

namespace hvh::ui
{

namespace
{

// Строка настройки 26 px (+4 отступ — шаг 30), поле внутри строки 22 px, скругление полей 5 px
constexpr float rowHeight = 26.0f;
constexpr float fieldH = 22.0f;
constexpr float rounding = 5.0f;
int groupRow = 0;                 // номер строки в текущей карточке — разделитель рисуется со второй

ImVec4 toVec4(ImU32 col) noexcept
{
    return ImGui::ColorConvertU32ToFloat4(col);
}

ImU32 withAlpha(ImU32 col, float alpha) noexcept
{
    const auto a = static_cast<int>(((col >> IM_COL32_A_SHIFT) & 0xFF) * std::clamp(alpha, 0.0f, 1.0f));
    return (col & ~IM_COL32_A_MASK) | (static_cast<ImU32>(a) << IM_COL32_A_SHIFT);
}

ImFont* uiFont() noexcept
{
    return fonts.ui ? fonts.ui : ImGui::GetFont();
}

}

float animate(ImGuiID id, bool target, float speed) noexcept
{
    static std::unordered_map<ImGuiID, float> values;
    auto& value = values[id];
    const float goal = target ? 1.0f : 0.0f;
    value += (goal - value) * std::min(1.0f, ImGui::GetIO().DeltaTime * speed);
    if (std::abs(goal - value) < 0.001f)
        value = goal;
    return value;
}

ImU32 lerpColor(ImU32 a, ImU32 b, float t) noexcept
{
    const auto va = toVec4(a);
    const auto vb = toVec4(b);
    return ImGui::ColorConvertFloat4ToU32({ va.x + (vb.x - va.x) * t, va.y + (vb.y - va.y) * t, va.z + (vb.z - va.z) * t, va.w + (vb.w - va.w) * t });
}

void applyStyle() noexcept
{
    auto& style = ImGui::GetStyle();
    style.WindowRounding = 14.0f;
    style.ChildRounding = 12.0f;
    style.FrameRounding = rounding;
    style.PopupRounding = 10.0f;
    style.GrabRounding = rounding;
    style.ScrollbarRounding = 3.0f;
    style.TabRounding = rounding;
    style.WindowBorderSize = 0.0f;
    style.ChildBorderSize = 0.0f;
    style.PopupBorderSize = 1.0f;
    style.FrameBorderSize = 0.0f;
    style.WindowPadding = { 10.0f, 10.0f };
    style.FramePadding = { 9.0f, 5.0f };
    style.ItemSpacing = { 8.0f, 6.0f };
    style.ItemInnerSpacing = { 6.0f, 4.0f };
    style.ScrollbarSize = 4.0f;
    style.GrabMinSize = 8.0f;

    auto* c = style.Colors;
    c[ImGuiCol_Text] = toVec4(color::textMid);
    c[ImGuiCol_TextDisabled] = toVec4(color::textDim);
    c[ImGuiCol_WindowBg] = toVec4(IM_COL32(13, 15, 22, 250));
    c[ImGuiCol_ChildBg] = { 0, 0, 0, 0 };
    c[ImGuiCol_PopupBg] = toVec4(color::popupBg);
    c[ImGuiCol_Border] = toVec4(color::outline);
    c[ImGuiCol_FrameBg] = toVec4(color::frame);
    c[ImGuiCol_FrameBgHovered] = toVec4(color::frameHover);
    c[ImGuiCol_FrameBgActive] = toVec4(color::frameHover);
    c[ImGuiCol_TitleBg] = toVec4(color::sidebarBg);
    c[ImGuiCol_TitleBgActive] = toVec4(color::sidebarBg);
    c[ImGuiCol_MenuBarBg] = toVec4(color::sidebarBg);
    c[ImGuiCol_ScrollbarBg] = { 0, 0, 0, 0 };
    c[ImGuiCol_ScrollbarGrab] = toVec4(color::selected);
    c[ImGuiCol_ScrollbarGrabHovered] = toVec4(color::textOff);
    c[ImGuiCol_ScrollbarGrabActive] = toVec4(color::accent);
    c[ImGuiCol_CheckMark] = toVec4(color::accent);
    c[ImGuiCol_SliderGrab] = toVec4(color::accent);
    c[ImGuiCol_SliderGrabActive] = toVec4(color::accent);
    c[ImGuiCol_Button] = toVec4(color::frame);
    c[ImGuiCol_ButtonHovered] = toVec4(color::frameHover);
    c[ImGuiCol_ButtonActive] = toVec4(color::selected);
    c[ImGuiCol_Header] = toVec4(withAlpha(color::accent, 0.16f));
    c[ImGuiCol_HeaderHovered] = toVec4(withAlpha(color::accent, 0.12f));
    c[ImGuiCol_HeaderActive] = toVec4(withAlpha(color::accent, 0.22f));
    c[ImGuiCol_Separator] = toVec4(color::divider);
    c[ImGuiCol_Tab] = toVec4(color::frame);
    c[ImGuiCol_TabHovered] = toVec4(color::frameHover);
    c[ImGuiCol_TabActive] = toVec4(color::selected);
    c[ImGuiCol_TextSelectedBg] = toVec4(withAlpha(color::accent, 0.35f));
    c[ImGuiCol_NavHighlight] = toVec4(color::accent);
    c[ImGuiCol_ModalWindowDimBg] = { 0, 0, 0, 0.5f };
}

// Подпись раздела заглавными: «Окно» → «ОКНО». ImGui сам регистр не меняет, а заголовки у нас
// по-русски — латиница и кириллица (а-я, ё) переводятся вручную, остальное как есть.
const char* upperCaption(const char* text) noexcept
{
    static char buffer[128];
    std::size_t out = 0;
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(text); *p && out + 3 < sizeof(buffer);) {
        if (*p >= 'a' && *p <= 'z') {
            buffer[out++] = static_cast<char>(*p - 32);
            ++p;
        } else if (p[0] == 0xD0 && p[1] >= 0xB0 && p[1] <= 0xBF) {          // а-п
            buffer[out++] = static_cast<char>(0xD0);
            buffer[out++] = static_cast<char>(p[1] - 0x20);
            p += 2;
        } else if (p[0] == 0xD1 && p[1] >= 0x80 && p[1] <= 0x8F) {          // р-я
            buffer[out++] = static_cast<char>(0xD0);
            buffer[out++] = static_cast<char>(p[1] + 0x20);
            p += 2;
        } else if (p[0] == 0xD1 && p[1] == 0x91) {                          // ё
            buffer[out++] = static_cast<char>(0xD0);
            buffer[out++] = static_cast<char>(0x81);
            p += 2;
        } else {
            buffer[out++] = static_cast<char>(*p++);
        }
    }
    buffer[out] = '\0';
    return buffer;
}

// Строки внутри карточки разделены тонкой линией — кроме первой
void rowDivider(ImDrawList* d, const ImVec2& pos, float width) noexcept
{
    if (groupRow++ > 0)
        d->AddLine({ pos.x - 2.0f, pos.y - 2.0f }, { pos.x + width + 2.0f, pos.y - 2.0f }, color::divider);
}

// подпись строки слева по центру высоты, обрезанная до `right`
void rowLabel(ImDrawList* d, const ImVec2& pos, float right, const char* label, ImU32 col) noexcept
{
    ImFont* font = uiFont();
    d->PushClipRect(pos, { right, pos.y + rowHeight }, true);
    d->AddText(font, font->FontSize, { pos.x, pos.y + std::floor((rowHeight - font->FontSize) * 0.5f) }, col, label, ImGui::FindRenderedTextEnd(label));
    d->PopClipRect();
}

// Карточка: подпись заглавными над ней, сама — скруглённая, с рамкой в 1 px.
// Высота 0 — по содержимому: меряем в endGroup, со следующего кадра карточка плавно
// подстраивается (раскрылись доп. строки — выросла), прокрутки внутри нет — крутится вся страница.
namespace
{
struct GroupFrame {
    ImGuiID key;
    bool autoHeight;
};
ImVector<GroupFrame> groupStack;
std::unordered_map<ImGuiID, float> groupHeights;
constexpr float groupPadY = 7.0f;
}

bool beginGroup(const char* title, const ImVec2& requestedSize) noexcept
{
    ImFont* captionFont = fonts.uiSmall ? fonts.uiSmall : uiFont();
    constexpr float captionH = 22.0f;

    const ImGuiID key = ImGui::GetID(title);
    const bool autoHeight = requestedSize.y <= 0.0f;
    float height = requestedSize.y;
    if (autoHeight) {
        const auto it = groupHeights.find(key);
        const float target = it != groupHeights.end() ? it->second : rowHeight * 3.0f;
        float* shown = ImGui::GetStateStorage()->GetFloatRef(key ^ 0x6A11u, target);
        *shown += (target - *shown) * std::min(1.0f, ImGui::GetIO().DeltaTime * 18.0f);
        if (std::abs(target - *shown) < 0.5f)
            *shown = target;
        height = captionH + std::floor(*shown);
    }
    groupStack.push_back({ key, autoHeight });

    auto* parentList = ImGui::GetWindowDrawList();
    const auto pos = ImGui::GetCursorScreenPos();
    const ImVec2 size{ requestedSize.x, std::max(captionH + rowHeight + groupPadY * 2.0f, height) };
    parentList->AddText(captionFont, captionFont->FontSize, { pos.x + 12.0f, pos.y + 4.0f }, color::textDim,
        upperCaption(title));
    const ImVec2 cardMin{ pos.x, pos.y + captionH };
    const ImVec2 cardMax{ pos.x + size.x, pos.y + size.y };
    parentList->AddRectFilled(cardMin, cardMax, color::groupBg, 11.0f);
    parentList->AddRect(cardMin, cardMax, color::groupLine, 11.0f);

    ImGui::SetCursorScreenPos(cardMin);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, { 0.0f, 0.0f, 0.0f, 0.0f });
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, { 14.0f, groupPadY });
    const ImGuiWindowFlags flags = ImGuiWindowFlags_AlwaysUseWindowPadding
        | (autoHeight ? ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse : 0);
    const bool open = ImGui::BeginChild(title, { size.x, size.y - captionH }, false, flags);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, { 8.0f, 4.0f });
    ImGui::PushItemWidth(-1.0f);
    groupRow = 0;
    return open;
}

void endGroup() noexcept
{
    ImGui::PopItemWidth();
    ImGui::PopStyleVar();
    ImGui::Dummy({ 0.0f, 1.0f });
    if (!groupStack.empty()) {
        const auto frame = groupStack.back();
        groupStack.pop_back();
        // карточка за краем прокрутки: содержимое не рисовалось — мерить нечего, высота прежняя
        if (frame.autoHeight && !ImGui::GetCurrentWindow()->SkipItems) {
            const auto* window = ImGui::GetCurrentWindow();
            groupHeights[frame.key] = std::ceil(window->DC.CursorMaxPos.y - window->DC.CursorStartPos.y + groupPadY * 2.0f);
        }
    }
    ImGui::EndChild();
}

bool colorEdit(const char* label, float* rgba) noexcept
{
    auto* window = ImGui::GetCurrentWindow();
    if (window->SkipItems)
        return false;

    const auto pos = window->DC.CursorPos;
    const float width = ImGui::GetContentRegionAvail().x;
    rowDivider(window->DrawList, pos, width);
    rowLabel(window->DrawList, pos, pos.x + width - 30.0f, label, color::textMid);

    ImGui::SetCursorScreenPos({ pos.x + width - 18.0f, pos.y + (rowHeight - 18.0f) * 0.5f });
    ImGui::PushID(label);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 5.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, { 0.0f, 0.0f });
    ImGui::PushItemWidth(18.0f);
    const bool changed = ImGui::ColorEdit4("##color", rgba, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel | ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_AlphaPreviewHalf);
    ImGui::PopItemWidth();
    ImGui::PopStyleVar(2);
    ImGui::PopID();
    ImGui::SetCursorScreenPos(pos);
    ImGui::ItemSize(ImRect{ pos, { pos.x + width, pos.y + rowHeight } });
    return changed;
}

// ---- бинды на строки ----
// Метка клавиши справа от подписи: серая — бинд ждёт, акцентная — держится.
static void bindBadge(ImDrawList* d, const ImVec2& pos, const char* label, const binds::Bind* bind) noexcept
{
    if (!bind || !bind->key.isSet())
        return;
    ImFont* font = uiFont();
    ImFont* small = fonts.uiSmall ? fonts.uiSmall : font;
    const float labelW = font->CalcTextSizeA(font->FontSize, FLT_MAX, 0.0f, label, ImGui::FindRenderedTextEnd(label)).x;
    const char* key = bind->key.toString();
    const float keyW = small->CalcTextSizeA(small->FontSize, FLT_MAX, 0.0f, key).x;
    const ImVec2 min{ pos.x + labelW + 7.0f, pos.y + (rowHeight - 15.0f) * 0.5f };
    const ImVec2 max{ min.x + keyW + 10.0f, min.y + 15.0f };
    const ImU32 col = bind->active ? color::accent : color::textOff;
    d->AddRectFilled(min, max, withAlpha(bind->active ? color::accent : color::frame, bind->active ? 0.18f : 1.0f), 4.0f);
    d->AddText(small, small->FontSize, { min.x + 5.0f, min.y + (15.0f - small->FontSize) * 0.5f }, col, key);
}

// ПКМ по строке — окно бинда: клавиша, режим, значение на бинде (тем же виджетом, что строка).
// Только для полей hvh::config; редактор получает float и рисует свой виджет на временной копии.
template <typename Editor>
static void bindMenu(ImGuiID row, const char* label, binds::Type type, void* field, const ImVec2& rowMin, float width, Editor editor) noexcept
{
    if (!binds::bindable(field))
        return;
    binds::Bind* bind = binds::attach(row, label, type, field, false);
    bindBadge(ImGui::GetWindowDrawList(), rowMin, label, bind);

    ImGui::PushID(static_cast<int>(row));
    const bool rowHovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem)
        && ImGui::IsMouseHoveringRect(rowMin, { rowMin.x + width, rowMin.y + rowHeight });
    if (rowHovered && ImGui::GetIO().MouseClicked[1])
        ImGui::OpenPopup("##bind");

    ImGui::SetNextWindowSize({ 290.0f, 0.0f });
    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, 10.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, { 14.0f, 12.0f });
    ImGui::PushStyleColor(ImGuiCol_PopupBg, toVec4(color::popupBg));
    if (ImGui::BeginPopup("##bind")) {
        ImGui::PopStyleColor();
        ImGui::PopStyleVar(2);
        if (!bind)
            bind = binds::attach(row, label, type, field, true);
        if (bind) {
            ImFont* bold = fonts.uiBold ? fonts.uiBold : uiFont();
            ImGui::PushFont(bold);
            ImGui::TextUnformatted(label, ImGui::FindRenderedTextEnd(label));
            ImGui::PopFont();
            ImGui::PushStyleColor(ImGuiCol_Text, toVec4(color::textDim));
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextUnformatted("Пока бинд активен, настройка принимает значение ниже");
            ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();
            ImGui::Dummy({ 0.0f, 4.0f });

            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, { 8.0f, 4.0f });
            const int savedRow = groupRow;
            groupRow = 0;
            keybind("Клавиша", bind->key);
            static constexpr const char* modes[]{ "Зажатие", "Переключение", "Выкл. на зажатие" };
            int mode = static_cast<int>(bind->mode);
            if (combo("Режим", &mode, modes, IM_ARRAYSIZE(modes))) {
                bind->mode = static_cast<binds::Mode>(mode);
                bind->toggled = false;
            }
            float value = bind->value;
            if (editor(value)) {
                bind->value = value;
                if (bind->active)
                    binds::write(*bind, value);
            }
            groupRow = savedRow;
            ImGui::PopStyleVar();

            ImGui::Dummy({ 0.0f, 6.0f });
            ImGui::PushStyleColor(ImGuiCol_Text, toVec4(color::bad));
            ImGui::PushStyleColor(ImGuiCol_Button, toVec4(withAlpha(color::bad, 0.10f)));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, toVec4(withAlpha(color::bad, 0.20f)));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, toVec4(withAlpha(color::bad, 0.30f)));
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.0f);
            if (ImGui::Button("Удалить бинд", { -1.0f, 26.0f })) {
                binds::remove(bind->id);
                ImGui::CloseCurrentPopup();
            }
            ImGui::PopStyleVar();
            ImGui::PopStyleColor(4);
        }
        ImGui::EndPopup();
    } else {
        ImGui::PopStyleColor();
        ImGui::PopStyleVar(2);
    }
    ImGui::PopID();
}

// Переключатель-таблетка: выключен — тёмная дорожка и серая кнопка, включён — акцент и белая.
// При наведении вокруг мягкий ореол акцента.
static void switchBox(ImDrawList* d, const ImVec2& at, float t, float hover) noexcept
{
    constexpr float w = 30.0f, h = 18.0f;
    const ImVec2 max{ at.x + w, at.y + h };
    if (hover > 0.01f)
        d->AddRectFilled({ at.x - 2.0f, at.y - 2.0f }, { max.x + 2.0f, max.y + 2.0f }, withAlpha(color::accent, 0.16f * hover), h * 0.5f + 2.0f);
    d->AddRectFilled(at, max, lerpColor(color::switchOff, color::accent, t), h * 0.5f);
    const float cx = at.x + h * 0.5f + (w - h) * t;
    d->AddCircleFilled({ cx, at.y + h * 0.5f }, 7.0f, lerpColor(color::accentSoft, IM_COL32(250, 251, 253, 255), t), 20);
}

bool toggle(const char* label, bool* value) noexcept
{
    auto* window = ImGui::GetCurrentWindow();
    if (window->SkipItems)
        return false;

    const auto id = window->GetID(label);
    const auto pos = window->DC.CursorPos;
    const float width = ImGui::GetContentRegionAvail().x;
    const ImRect bb{ pos, { pos.x + width, pos.y + rowHeight } };

    ImGui::ItemSize(bb);
    if (!ImGui::ItemAdd(bb, id))
        return false;

    bool hovered, held;
    const bool pressed = ImGui::ButtonBehavior(bb, id, &hovered, &held);
    if (pressed) {
        *value = !*value;
        ImGui::MarkItemEdited(id);
    }

    const float t = animate(id, *value, 18.0f);
    const float h = animate(id ^ 0x5A17u, hovered, 18.0f);
    auto* d = window->DrawList;
    rowDivider(d, pos, width);
    rowLabel(d, pos, bb.Max.x - 40.0f, label, lerpColor(color::textMid, color::text, h));
    switchBox(d, { bb.Max.x - 30.0f, pos.y + (rowHeight - 18.0f) * 0.5f }, t, h);
    bindMenu(id, label, binds::Type::Bool, value, pos, width, [](float& v) {
        bool on = v != 0.0f;
        if (!toggle("Значение", &on))
            return false;
        v = on ? 1.0f : 0.0f;
        return true;
    });
    return pressed;
}

bool colorToggle(const char* label, bool* value, float* rgba) noexcept
{
    auto* window = ImGui::GetCurrentWindow();
    if (window->SkipItems)
        return false;

    const auto pos = window->DC.CursorPos;
    const float width = ImGui::GetContentRegionAvail().x;
    // квадрат цвета — первым: так клик по нему не доходит до переключателя строки
    ImGui::SetCursorScreenPos({ pos.x + width - 30.0f - 10.0f - 18.0f, pos.y + (rowHeight - 18.0f) * 0.5f });
    ImGui::PushID(label);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 5.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, { 0.0f, 0.0f });
    ImGui::PushItemWidth(18.0f);
    ImGui::ColorEdit4("##color", rgba, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel | ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_AlphaPreviewHalf);
    ImGui::PopItemWidth();
    ImGui::PopStyleVar(2);
    ImGui::PopID();
    ImGui::SetCursorScreenPos(pos);
    return toggle(label, value);
}

// Слайдер в одну строку: подпись слева, дорожка и значение в плашке справа
template <typename T>
static bool slider(const char* label, T* value, T min, T max, const char* format) noexcept
{
    auto* window = ImGui::GetCurrentWindow();
    if (window->SkipItems)
        return false;

    const auto id = window->GetID(label);
    const auto pos = window->DC.CursorPos;
    const float width = ImGui::GetContentRegionAvail().x;
    const ImRect bb{ pos, { pos.x + width, pos.y + rowHeight } };

    ImFont* font = uiFont();
    ImFont* small = fonts.uiSmall ? fonts.uiSmall : font;
    char buffer[32];
    // «@hp» — мин. урон: выше 100 это «HP+N». Подпись считается от значения, а не вписывается
    // заранее: в окне бинда тот же формат рисовал число строки, а не бинда («HP+1» при 1)
    if (std::strcmp(format, "@hp") == 0) {
        const int v = static_cast<int>(*value);
        if (v > 100)
            std::snprintf(buffer, sizeof(buffer), "HP+%d", v - 100);
        else
            std::snprintf(buffer, sizeof(buffer), "%d", v);
    } else if constexpr (std::is_integral_v<T>)
        std::snprintf(buffer, sizeof(buffer), format, *value);
    else
        std::snprintf(buffer, sizeof(buffer), format, static_cast<double>(*value));
    const float textW = small->CalcTextSizeA(small->FontSize, FLT_MAX, 0.0f, buffer).x;
    const float pillW = std::max(44.0f, textW + 14.0f);
    const ImVec2 pillMin{ bb.Max.x - pillW, pos.y + (rowHeight - 20.0f) * 0.5f };
    const float trackW = std::clamp(width * 0.3f, 48.0f, 100.0f);
    const float cy = pos.y + rowHeight * 0.5f;
    const ImRect track{ { pillMin.x - 12.0f - trackW, cy - 2.0f }, { pillMin.x - 12.0f, cy + 2.0f } };

    ImGui::ItemSize(bb);
    if (!ImGui::ItemAdd(bb, id))
        return false;

    bool hovered, held;
    ImGui::ButtonBehavior(ImRect{ { track.Min.x - 6.0f, pos.y + 4.0f }, { track.Max.x + 6.0f, bb.Max.y - 4.0f } }, id, &hovered, &held);

    bool changed = false;
    if (held) {
        const float ratio = std::clamp((ImGui::GetIO().MousePos.x - track.Min.x) / track.GetWidth(), 0.0f, 1.0f);
        T newValue;
        if constexpr (std::is_integral_v<T>)
            newValue = static_cast<T>(std::lround(min + ratio * (max - min)));
        else
            newValue = min + ratio * (max - min);
        if (newValue != *value) {
            *value = newValue;
            changed = true;
            ImGui::MarkItemEdited(id);
        }
    }

    auto* d = window->DrawList;
    rowDivider(d, pos, width);
    rowLabel(d, pos, track.Min.x - 10.0f, label, color::textMid);

    // плавное положение кнопки, а не скачок
    const float ratio = std::clamp(static_cast<float>(*value - min) / static_cast<float>(max - min), 0.0f, 1.0f);
    float* shown = ImGui::GetStateStorage()->GetFloatRef(id ^ 0x51D3u, ratio);
    *shown += (ratio - *shown) * std::min(1.0f, ImGui::GetIO().DeltaTime * 20.0f);
    const float fillX = track.Min.x + track.GetWidth() * *shown;
    d->AddRectFilled(track.Min, track.Max, color::switchOff, 2.0f);
    if (fillX > track.Min.x + 0.5f)
        d->AddRectFilled(track.Min, { fillX, track.Max.y }, color::accent, 2.0f);
    const float active = animate(id ^ 0x2B1Cu, hovered || held, 18.0f);
    d->AddCircleFilled({ fillX, cy }, 5.5f + active, IM_COL32(248, 249, 252, 255), 20);

    // Плашка значения — настоящее поле ввода: клик ставит курсор, Enter или клик мимо — принять,
    // Esc — отмена. Раньше плашка была кнопкой, которая включала поле через SetKeyboardFocusHere:
    // в нашем окне фокус не доходил, поле через 3 кадра закрывалось — «нажимаю, и ничего».
    // Пока не редактируем, в поле лежит подпись («60%», «HP+5»); разбор понимает и её.
    static ImGuiID editing = 0;
    static char editText[32];
    const ImRect pillRect{ pillMin, { bb.Max.x, pillMin.y + 20.0f } };
    const bool pillHovered = ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(pillRect.Min, pillRect.Max);
    char shownText[32];
    std::snprintf(shownText, sizeof(shownText), "%s", buffer);
    {
        const auto savedCursor = window->DC.CursorPos;
        const auto savedPrevLine = window->DC.CursorPosPrevLine;
        const auto savedPrevSize = window->DC.PrevLineSize;
        const auto savedMax = window->DC.CursorMaxPos;
        ImGui::SetCursorScreenPos(pillMin);
        const bool isEditing = editing == id;
        // текст по центру плашки, пока не редактируем
        const float padX = isEditing ? 6.0f : std::max(4.0f, (pillW - textW) * 0.5f);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, { padX, std::floor((20.0f - small->FontSize) * 0.5f) });
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
        ImGui::PushStyleColor(ImGuiCol_FrameBg, toVec4(isEditing ? color::frame : (pillHovered ? color::frameHover : withAlpha(color::frame, 0.6f))));
        ImGui::PushStyleColor(ImGuiCol_Border, toVec4(isEditing || pillHovered ? color::accent : color::outline));
        ImGui::PushStyleColor(ImGuiCol_Text, toVec4(color::accentText));
        ImGui::PushFont(small);
        ImGui::SetNextItemWidth(pillW);
        ImGui::PushID(static_cast<int>(id ^ 0x9E11u));
        ImGui::InputText("##edit", isEditing ? editText : shownText, isEditing ? sizeof(editText) : sizeof(shownText),
                         ImGuiInputTextFlags_AutoSelectAll);
        ImGui::PopID();
        const bool activated = ImGui::IsItemActivated();
        const bool deactivated = ImGui::IsItemDeactivated();
        const bool escaped = deactivated && ImGui::IsKeyPressed(ImGui::GetKeyIndex(ImGuiKey_Escape), false);
        ImGui::PopFont();
        ImGui::PopStyleColor(3);
        ImGui::PopStyleVar(3);
        window->DC.CursorPos = savedCursor;
        window->DC.CursorPosPrevLine = savedPrevLine;
        window->DC.PrevLineSize = savedPrevSize;
        window->DC.CursorMaxPos = savedMax;

        if (activated) {
            editing = id;
            std::snprintf(editText, sizeof(editText), "%s", shownText);
        }
        if (deactivated && editing == id) {
            editing = 0;
            if (!escaped) {
                // «HP+5» → 105, «60%» → 60, запятая — как точка
                char text[32];
                std::snprintf(text, sizeof(text), "%s", editText);
                for (char* c = text; *c; ++c)
                    if (*c == ',')
                        *c = '.';
                const char* from = text;
                while (*from == ' ')
                    ++from;
                double base = 0.0;
                if ((from[0] == 'H' || from[0] == 'h') && (from[1] == 'P' || from[1] == 'p')) {
                    base = 100.0;
                    from += 2;
                    while (*from == ' ' || *from == '+')
                        ++from;
                }
                char* end = nullptr;
                const double typed = std::strtod(from, &end);
                if (end != from) {
                    const double result = base + typed;
                    T newValue;
                    if constexpr (std::is_integral_v<T>)
                        newValue = static_cast<T>(std::clamp<long long>(std::llround(result), min, max));
                    else
                        newValue = std::clamp(static_cast<T>(result), min, max);
                    if (newValue != *value) {
                        *value = newValue;
                        changed = true;
                        ImGui::MarkItemEdited(id);
                    }
                }
            }
        }
    }

    bindMenu(id, label, std::is_integral_v<T> ? binds::Type::Int : binds::Type::Float, value, pos, width, [&](float& v) {
        T tmp = static_cast<T>(v);
        if (!slider<T>("Значение", &tmp, min, max, format))
            return false;
        v = static_cast<float>(tmp);
        return true;
    });
    return changed;
}

bool sliderInt(const char* label, int* value, int min, int max, const char* format) noexcept
{
    return slider(label, value, min, max, format);
}

bool sliderFloat(const char* label, float* value, float min, float max, const char* format) noexcept
{
    return slider(label, value, min, max, format);
}

// Выпадающий список: поле с рамкой справа в строке, меню — тёмная карточка со скруглением
static void pushComboStyle() noexcept
{
    ImFont* font = uiFont();
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, { 9.0f, std::floor((fieldH - font->FontSize) * 0.5f) });
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, { 6.0f, 2.0f });
    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, 10.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, { 6.0f, 6.0f });
    ImGui::PushStyleColor(ImGuiCol_FrameBg, toVec4(color::frame));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, toVec4(color::frameHover));
    ImGui::PushStyleColor(ImGuiCol_Button, toVec4(color::frame));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, toVec4(color::frameHover));
    ImGui::PushStyleColor(ImGuiCol_Border, toVec4(color::outline));
    ImGui::PushStyleColor(ImGuiCol_Text, toVec4(color::accentText));
}

// превью в поле: не длиннее maxWidth, иначе обрезка по символам UTF-8 и «...»
static std::string fitPreview(const char* text, float maxWidth) noexcept
{
    ImFont* font = uiFont();
    std::string out = text;
    if (font->CalcTextSizeA(font->FontSize, FLT_MAX, 0.0f, out.c_str()).x <= maxWidth)
        return out;
    while (!out.empty()) {
        do
            out.pop_back();
        while (!out.empty() && (static_cast<unsigned char>(out.back()) & 0xC0) == 0x80);
        while (!out.empty() && (out.back() == ' ' || out.back() == ','))
            out.pop_back();
        if (font->CalcTextSizeA(font->FontSize, FLT_MAX, 0.0f, (out + "...").c_str()).x <= maxWidth)
            break;
    }
    return out + "...";
}

static void popComboStyle() noexcept
{
    ImGui::PopStyleColor(6);
    ImGui::PopStyleVar(6);
}

// Строка с полем справа: рисует разделитель и подпись, ставит курсор на поле, возвращает ширину поля
static float fieldRow(const char* label, ImVec2& rowPos) noexcept
{
    auto* window = ImGui::GetCurrentWindow();
    rowPos = window->DC.CursorPos;
    const float width = ImGui::GetContentRegionAvail().x;
    const float fieldW = std::floor(std::clamp(width * 0.5f, 100.0f, 156.0f));
    rowDivider(window->DrawList, rowPos, width);
    rowLabel(window->DrawList, rowPos, rowPos.x + width - fieldW - 8.0f, label, color::textMid);
    ImGui::SetCursorScreenPos({ rowPos.x + width - fieldW, rowPos.y + (rowHeight - fieldH) * 0.5f });
    return fieldW;
}

static void endFieldRow(const ImVec2& rowPos) noexcept
{
    ImGui::SetCursorScreenPos({ rowPos.x, rowPos.y + rowHeight });
    ImGui::Dummy({ 0.0f, 0.0f });
}

// шеврон «v» у правого края последнего поля
static void fieldChevron(bool open) noexcept
{
    const auto min = ImGui::GetItemRectMin();
    const auto max = ImGui::GetItemRectMax();
    const ImVec2 c{ max.x - 11.0f, (min.y + max.y) * 0.5f };
    const float dir = open ? -1.0f : 1.0f;
    auto* d = ImGui::GetWindowDrawList();
    const ImU32 col = ImGui::IsItemHovered() || open ? color::text : color::textOff;
    d->AddLine({ c.x - 3.5f, c.y - 1.5f * dir }, { c.x, c.y + 2.0f * dir }, col, 1.5f);
    d->AddLine({ c.x, c.y + 2.0f * dir }, { c.x + 3.5f, c.y - 1.5f * dir }, col, 1.5f);
}

bool combo(const char* label, int* value, const char* const items[], int count) noexcept
{
    ImVec2 rowPos;
    const float fieldW = fieldRow(label, rowPos);
    bool changed = false;
    ImGui::PushID(label);
    ImGui::SetNextItemWidth(fieldW);
    pushComboStyle();
    const std::string preview = fitPreview(*value >= 0 && *value < count ? items[*value] : "—", fieldW - 32.0f);
    const bool open = ImGui::BeginCombo("##combo", preview.c_str(), ImGuiComboFlags_NoArrowButton);
    if (open) {
        for (int i = 0; i < count; ++i) {
            const bool isSelected = i == *value;
            ImGui::PushStyleColor(ImGuiCol_Text, toVec4(isSelected ? color::text : color::textMid));
            if (ImGui::Selectable(items[i], isSelected)) {
                *value = i;
                changed = true;
            }
            ImGui::PopStyleColor();
            if (isSelected)
                ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    fieldChevron(open);
    popComboStyle();
    ImGui::PopID();
    endFieldRow(rowPos);
    bindMenu(ImGui::GetID(label), label, binds::Type::Int, value, rowPos, ImGui::GetContentRegionAvail().x, [&](float& v) {
        int tmp = static_cast<int>(v);
        if (!combo("Значение", &tmp, items, count))
            return false;
        v = static_cast<float>(tmp);
        return true;
    });
    return changed;
}

bool multiCombo(const char* label, int* flags, const char* const items[], int count) noexcept
{
    ImVec2 rowPos;
    const float fieldW = fieldRow(label, rowPos);

    std::string preview;
    for (int i = 0; i < count; ++i) {
        if (*flags & (1 << i)) {
            if (!preview.empty())
                preview += ", ";
            preview += items[i];
        }
    }
    if (preview.empty())
        preview = "—";

    bool changed = false;
    ImGui::PushID(label);
    ImGui::SetNextItemWidth(fieldW);
    pushComboStyle();
    preview = fitPreview(preview.c_str(), fieldW - 32.0f);
    const bool open = ImGui::BeginCombo("##multi", preview.c_str(), ImGuiComboFlags_NoArrowButton);
    if (open) {
        for (int i = 0; i < count; ++i) {
            const bool selected = *flags & (1 << i);
            const auto rowStart = ImGui::GetCursorScreenPos();
            ImGui::PushStyleColor(ImGuiCol_Text, toVec4(selected ? color::text : color::textOff));
            if (ImGui::Selectable((std::string{ "      " } + items[i]).c_str(), false, ImGuiSelectableFlags_DontClosePopups)) {
                *flags ^= 1 << i;
                changed = true;
            }
            ImGui::PopStyleColor();
            if (selected) {
                // галочка слева
                auto* d = ImGui::GetWindowDrawList();
                const float cy = rowStart.y + ImGui::GetTextLineHeight() * 0.5f;
                d->AddLine({ rowStart.x + 3.0f, cy }, { rowStart.x + 6.0f, cy + 3.0f }, color::accent, 1.6f);
                d->AddLine({ rowStart.x + 6.0f, cy + 3.0f }, { rowStart.x + 12.0f, cy - 3.0f }, color::accent, 1.6f);
            }
        }
        ImGui::EndCombo();
    }
    fieldChevron(open);
    popComboStyle();
    ImGui::PopID();
    endFieldRow(rowPos);
    bindMenu(ImGui::GetID(label), label, binds::Type::Flags, flags, rowPos, ImGui::GetContentRegionAvail().x, [&](float& v) {
        int tmp = static_cast<int>(v);
        if (!multiCombo("Значение", &tmp, items, count))
            return false;
        v = static_cast<float>(tmp);
        return true;
    });
    return changed;
}

void keybind(const char* label, KeyBind& key) noexcept
{
    auto* window = ImGui::GetCurrentWindow();
    if (window->SkipItems)
        return;

    const auto id = window->GetID(label);
    const auto pos = window->DC.CursorPos;
    const float width = ImGui::GetContentRegionAvail().x;
    const ImRect bb{ pos, { pos.x + width, pos.y + rowHeight } };
    ImFont* small = fonts.uiSmall ? fonts.uiSmall : uiFont();

    const bool listening = ImGui::GetActiveID() == id;
    const char* keyName = listening ? "..." : (key.isSet() ? key.toString() : "—");
    const auto keySize = small->CalcTextSizeA(small->FontSize, FLT_MAX, 0.0f, keyName);
    const float chipW = std::max(keySize.x + 18.0f, 44.0f);
    const ImRect button{ { bb.Max.x - chipW, pos.y + (rowHeight - fieldH) * 0.5f }, { bb.Max.x, pos.y + (rowHeight + fieldH) * 0.5f } };

    ImGui::ItemSize(bb);
    if (!ImGui::ItemAdd(bb, id))
        return;

    bool hovered, held;
    const bool pressed = ImGui::ButtonBehavior(button, id, &hovered, &held);

    if (listening) {
        ImGui::GetCurrentContext()->ActiveIdAllowOverlap = true;
        if ((!hovered && ImGui::GetIO().MouseClicked[0]) || key.setToPressedKey())
            ImGui::ClearActiveID();
    } else if (pressed) {
        ImGui::SetActiveID(id, window);
    }
    // ПКМ — сбросить бинд
    if (hovered && ImGui::GetIO().MouseClicked[1]) {
        key = KeyBind{};
        ImGui::ClearActiveID();
    }

    auto* d = window->DrawList;
    rowDivider(d, pos, width);
    rowLabel(d, pos, button.Min.x - 8.0f, label, color::textMid);
    d->AddRectFilled(button.Min, button.Max, hovered ? color::frameHover : color::frame, 5.0f);
    d->AddRect(button.Min, button.Max, listening ? color::accent : color::outline, 5.0f);
    d->AddText(small, small->FontSize, { (button.Min.x + button.Max.x - keySize.x) * 0.5f, (button.Min.y + button.Max.y - small->FontSize) * 0.5f },
        listening ? color::accent : (key.isSet() ? color::text : color::textOff), keyName);
}

// Подвкладка — сегмент переключателя: выбранная на светлой плашке белым, остальные серым.
// Общую подложку под сегментами рисует subTabs.
bool pill(const char* label, bool selected) noexcept
{
    auto* window = ImGui::GetCurrentWindow();
    const auto id = window->GetID(label);
    ImFont* font = uiFont();
    const auto labelEnd = ImGui::FindRenderedTextEnd(label);
    const auto textSize = font->CalcTextSizeA(font->FontSize, FLT_MAX, 0.0f, label, labelEnd);
    const auto pos = window->DC.CursorPos;
    constexpr float h = 28.0f;
    const ImRect bb{ pos, { pos.x + textSize.x + 28.0f, pos.y + h } };

    ImGui::ItemSize(bb);
    if (!ImGui::ItemAdd(bb, id))
        return false;

    bool hovered, held;
    const bool pressed = ImGui::ButtonBehavior(bb, id, &hovered, &held);
    const float t = animate(id, selected, 16.0f);
    const float hv = animate(id ^ 0x77u, hovered, 18.0f);

    auto* d = window->DrawList;
    if (const float fill = std::max(t, hv * 0.35f); fill > 0.01f)
        d->AddRectFilled(bb.Min, bb.Max, withAlpha(color::selected, fill), 6.0f);
    if (t > 0.01f)
        d->AddRect(bb.Min, bb.Max, withAlpha(color::outline, t), 6.0f);
    d->AddText(font, font->FontSize, { pos.x + 14.0f, pos.y + std::floor((h - textSize.y) * 0.5f) },
        lerpColor(lerpColor(color::textOff, color::textMid, hv), color::text, t), label, labelEnd);
    return pressed;
}

namespace
{
ImVec2 segmentsStart;
}

void beginSegments() noexcept
{
    auto* d = ImGui::GetWindowDrawList();
    d->ChannelsSplit(2);
    d->ChannelsSetCurrent(1);
    segmentsStart = ImGui::GetCursorScreenPos();
    ImGui::SetCursorScreenPos({ segmentsStart.x + 3.0f, segmentsStart.y + 3.0f });
    ImGui::BeginGroup();
}

void endSegments() noexcept
{
    ImGui::EndGroup();
    const auto max = ImGui::GetItemRectMax();
    auto* d = ImGui::GetWindowDrawList();
    d->ChannelsSetCurrent(0);
    d->AddRectFilled(segmentsStart, { max.x + 3.0f, max.y + 3.0f }, withAlpha(color::frame, 0.9f), 9.0f);
    d->AddRect(segmentsStart, { max.x + 3.0f, max.y + 3.0f }, withAlpha(color::outline, 0.7f), 9.0f);
    d->ChannelsMerge();
}

void drawIcon(ImDrawList* d, Icon icon, const ImVec2& c, float s, ImU32 col) noexcept
{
    const float r = s * 0.5f;
    const float th = s > 18.0f ? 1.6f : 1.4f;
    switch (icon) {
    case Icon::Rage: // прицел
        d->AddCircle(c, r * 0.72f, col, 24, th);
        d->AddLine({ c.x, c.y - r }, { c.x, c.y - r * 0.35f }, col, th);
        d->AddLine({ c.x, c.y + r * 0.35f }, { c.x, c.y + r }, col, th);
        d->AddLine({ c.x - r, c.y }, { c.x - r * 0.35f, c.y }, col, th);
        d->AddLine({ c.x + r * 0.35f, c.y }, { c.x + r, c.y }, col, th);
        d->AddCircleFilled(c, 1.6f, col);
        break;
    case Icon::AntiAim: { // щит
        const ImVec2 pts[]{ { c.x, c.y - r }, { c.x + r * 0.85f, c.y - r * 0.6f }, { c.x + r * 0.7f, c.y + r * 0.3f }, { c.x, c.y + r }, { c.x - r * 0.7f, c.y + r * 0.3f }, { c.x - r * 0.85f, c.y - r * 0.6f } };
        d->AddPolyline(pts, 6, col, ImDrawFlags_Closed, th);
        d->AddLine({ c.x, c.y - r * 0.55f }, { c.x, c.y + r * 0.55f }, col, th);
        break;
    }
    case Icon::Legit: // мишень
        d->AddCircle(c, r * 0.9f, col, 24, th);
        d->AddCircle(c, r * 0.45f, col, 20, th);
        d->AddCircleFilled(c, 1.8f, col);
        break;
    case Icon::Players: // человек
        d->AddCircle({ c.x, c.y - r * 0.38f }, r * 0.34f, col, 16, th);
        d->PathArcTo({ c.x, c.y + r * 0.95f }, r * 0.75f, IM_PI * 1.05f, IM_PI * 1.95f, 16);
        d->PathStroke(col, ImDrawFlags_None, th);
        break;
    case Icon::Models: { // куб
        const ImVec2 top{ c.x, c.y - r }, left{ c.x - r * 0.85f, c.y - r * 0.5f }, right{ c.x + r * 0.85f, c.y - r * 0.5f }, mid{ c.x, c.y };
        const ImVec2 bl{ c.x - r * 0.85f, c.y + r * 0.5f }, br{ c.x + r * 0.85f, c.y + r * 0.5f }, bottom{ c.x, c.y + r };
        const ImVec2 outline[]{ top, right, br, bottom, bl, left };
        d->AddPolyline(outline, 6, col, ImDrawFlags_Closed, th);
        d->AddLine(left, mid, col, th);
        d->AddLine(right, mid, col, th);
        d->AddLine(mid, bottom, col, th);
        break;
    }
    case Icon::World: // глобус
        d->AddCircle(c, r * 0.9f, col, 24, th);
        d->AddLine({ c.x - r * 0.9f, c.y }, { c.x + r * 0.9f, c.y }, col, th);
        d->PathArcTo({ c.x + r * 0.9f, c.y }, r * 1.27f, IM_PI * 0.75f, IM_PI * 1.25f, 12);
        d->PathStroke(col, ImDrawFlags_None, th);
        d->PathArcTo({ c.x - r * 0.9f, c.y }, r * 1.27f, -IM_PI * 0.25f, IM_PI * 0.25f, 12);
        d->PathStroke(col, ImDrawFlags_None, th);
        break;
    case Icon::Misc: // ползунки
        for (int i = 0; i < 3; ++i) {
            const float y = c.y - r * 0.6f + i * r * 0.6f;
            d->AddLine({ c.x - r * 0.85f, y }, { c.x + r * 0.85f, y }, col, th);
            d->AddCircleFilled({ c.x + (i == 1 ? r * 0.35f : -r * 0.3f), y }, 2.6f, col);
        }
        break;
    case Icon::Skins: // кисть-палитра
        d->AddCircle(c, r * 0.9f, col, 24, th);
        d->AddCircleFilled({ c.x - r * 0.35f, c.y - r * 0.3f }, 1.8f, col);
        d->AddCircleFilled({ c.x + r * 0.15f, c.y - r * 0.45f }, 1.8f, col);
        d->AddCircleFilled({ c.x + r * 0.45f, c.y + r * 0.05f }, 1.8f, col);
        d->AddCircleFilled({ c.x - r * 0.15f, c.y + r * 0.4f }, 2.6f, col);
        break;
    case Icon::Configs: // папка
        d->AddRect({ c.x - r * 0.9f, c.y - r * 0.45f }, { c.x + r * 0.9f, c.y + r * 0.75f }, col, 2.0f, ImDrawFlags_None, th);
        d->AddLine({ c.x - r * 0.9f, c.y - r * 0.45f }, { c.x - r * 0.9f, c.y - r * 0.75f }, col, th);
        d->AddLine({ c.x - r * 0.9f, c.y - r * 0.75f }, { c.x - r * 0.15f, c.y - r * 0.75f }, col, th);
        d->AddLine({ c.x - r * 0.15f, c.y - r * 0.75f }, { c.x + r * 0.05f, c.y - r * 0.45f }, col, th);
        break;
    // мелкие иконки HUD и шапки — те же, что у NewerWin (ClientIcon)
    case Icon::User: // голова и плечи
        d->AddCircle({ c.x, c.y - r * 0.35f }, r * 0.38f, col, 16, th);
        d->PathArcTo({ c.x, c.y + r * 1.0f }, r * 0.72f, IM_PI * 1.1f, IM_PI * 1.9f, 14);
        d->PathStroke(col, ImDrawFlags_None, th);
        break;
    case Icon::Speed: // спидометр
        d->PathArcTo({ c.x, c.y + r * 0.3f }, r * 0.9f, IM_PI * 1.0f, IM_PI * 2.0f, 18);
        d->PathStroke(col, ImDrawFlags_None, th);
        d->AddLine({ c.x - r * 0.9f, c.y + r * 0.3f }, { c.x + r * 0.9f, c.y + r * 0.3f }, col, th);
        d->AddLine({ c.x, c.y + r * 0.3f }, { c.x + r * 0.45f, c.y - r * 0.25f }, col, th);
        break;
    case Icon::Clock:
        d->AddCircle(c, r * 0.9f, col, 20, th);
        d->AddLine(c, { c.x, c.y - r * 0.55f }, col, th);
        d->AddLine(c, { c.x + r * 0.4f, c.y + r * 0.1f }, col, th);
        break;
    case Icon::Ping: { // компас — как иконка координат у NewerWin
        d->AddCircle(c, r * 0.9f, col, 20, th);
        const ImVec2 pts[]{ { c.x + r * 0.42f, c.y - r * 0.42f }, { c.x + r * 0.12f, c.y + r * 0.12f }, { c.x - r * 0.42f, c.y + r * 0.42f }, { c.x - r * 0.12f, c.y - r * 0.12f } };
        d->AddPolyline(pts, 4, col, ImDrawFlags_Closed, th);
        break;
    }
    case Icon::Save: // дискета
        d->AddRect({ c.x - r * 0.8f, c.y - r * 0.8f }, { c.x + r * 0.8f, c.y + r * 0.8f }, col, 2.0f, ImDrawFlags_None, th);
        d->AddRect({ c.x - r * 0.4f, c.y - r * 0.8f }, { c.x + r * 0.35f, c.y - r * 0.3f }, col, 0.0f, ImDrawFlags_None, th);
        d->AddRect({ c.x - r * 0.45f, c.y + r * 0.15f }, { c.x + r * 0.45f, c.y + r * 0.8f }, col, 0.0f, ImDrawFlags_None, th);
        break;
    case Icon::Keyboard: // плашка-клавиатура, как значок «Server Binds»
        d->AddRectFilled({ c.x - r * 0.95f, c.y - r * 0.6f }, { c.x + r * 0.95f, c.y + r * 0.6f }, col, 2.0f);
        d->AddRectFilled({ c.x - r * 0.7f, c.y - r * 0.38f }, { c.x + r * 0.7f, c.y + r * 0.38f }, withAlpha(IM_COL32(0, 0, 0, 255), 0.45f), 1.0f);
        break;
    case Icon::Chevron: // «>»
        d->AddLine({ c.x - r * 0.25f, c.y - r * 0.5f }, { c.x + r * 0.25f, c.y }, col, th);
        d->AddLine({ c.x + r * 0.25f, c.y }, { c.x - r * 0.25f, c.y + r * 0.5f }, col, th);
        break;
    }
}

// Пункт сайдбара: иконка + название. Выбранный — на светлой плашке, иконка акцентом;
// наведение — плашка вполсилы. Переходы плавные.
bool sidebarTab(const char* label, Icon icon, bool selected) noexcept
{
    auto* window = ImGui::GetCurrentWindow();
    const auto id = window->GetID(label);
    const auto pos = window->DC.CursorPos;
    const float width = ImGui::GetContentRegionAvail().x;
    constexpr float h = 32.0f;
    const ImRect bb{ pos, { pos.x + width, pos.y + h } };

    ImGui::ItemSize(ImRect{ pos, { bb.Max.x, bb.Max.y + 2.0f } });
    if (!ImGui::ItemAdd(bb, id))
        return false;

    bool hovered, held;
    const bool pressed = ImGui::ButtonBehavior(bb, id, &hovered, &held);
    const float t = animate(id, selected, 16.0f);
    const float hv = animate(id ^ 0x3C1Au, hovered, 18.0f);

    auto* d = window->DrawList;
    if (const float fill = std::max(t, hv * 0.45f); fill > 0.01f)
        d->AddRectFilled(bb.Min, bb.Max, withAlpha(color::selected, fill), 7.0f);
    const ImU32 textCol = lerpColor(lerpColor(color::textOff, color::textMid, hv), color::text, t);
    drawIcon(d, icon, { bb.Min.x + 19.0f, bb.Min.y + h * 0.5f }, 14.0f, lerpColor(textCol, color::accent, t));
    ImFont* font = uiFont();
    d->AddText(font, font->FontSize, { bb.Min.x + 38.0f, bb.Min.y + std::floor((h - font->FontSize) * 0.5f) }, textCol, label, ImGui::FindRenderedTextEnd(label));
    return pressed;
}

bool sidebarSubTab(const char* label, bool selected) noexcept
{
    auto* window = ImGui::GetCurrentWindow();
    const auto id = window->GetID(label);
    const auto pos = window->DC.CursorPos;
    const float width = ImGui::GetContentRegionAvail().x;
    constexpr float h = 30.0f, indentX = 18.0f;
    const ImRect bb{ { pos.x + indentX, pos.y }, { pos.x + width, pos.y + h } };

    ImGui::ItemSize(ImRect{ pos, { bb.Max.x, bb.Max.y + 2.0f } });
    if (!ImGui::ItemAdd(bb, id))
        return false;

    bool hovered, held;
    const bool pressed = ImGui::ButtonBehavior(bb, id, &hovered, &held);
    const float t = animate(id, selected, 16.0f);
    const float hv = animate(id ^ 0x3C1Bu, hovered, 18.0f);

    auto* d = window->DrawList;
    if (const float fill = std::max(t, hv * 0.45f); fill > 0.01f)
        d->AddRectFilled(bb.Min, bb.Max, withAlpha(color::selected, fill), 7.0f);
    const ImU32 textCol = lerpColor(lerpColor(color::textOff, color::textMid, hv), color::text, t);
    const ImVec2 dot{ bb.Min.x + 16.0f, bb.Min.y + h * 0.5f };
    d->AddCircle(dot, 3.5f, lerpColor(textCol, color::accent, t), 16, 1.4f);
    if (t > 0.01f)
        d->AddCircleFilled(dot, 2.0f, withAlpha(color::accent, t), 12);
    ImFont* font = uiFont();
    d->AddText(font, font->FontSize, { bb.Min.x + 30.0f, bb.Min.y + std::floor((h - font->FontSize) * 0.5f) }, textCol, label, ImGui::FindRenderedTextEnd(label));
    return pressed;
}

void sidebarSection(const char* label) noexcept
{
    auto* window = ImGui::GetCurrentWindow();
    const auto pos = window->DC.CursorPos;
    ImFont* font = fonts.uiSmall ? fonts.uiSmall : uiFont();
    constexpr float h = 28.0f;
    ImGui::ItemSize(ImRect{ pos, { pos.x + 10.0f, pos.y + h } });
    window->DrawList->AddText(font, font->FontSize, { pos.x + 12.0f, pos.y + h - font->FontSize - 7.0f }, color::textDim, upperCaption(label));
}

void drawBackdrop(ImDrawList* d, const ImVec2& min, const ImVec2& max, float r) noexcept
{
    // тень: несколько расходящихся контуров с затуханием
    for (int i = 1; i <= 10; ++i) {
        const float grow = static_cast<float>(i) * 2.0f;
        const float a = 0.10f * (1.0f - static_cast<float>(i) / 11.0f);
        d->AddRect({ min.x - grow, min.y - grow + 4.0f }, { max.x + grow, max.y + grow + 4.0f },
            withAlpha(IM_COL32(0, 0, 0, 255), a), r + grow, ImDrawFlags_None, 2.0f);
    }
    // размытая игра под окном; без неё (меню в главном меню игры, нет FBO) — просто плотнее тонировка
    blur::request();
    const ImTextureID blurred = blur::texture();
    if (blurred) {
        ImVec2 uv0, uv1;
        blur::uv(min, max, uv0, uv1);
        d->AddImageRounded(blurred, min, max, uv0, uv1, IM_COL32_WHITE, r);
        d->AddRectFilled(min, max, color::windowBg, r);
    } else {
        d->AddRectFilled(min, max, (color::windowBg & ~IM_COL32_A_MASK) | IM_COL32(0, 0, 0, 248), r);
    }
    // лёгкий блик сверху и рамка
    d->AddRectFilledMultiColor({ min.x + r, min.y + 1.0f }, { max.x - r, min.y + 60.0f },
        IM_COL32(255, 255, 255, 6), IM_COL32(255, 255, 255, 6), IM_COL32(255, 255, 255, 0), IM_COL32(255, 255, 255, 0));
    d->AddRect(min, max, withAlpha(IM_COL32(255, 255, 255, 255), 0.07f), r);
}

void drawLogo(ImDrawList* d, const ImVec2& pos) noexcept
{
    // затмение: диск акцента, сверху сдвинутый тёмный — остаётся серп, вокруг слабое свечение
    const ImVec2 c{ pos.x + 16.0f, pos.y + 16.0f };
    d->AddCircleFilled(c, 9.0f, color::accent, 32);
    d->AddCircleFilled({ c.x + 3.5f, c.y - 2.5f }, 7.6f, IM_COL32(12, 14, 19, 255), 32);

    ImFont* title = fonts.uiTitle ? fonts.uiTitle : uiFont();
    ImFont* small = fonts.uiSmall ? fonts.uiSmall : uiFont();
    d->AddText(title, title->FontSize, { pos.x + 34.0f, pos.y + 1.0f }, color::text, "ECLIPSE");
    d->AddText(small, small->FontSize, { pos.x + 34.0f, pos.y + 19.0f }, color::textDim, "Counter-Strike: GO");
}

bool selectButton(const char* idText, Icon icon, const char* text, float width) noexcept
{
    auto* window = ImGui::GetCurrentWindow();
    const auto id = window->GetID(idText);
    const auto pos = window->DC.CursorPos;
    constexpr float h = 30.0f;
    const ImRect bb{ pos, { pos.x + width, pos.y + h } };
    ImGui::ItemSize(bb);
    if (!ImGui::ItemAdd(bb, id))
        return false;
    bool hovered, held;
    const bool pressed = ImGui::ButtonBehavior(bb, id, &hovered, &held);
    const float hv = animate(id, hovered, 18.0f);

    auto* d = window->DrawList;
    d->AddRectFilled(bb.Min, bb.Max, lerpColor(color::frame, color::frameHover, hv), 7.0f);
    d->AddRect(bb.Min, bb.Max, color::outline, 7.0f);
    drawIcon(d, icon, { bb.Min.x + 16.0f, bb.Min.y + h * 0.5f }, 12.0f, color::accent);
    ImFont* font = uiFont();
    d->PushClipRect(bb.Min, { bb.Max.x - 26.0f, bb.Max.y }, true);
    d->AddText(font, font->FontSize, { bb.Min.x + 30.0f, bb.Min.y + std::floor((h - font->FontSize) * 0.5f) }, color::text, text);
    d->PopClipRect();
    const ImVec2 c{ bb.Max.x - 14.0f, bb.Min.y + h * 0.5f };
    const ImU32 col = lerpColor(color::textOff, color::text, hv);
    d->AddLine({ c.x - 3.5f, c.y - 1.5f }, { c.x, c.y + 2.0f }, col, 1.5f);
    d->AddLine({ c.x, c.y + 2.0f }, { c.x + 3.5f, c.y - 1.5f }, col, 1.5f);
    return pressed;
}

bool iconButton(const char* idText, Icon icon, const char* tip) noexcept
{
    auto* window = ImGui::GetCurrentWindow();
    const auto id = window->GetID(idText);
    const auto pos = window->DC.CursorPos;
    constexpr float s = 30.0f;
    const ImRect bb{ pos, { pos.x + s, pos.y + s } };
    ImGui::ItemSize(bb);
    if (!ImGui::ItemAdd(bb, id))
        return false;
    bool hovered, held;
    const bool pressed = ImGui::ButtonBehavior(bb, id, &hovered, &held);
    const float hv = animate(id, hovered, 18.0f);

    auto* d = window->DrawList;
    d->AddRectFilled(bb.Min, bb.Max, lerpColor(color::frame, color::frameHover, hv), 7.0f);
    d->AddRect(bb.Min, bb.Max, color::outline, 7.0f);
    drawIcon(d, icon, { bb.Min.x + s * 0.5f, bb.Min.y + s * 0.5f }, 12.0f, held ? color::accent : lerpColor(color::textMid, color::text, hv));
    if (hovered && tip)
        ImGui::SetTooltip("%s", tip);
    return pressed;
}

void drawProfile(ImDrawList* d, const ImVec2& min, float width, const char* name, const char* caption) noexcept
{
    d->AddLine({ min.x, min.y }, { min.x + width, min.y }, withAlpha(IM_COL32(255, 255, 255, 255), 0.06f));
    const ImVec2 c{ min.x + 26.0f, min.y + 30.0f };
    d->AddCircleFilled(c, 15.0f, color::frameHover, 32);
    d->AddCircle(c, 15.0f, withAlpha(color::accent, 0.55f), 32, 1.2f);
    drawIcon(d, Icon::User, { c.x, c.y + 1.0f }, 14.0f, color::textMid);
    // точка «в сети»
    d->AddCircleFilled({ c.x + 10.5f, c.y + 10.5f }, 4.5f, IM_COL32(13, 16, 24, 255), 16);
    d->AddCircleFilled({ c.x + 10.5f, c.y + 10.5f }, 3.0f, color::good, 16);

    ImFont* font = fonts.uiBold ? fonts.uiBold : uiFont();
    ImFont* small = fonts.uiSmall ? fonts.uiSmall : uiFont();
    d->PushClipRect({ min.x, min.y }, { min.x + width - 6.0f, min.y + 60.0f }, true);
    d->AddText(font, font->FontSize, { c.x + 24.0f, c.y - font->FontSize + 1.0f }, color::text, name);
    d->AddText(small, small->FontSize, { c.x + 24.0f, c.y + 3.0f }, color::textDim, caption);
    d->PopClipRect();
    drawIcon(d, Icon::Chevron, { min.x + width - 10.0f, c.y }, 14.0f, color::textOff);
}

}

namespace hvh::gui
{

using namespace hvh::ui;

void updateInput() noexcept
{
    binds::update();
    antiaim::updateInput();
    config.exploits.doubleTapKey.handleToggle();
}

void drawIndicators(ImDrawList* drawList) noexcept
{
    if (!localPlayer || !localPlayer.get().isAlive())
        return;

    const auto& display = ImGui::GetIO().DisplaySize;
    const ImVec2 center{ display.x * 0.5f, display.y * 0.5f };

    // ровные тонкие линии вместо игрового скоупа (он спрятан и не плывёт при движении)
    if (config.view.removeScopeZoom && !config.view.customScope && localPlayer.get().isScoped()) {
        drawList->AddLine({ 0.0f, center.y }, { display.x, center.y }, IM_COL32(0, 0, 0, 220), 1.0f);
        drawList->AddLine({ center.x, 0.0f }, { center.x, display.y }, IM_COL32(0, 0, 0, 220), 1.0f);
    }

    // стрелки ручного антиаима
    if (config.antiAim.enabled) {
        const auto manual = antiaim::manualDirection();
        const auto arrow = [drawList](ImVec2 tip, float dir, bool active) {
            const ImU32 col = active ? color::accent : IM_COL32(255, 255, 255, 40);
            drawList->AddTriangleFilled(tip, { tip.x - dir * 12.0f, tip.y - 8.0f }, { tip.x - dir * 12.0f, tip.y + 8.0f }, col);
        };
        arrow({ center.x - 50.0f, center.y }, -1.0f, manual == antiaim::Manual::Left);
        arrow({ center.x + 50.0f, center.y }, 1.0f, manual == antiaim::Manual::Right);
        // вперёд/назад — только когда выбраны, чтобы не загромождать прицел
        if (manual == antiaim::Manual::Forward)
            drawList->AddTriangleFilled({ center.x, center.y - 50.0f }, { center.x - 8.0f, center.y - 38.0f }, { center.x + 8.0f, center.y - 38.0f }, color::accent);
        else if (manual == antiaim::Manual::Back)
            drawList->AddTriangleFilled({ center.x, center.y + 62.0f }, { center.x + 8.0f, center.y + 50.0f }, { center.x - 8.0f, center.y + 50.0f }, color::accent);
    }

    // список под прицелом
    float y = center.y + 24.0f;
    const auto line = [&](const char* text, ImU32 col) {
        const auto size = ImGui::CalcTextSize(text);
        drawList->AddText({ center.x - size.x * 0.5f + 1.0f, y + 1.0f }, IM_COL32(0, 0, 0, 160), text);
        drawList->AddText({ center.x - size.x * 0.5f, y }, col, text);
        y += size.y + 1.0f;
    };

    if (movement::isFakeDucking()) {
        // FD присед на отправке / высота выстрела: хорошо — ~100% / ниже 40%
        float sent = -1.0f, low = -1.0f;
        movement::fakeDuckStats(sent, low);
        char text[48];
        if (sent >= 0.0f)
            std::snprintf(text, sizeof(text), "FD  %.0f%% / %.0f%%  %dт", sent * 100.0f, low * 100.0f, tickbase::fakeDuckChoke());
        else
            std::snprintf(text, sizeof(text), "FD");
        line(text, sent >= 0.85f ? color::good : color::bad);
    }
    else if (config.exploits.doubleTap && (!config.exploits.doubleTapKey.isSet() || config.exploits.doubleTapKey.isToggled()))
    {
        // В дефенсивном режиме DT заряд тратить некому, если дефенсив копит чоком, — показываем
        // тусклым, чтобы пустой заряд не читался как поломка
        const bool idle = tickbase::defensiveDoubleTap() && defensive::enabled() && defensive::chargeByChoke();
        line(tickbase::defensiveDoubleTap() ? "DT · DEF" : "DT",
            idle ? color::textDim : (tickbase::isCharged() || tickbase::isDefensiveRecharging() ? color::good : color::bad));
    }
    if (antiaim::isFreestanding())
        line("FS", color::accent);
    if (defensive::enabled() && !movement::isFakeDucking()) {
        // Словами, а не процентом: при заряде чоком «процент» почти всегда 0, и красный «DEF 0%»
        // читается как поломка, хотя дефенсив просто ждёт повода — рядом никого нет
        char text[48];
        const int window = defensive::windowLeft();
        if (window > 0) {
            line("DEF ОКНО", color::accent);
        } else if (defensive::verdict() == defensive::Verdict::Ignores) {
            line("DEF НЕТ ОТКАТА", color::bad);
        } else if (!defensive::triggerActive()) {
            line("DEF ЖДЁТ", color::textDim);
        } else {
            const int percent = defensive::chargePercent();
            std::snprintf(text, sizeof(text), "DEF %d%%", percent);
            line(text, percent >= 100 ? color::good : color::accent);
        }
    }
    if (defensive::lagPeekHolding())
        line("LAG PEEK", color::good);
    if (config.movement.autoPeekKey.isSet() && config.movement.autoPeekKey.isDown())
        line("PEEK", color::accent);
    if (config.antiAim.enabled) {
        char text[96];
        if (config.antiAim.desync)
            std::snprintf(text, sizeof(text), "%s · %s", antiaim::desyncSide() > 0 ? "DESYNC <" : "DESYNC >", antiaim::stateName());
        else
            std::snprintf(text, sizeof(text), "AA · %s", antiaim::stateName());
        line(text, color::accent);
    }
    if (ragebot::minDamageOverrideActive())
        line("DMG", color::text);
    if (ragebot::forceBodyActive())
        line("BAIM", color::text);
    if (ragebot::forceShotActive())
        line("FORCE", color::text);
    if (config.rage.enabled && *ragebot::status())
        line(ragebot::status(), color::textDim);

    // резольвер по слоям: угол ног цели и насколько уверенно он найден
    if (config.rage.enabled && config.rage.resolver && config.rage.layerResolver) {
        const int target = ragebot::currentTarget();
        if (resolver::valid(target)) {
            char text[64];
            std::snprintf(text, sizeof(text), "R %+.0f°  %.0f%%", resolver::delta(target), resolver::confidence(target) * 100.0f);
            line(text, resolver::confidence(target) > 0.3f ? color::good : color::accent2);
        } else if (target > 0 && !resolver::available()) {
            line("R —", color::bad);
        }
    }
}

// ---- HUD ----
namespace
{

// HUD в цветах NewerWin (как было до переделки меню) — свои константы, палитра меню другая
constexpr ImU32 hudBg = IM_COL32(12, 20, 31, 225);
constexpr ImU32 hudLine = IM_COL32(35, 42, 56, 160);
constexpr ImU32 nwAccent = IM_COL32(66, 135, 245, 255);
constexpr ImU32 nwAccent2 = IM_COL32(80, 147, 255, 255);
constexpr ImU32 nwChip = IM_COL32(31, 38, 49, 255);
constexpr ImU32 nwChipText = IM_COL32(166, 200, 253, 255);

ImFont* hudFont() noexcept
{
    return fonts.ui ? fonts.ui : (fonts.menu ? fonts.menu : ImGui::GetFont());
}

}

// Ватермарка NewerWin: пилюля сверху по центру — «NW» акцентом, дальше сегменты
// [синяя иконка + белый текст]: ник, FPS, время, пинг. Цвета свои, старые NewerWin —
// остальной интерфейс в другой палитре
void drawWatermark(ImDrawList* d) noexcept
{
    if (!config.view.watermark)
        return;


    char nick[32] = "";
    bool inMatch = false;
    {
        GameData::Lock lock;
        const auto& local = GameData::local();
        inMatch = local.exists;
        if (local.exists)
            std::snprintf(nick, sizeof(nick), "%s", local.name);
    }
    if (!*nick) {
        const char* user = std::getenv("USER");
        std::snprintf(nick, sizeof(nick), "%s", user && *user ? user : "user");
    }

    const std::time_t now = std::time(nullptr);
    std::tm tm{};
    localtime_r(&now, &tm);

    // FPS сглаженный, обновляется 4 раза в секунду — чтобы цифры не мельтешили
    static float shownFps = 0.0f;
    static double fpsAt = 0.0;
    if (ImGui::GetTime() - fpsAt > 0.25) {
        shownFps = ImGui::GetIO().Framerate;
        fpsAt = ImGui::GetTime();
    }

    char fps[24], time[16], ping[24];
    std::snprintf(fps, sizeof(fps), "%d FPS", static_cast<int>(shownFps + 0.5f));
    std::snprintf(time, sizeof(time), "%02d:%02d", tm.tm_hour, tm.tm_min);
    std::snprintf(ping, sizeof(ping), "%d ms", GameData::getNetOutgoingLatency());

    struct Segment { Icon icon; const char* text; };
    const Segment segments[]{ { Icon::User, nick }, { Icon::Speed, fps }, { Icon::Clock, time }, { Icon::Ping, ping } };

    ImFont* font = hudFont();
    ImFont* logo = fonts.uiBold ? fonts.uiBold : font;
    const float fontSize = font->FontSize;
    const float logoSize = logo->FontSize + 2.0f;
    constexpr float padX = 10.0f, height = 26.0f, iconSize = 12.0f, iconGap = 5.0f, segmentGap = 11.0f;

    const float logoW = logo->CalcTextSizeA(logoSize, FLT_MAX, 0.0f, "NW").x;
    float width = padX * 2.0f + logoW;
    float textW[4];
    for (int i = 0; i < 4; ++i) {
        textW[i] = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, segments[i].text).x;
        width += segmentGap + iconSize + iconGap + textW[i];
    }

    const auto& display = ImGui::GetIO().DisplaySize;
    // В матче сверху по центру стоит табло счёта (время раунда, счёт, аватарки) — уходим под него.
    // Высота табло Panorama растёт с высотой экрана: ~8.5% (≈92 px на 1080p). Съезжает плавно.
    static float shownY = 4.0f;
    const float targetY = inMatch ? std::floor(display.y * 0.085f) + 4.0f : 4.0f;
    shownY += (targetY - shownY) * std::min(1.0f, ImGui::GetIO().DeltaTime * 10.0f);
    if (std::abs(targetY - shownY) < 0.5f)
        shownY = targetY;
    const ImVec2 pos{ std::floor((display.x - width) * 0.5f), std::floor(shownY) };
    const ImVec2 max{ pos.x + width, pos.y + height };
    d->AddRectFilled(pos, max, hudBg, 8.0f);

    float x = pos.x + padX;
    const float cy = pos.y + height * 0.5f;
    d->AddText(logo, logoSize, { x, cy - logoSize * 0.5f - 0.5f }, nwAccent2, "NW");
    x += logoW;
    for (int i = 0; i < 4; ++i) {
        x += segmentGap;
        drawIcon(d, segments[i].icon, { x + iconSize * 0.5f, cy }, iconSize, nwAccent);
        x += iconSize + iconGap;
        d->AddText(font, fontSize, { x, cy - fontSize * 0.5f }, IM_COL32(255, 255, 255, 255), segments[i].text);
        x += textW[i];
    }
}

// Панель «Hotkeys» NewerWin: шапка (иконка + название), под ней активные бинды:
// слева название, справа клавиша. Строки появляются и уходят плавно.
// При открытом меню панель видна всегда и таскается за шапку. Единственный список биндов.
void drawHotkeys(ImDrawList* d, bool menuOpen) noexcept
{
    if (!config.view.hotkeys)
        return;
    const bool inGame = localPlayer && localPlayer.get().isAlive();
    if (!inGame && !menuOpen)
        return;

    struct Row { const char* name; const char* value; bool active; };
    Row rows[40];
    int count = 0;
    const auto add = [&](const char* name, const KeyBind& key, bool active) {
        if (count < 40 && key.isSet())
            rows[count++] = { name, key.toString(), inGame && active };
    };
    const auto& ex = config.exploits;
    const auto& aa = config.antiAim;
    add("Double tap", ex.doubleTapKey, ex.doubleTap && ex.doubleTapKey.isToggled());
    add("Hide shots", ex.hideShotsKey, ex.hideShots && ex.hideShotsKey.isToggled());
    add("Defensive", config.defensive.key, config.defensive.key.isDown());
    add("Teleport", ex.teleportKey, ex.teleportKey.isDown());
    add("Lag peek", ex.lagPeekKey, ex.lagPeek && ex.lagPeekKey.isDown());
    add("Fake duck", config.movement.fakeDuckKey, movement::isFakeDucking());
    add("Auto peek", config.movement.autoPeekKey, config.movement.autoPeekKey.isDown());
    add("Freestand", aa.freestandKey, aa.freestandKey.isDown() && antiaim::isFreestanding());
    add("Edge yaw", aa.edgeKey, aa.edgeYaw && aa.edgeKey.isDown());
    add("Inverter", aa.inverter, aa.inverter.isToggled());
    // бинды на настройки (ПКМ в меню)
    // одна строка на «подпись + клавишу»: «Мин. урон» висит на каждой группе оружия — 8 одинаковых строк
    {
        const auto& all = binds::all();
        for (std::size_t k = 0; k < all.size(); ++k) {
            bool duplicate = false, active = all[k].resolved && all[k].active;
            for (std::size_t m = 0; m < all.size(); ++m) {
                if (m == k || all[m].label != all[k].label || all[m].key.toString() != std::string_view{ all[k].key.toString() })
                    continue;
                if (m < k)
                    duplicate = true;
                active = active || (all[m].resolved && all[m].active);
            }
            if (!duplicate)
                add(all[k].label.c_str(), all[k].key, active);
        }
    }
    if (count < 40 && aa.enabled) {
        const auto manual = antiaim::manualDirection();
        const char* dir = manual == antiaim::Manual::Left ? "Left" : manual == antiaim::Manual::Right ? "Right"
            : manual == antiaim::Manual::Back ? "Back" : manual == antiaim::Manual::Forward ? "Forward" : "";
        rows[count++] = { "Manual", dir, inGame && manual != antiaim::Manual::None };
    }

    ImFont* font = hudFont();
    ImFont* small = fonts.uiSmall ? fonts.uiSmall : font;
    const float fontSize = font->FontSize;
    constexpr float headerH = 26.0f, rowH = 21.0f, padX = 10.0f;

    // плавность строк по имени
    float shown[40];
    float bodyH = 0.0f;
    float widest = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, "Hotkeys").x + 26.0f;
    for (int i = 0; i < count; ++i) {
        shown[i] = animate(ImHashStr(rows[i].name) ^ 0x484b, rows[i].active, 10.0f);
        if (shown[i] <= 0.001f)
            continue;
        bodyH += rowH * shown[i];
        const float w = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, rows[i].name).x
            + small->CalcTextSizeA(small->FontSize, FLT_MAX, 0.0f, rows[i].value).x + 30.0f;
        widest = std::max(widest, w);
    }
    // ширина тоже плывёт, а не прыгает
    static float width = 0.0f;
    const float targetW = std::max(130.0f, widest + padX * 2.0f);
    width = width <= 0.0f ? targetW : width + (targetW - width) * std::min(1.0f, ImGui::GetIO().DeltaTime * 14.0f);

    const auto& display = ImGui::GetIO().DisplaySize;
    ImVec2 pos{ std::floor(config.view.hotkeysX * display.x), std::floor(config.view.hotkeysY * display.y) };

    // перетаскивание за шапку при открытом меню (если мышь не над окном меню)
    static bool dragging = false;
    static ImVec2 grab{};
    const auto& io = ImGui::GetIO();
    if (menuOpen) {
        const bool overHeader = io.MousePos.x >= pos.x && io.MousePos.x <= pos.x + width && io.MousePos.y >= pos.y && io.MousePos.y <= pos.y + headerH;
        if (!dragging && overHeader && io.MouseClicked[0] && !ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow)) {
            dragging = true;
            grab = { io.MousePos.x - pos.x, io.MousePos.y - pos.y };
        }
        if (dragging) {
            if (io.MouseDown[0]) {
                pos.x = std::clamp(io.MousePos.x - grab.x, 0.0f, display.x - width);
                pos.y = std::clamp(io.MousePos.y - grab.y, 0.0f, display.y - headerH);
                config.view.hotkeysX = pos.x / display.x;
                config.view.hotkeysY = pos.y / display.y;
            } else {
                dragging = false;
            }
        }
    } else {
        dragging = false;
    }

    const ImVec2 max{ pos.x + width, pos.y + headerH + (bodyH > 0.5f ? bodyH + 6.0f : 0.0f) };
    d->AddRectFilled(pos, max, hudBg, 8.0f);
    if (dragging)
        d->AddRect(pos, max, withAlpha(nwAccent2, 0.7f), 8.0f);

    // шапка
    drawIcon(d, Icon::Keyboard, { pos.x + padX + 7.0f, pos.y + headerH * 0.5f }, 13.0f, nwAccent);
    d->AddText(font, fontSize, { pos.x + padX + 20.0f, pos.y + (headerH - fontSize) * 0.5f }, IM_COL32(255, 255, 255, 255), "Hotkeys");
    if (bodyH > 0.5f)
        d->AddLine({ pos.x + padX, pos.y + headerH }, { max.x - padX, pos.y + headerH }, hudLine);

    // строки: клип по телу, чтобы уходящая строка не вылезала
    d->PushClipRect({ pos.x, pos.y + headerH }, max, true);
    float y = pos.y + headerH + 3.0f;
    for (int i = 0; i < count; ++i) {
        if (shown[i] <= 0.001f)
            continue;
        const float a = shown[i];
        const float cy = y + rowH * 0.5f;
        d->AddText(font, fontSize, { pos.x + padX, cy - fontSize * 0.5f }, withAlpha(IM_COL32(255, 255, 255, 255), a), rows[i].name);
        // клавиша — в плашке справа, как у Server Binds
        const auto valueSize = small->CalcTextSizeA(small->FontSize, FLT_MAX, 0.0f, rows[i].value);
        const ImVec2 chipMin{ max.x - padX - valueSize.x - 10.0f, cy - 8.0f };
        d->AddRectFilled(chipMin, { max.x - padX, cy + 8.0f }, withAlpha(nwChip, a), 5.0f);
        d->AddText(small, small->FontSize, { chipMin.x + 5.0f, cy - small->FontSize * 0.5f }, withAlpha(nwChipText, a), rows[i].value);
        y += rowH * a;
    }
    d->PopClipRect();
}

static int selectedWeapon = 0;

void drawRageTab() noexcept
{
    auto& rage = config.rage;

    selectedWeapon = std::clamp(selectedWeapon, 0, static_cast<int>(weaponGroupNames.size()) - 1);
    const float width = (ImGui::GetContentRegionAvail().x - 12.0f) * 0.5f;
    constexpr float gap = 10.0f;

    // ---- левая колонка: основное, резольвер, эксплойты ----
    ImGui::BeginGroup();
    if (beginGroup("Основное", { width, 0.0f })) {
        toggle("Включить", &rage.enabled);
        toggle("Сайлент", &rage.silent);
        toggle("Автовыстрел", &rage.autoFire);
        toggle("Автоволл", &rage.autowall);
        toggle("Форсшут", &rage.forceShot);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Стрелять, даже если заданный «Мин. урон» по этой цели недостижим:\n"
                              "порог опускается до лучшего достижимого урона, и выстрел всё равно идёт.\n"
                              "Без него не стреляет — кроме случая, когда порог требует убийства, а набирается\n"
                              "хотя бы половина хп (вторая пуля DT добьёт).");
        sliderFloat("FOV", &rage.fov, 1.0f, 180.0f, "%.0f°");
        toggle("Только в тело", &rage.forceBody);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Клавиша, МД на клавишу и прочие «оверрайды» — ПКМ по строке: бинд");
        toggle("Дормант-аимбот", &rage.dormantAim);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Враг за пределами видимости (дормант), но слышен — шаги, выстрел —\n"
                              "или виден союзникам на радаре: стрельба в живот через стену,\n"
                              "если пробивает на мин. урон. Только когда видимых целей нет.");
        if (rage.dormantAim)
            sliderInt("Мин. точность", &rage.dormantAccuracy, 25, 95, "%d%%");
    }
    endGroup();
    ImGui::Dummy({ 0.0f, gap });

    if (beginGroup("Резольвер", { width, 0.0f })) {
        toggle("Включить", &rage.resolver);
        if (rage.resolver) {
            toggle("По слоям анимации", &rage.layerResolver);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Сервер присылает слои анимации врага — они посчитаны из его настоящего угла ног.\n"
                                  "Подставляем в animstate врага углы-кандидаты, прогоняем ту же функцию движка\n"
                                  "и берём угол, чьи слои совпали с сетевыми. Выкл — только перебор стороны по промахам.");
            if (rage.layerResolver) {
                static constexpr const char* stepModes[]{ "Грубо (5)", "Обычно (9)", "Точно (17)" };
                int steps = rage.resolverSteps >= 17 ? 2 : rage.resolverSteps >= 9 ? 1 : 0;
                if (combo("Кандидатов на обновление", &steps, stepModes, IM_ARRAYSIZE(stepModes)))
                    rage.resolverSteps = steps == 2 ? 17 : steps == 1 ? 9 : 5;
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Сколько углов ног перебирать на каждое обновление врага.\nТочнее — дороже по процессору.");
            }
            static constexpr const char* safeModes[]{ "Выкл", "После промаха", "Всегда" };
            combo("Безопасные точки", &rage.safePoint, safeModes, IM_ARRAYSIZE(safeModes));
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Выкл — смесь гипотез сама решает: точка, общая для всех сторон десинка,\n"
                                  "и так получает больший шанс, чем точка одной стороны.\n"
                                  "После промаха / Всегда — стрелять ТОЛЬКО в точки, общие для всех гипотез.");
        }
    }
    endGroup();
    ImGui::Dummy({ 0.0f, gap });

    auto& ex = config.exploits;
    if (beginGroup("Эксплойты", { width, 0.0f })) {
        toggle("Double tap", &ex.doubleTap);
        keybind("Клавиша DT", ex.doubleTapKey);
        static constexpr const char* dtModes[]{ "Быстрая стрельба", "Дефенсив" };
        combo("Режим DT", &ex.doubleTapMode, dtModes, IM_ARRAYSIZE(dtModes));
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Быстрая стрельба — сдвиг на выстреле, вторая пуля сразу. На это время\nдефенсив молчит: заряд один, и он принадлежит выстрелу.\nДефенсив — выстрел обычный, весь заряд уходит на рывки дефенсива.\nСкаут и AWP всегда в дефенсиве (второй пули нет, DT-фикс на серверах).");
        sliderInt("Тиков сдвига", &ex.shiftTicks, 6, 14);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Заряд под вторую пулю DT (быстрая стрельба). У дефенсива свой ползунок ниже.");
        sliderInt("Команд за тик (сервер)", &ex.serverPackLimit, 0, 16, ex.serverPackLimit == 0 ? "выкл" : "%d");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Сколько команд удалённый сервер принимает за тик. Чок, фейклаг, лаг-пик и сдвиг DT\n"
                              "не длиннее этого: хвост длинной пачки сервер выкидывает, и выстрел уходит из старой\n"
                              "позиции (в стену). Мисы с «пуля застряла» на лаг-пике — уменьши. На своём сервере — не важно.");
        toggle("Нож в прыжке с DT — без лагов", &ex.knifeAirNoLag);
        keybind("Телепорт", ex.teleportKey);
        toggle("Хайдшоты", &ex.hideShots);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Выстрел всегда в чокнутой команде, следующим пакетом — углы AA:\n"
                              "угол выстрела (голова вверх, взгляд на врага) никто не видит,\n"
                              "резольверу по нему не за что зацепиться. При быстром DT не работают.");
        if (ex.hideShots)
            keybind("Клавиша хайдшотов", ex.hideShotsKey);
        toggle("Авто хайдшоты", &ex.autoHideShots);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("С включённым DT: стоишь, идёшь на shift или сидишь — вместо быстрой\n"
                              "стрельбы хайдшоты (стоячий DT резольвят легче всего). В воздухе,\n"
                              "на бегу и на автопике — снова DT.");
        if (ex.autoHideShots) {
            toggle("Автоматы, ПП, пистолеты", &ex.autoHsRifles);
            toggle("Дигл", &ex.autoHsDeagle);
            toggle("Автоснайперки (SCAR/G3SG1)", &ex.autoHsAuto);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Выключено — стоя и сидя у автоснайперки остаётся DT: вторая пуля\n"
                                  "добивает того, кого первая только ранила.");
        }
        if (!tickbase::isInstalled()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(color::bad));
            ImGui::TextWrapped("CL_Move не найден — DT и десинк недоступны в этом билде игры");
            ImGui::PopStyleColor();
        }
    }
    endGroup();
    ImGui::EndGroup();

    ImGui::SameLine(0.0f, 12.0f);

    // ---- правая колонка: оружие (выбор в шапке), пик, нож и зевс ----
    ImGui::BeginGroup();
    auto& weapon = rage.weapons[selectedWeapon];
    if (beginGroup("Наведение", { width, 0.0f })) {
        if (selectedWeapon != 0)
            toggle("Свои настройки", &weapon.override);
        else
            weapon.override = true;

        static constexpr const char* hitboxNames[]{ "Голова", "Грудь", "Живот", "Таз", "Руки", "Ноги", "Стопы" };
        multiCombo("Хитбоксы", &weapon.hitboxes, hitboxNames, IM_ARRAYSIZE(hitboxNames));
        multiCombo("Мультипоинт", &weapon.multipoint, hitboxNames, IM_ARRAYSIZE(hitboxNames));
        if (weapon.multipoint) {
            toggle("Авто-размер точек", &weapon.autoScale);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Как в ските: размер точек считается каждый выстрел от разброса оружия на этой дистанции,\n"
                    "чтобы пули ложились в хитбокс. Далеко / в движении точки сжимаются к центру, лишние отпадают.\n"
                    "Ползунки ниже тогда — потолок размера.");
            sliderFloat(weapon.autoScale ? "Макс. размер: голова" : "Размер: голова", &weapon.headScale, 0.0f, 1.0f, "%.2f");
            sliderFloat(weapon.autoScale ? "Макс. размер: тело" : "Размер: тело", &weapon.bodyScale, 0.0f, 1.0f, "%.2f");
        }
        sliderInt("Хитшанс", &weapon.hitchance, 0, 100, "%d%%");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Как раньше: доля из 256 сидов разброса, где пуля даёт мин. урон по главной модели врага.\n"
                              "Сомнения резольвера (сторона, прогноз, джиттер) выбирают точку, а стрелять запрещают\n"
                              "только если с ними шанс ниже половины этого порога.");
        // выше 100 — «HP+N»; другое значение на клавишу — ПКМ по строке (бинд)
        sliderInt("Мин. урон", &weapon.minDamage, 1, 120, "@hp");
    }
    endGroup();
    ImGui::Dummy({ 0.0f, gap });

    if (beginGroup("Точность", { width, 0.0f })) {
        static constexpr const char* autoStops[]{ "Выкл", "Полный", "Точность" };
        combo("Автостоп", &weapon.autoStop, autoStops, IM_ARRAYSIZE(autoStops));
        if (weapon.autoStop != static_cast<int>(AutoStop::Off)) {
            toggle("Тормозить между выстрелами", &weapon.autoStopEarly);
            toggle("Стоп перед пиком", &weapon.peekStop);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Через пару тиков выйдем туда, откуда враг простреливается с мин. уроном, —\n"
                                  "тормозим и скопимся заранее. На выходе уже стоим точно и стреляем сразу,\n"
                                  "а не гасим скорость 5–7 тиков под прицелом холдера.");
        }
        toggle("Автоскоуп", &weapon.autoScope);
        toggle("Предпочитать тело", &weapon.preferBody);
        toggle("В тело, если летально", &weapon.bodyIfLethal);
    }
    endGroup();
    ImGui::Dummy({ 0.0f, gap });

    if (beginGroup("Пик", { width, 0.0f })) {
        toggle("Лаг-пик", &ex.lagPeek);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Держит чок, пока выходишь из-за угла; выстрел отпускает пачку.\n"
                              "Не работает с DT «Быстрая стрельба» и фейкдаком.");
        if (ex.lagPeek) {
            keybind("Клавиша лаг-пика", ex.lagPeekKey);
            sliderInt("Чок на выходе", &ex.lagPeekTicks, 2, 14, "%d");
            sliderInt("Предсказание выхода", &ex.lagPeekPredict, 2, 16, "%d");
            colorToggle("Показывать позицию на сервере", &config.view.serverBox, config.view.serverBoxColor);
        }
        keybind("Автопик", config.movement.autoPeekKey);
        {
            static constexpr const char* peekModes[]{ "После выстрела", "Выстрел или отпустил WASD" };
            combo("Возврат автопика", &config.movement.autoPeekMode, peekModes, IM_ARRAYSIZE(peekModes));
        }
        toggle("AI peek", &config.movement.aiPeek);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Зажат автопик, WASD не трогаешь: ищет вокруг точку в паре шагов, откуда\n"
                              "пробиваешь врага у прицела на мин. урон, сам выходит туда, рейдж стреляет,\n"
                              "автопик возвращает. Лучше с DT (телепорт назад).");
        if (config.movement.aiPeek) {
            sliderInt("Качество поиска", &config.movement.aiPeekQuality, 30, 100, "%d%%");
            sliderInt("Проверок подряд", &config.movement.aiPeekSafety, 1, 16);
        }
        keybind("Фейкдак", config.movement.fakeDuckKey);
    }
    endGroup();
    ImGui::Dummy({ 0.0f, gap });

    if (beginGroup("Нож и зевс", { width, 0.0f })) {
        toggle("Кнайфбот", &rage.knifeBot);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("С ножом в руке сам бьёт врага в радиусе удара (с учётом бектрека):\n"
                              "ПКМ (до 32 ед.) — 65 урона, в спину 180; ЛКМ (до 48 ед.) — 25, в спину 90.\n"
                              "Броня режет урон на 15%%. Сначала смертельный ПКМ, потом смертельный ЛКМ.");
        if (rage.knifeBot) {
            static constexpr const char* knifeModes[]{ "Только наповал", "Любой удар" };
            combo("Когда бить", &rage.knifeBotMode, knifeModes, IM_ARRAYSIZE(knifeModes));
        }
        toggle("Зевсбот", &rage.zeusBot);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("С зевсом в руке стреляет только наверняка: грудь/живот/таз,\nурон должен убивать, автостоп по точности.");
        if (rage.zeusBot) {
            sliderInt("Хитшанс зевса", &rage.zeusHitchance, 30, 100, "%d%%");
            sliderInt("Запас урона", &rage.zeusMargin, 0, 20, "+%d хп");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Стреляет, только если урон больше хп цели на столько.\n"
                                  "Урон зевса резко падает с дистанцией: впритык на краю — враг выживает.");
        }
    }
    endGroup();
    ImGui::EndGroup();
}

int& rageWeapon() noexcept
{
    return selectedWeapon;
}

static void sectionTitle(const char* title) noexcept
{
    ImGui::Dummy({ 0.0f, 6.0f });
    ImGui::PushFont(fonts.uiBold ? fonts.uiBold : fonts.bold);
    ImGui::TextUnformatted(title);
    ImGui::PopFont();
}

static void hint(const char* text) noexcept
{
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(color::textDim));
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
}

static void tooltip(const char* text) noexcept
{
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", text);
}

// ---------- вкладка «Антиаимы» ----------
// Страницы: Углы · Билдер · Десинк · Дефенсив · Анти-брут. Настройки разложены по смыслу
// («что за углы» / «по состояниям» / «чем ломаем сервер» / «скрытые углы» / «против их резольвера»),
// а на первой странице справа — вид сверху на то, что видит сервер: взгляд камеры, отправленный
// взгляд и увод тела десинком. Раньше всё это лежало двумя простынями по 25 строк, и понять,
// что в итоге уходит на сервер, можно было только по 3-му лицу.

// высота группы под n строк: отступы child'а + строки + хвост endGroup
[[nodiscard]] static float rowsHeight(int n) noexcept
{
    // подпись 22 + отступы карточки 14 + строки по 30
    return 38.0f + static_cast<float>(n) * 30.0f;
}

// Строка списка (состояния билдера, пресеты): полоска акцента у выбранной, заметка справа серым
static bool listRow(const char* label, bool selected, const char* note) noexcept
{
    auto* window = ImGui::GetCurrentWindow();
    if (window->SkipItems)
        return false;

    const auto id = window->GetID(label);
    const auto pos = window->DC.CursorPos;
    const float width = ImGui::GetContentRegionAvail().x;
    const ImRect bb{ pos, { pos.x + width, pos.y + rowHeight + 2.0f } };

    ImGui::ItemSize(bb);
    if (!ImGui::ItemAdd(bb, id))
        return false;

    bool hovered, held;
    const bool pressed = ImGui::ButtonBehavior(bb, id, &hovered, &held);
    const float t = animate(id, selected);
    const float h = animate(id + 1, hovered, 16.0f);

    auto* drawList = window->DrawList;
    if (const float fill = std::max(t, h * 0.45f); fill > 0.01f)
        drawList->AddRectFilled(bb.Min, bb.Max, withAlpha(IM_COL32(255, 255, 255, 26), fill));
    if (t > 0.01f)
        drawList->AddRectFilled(bb.Min, { bb.Min.x + 2.0f, bb.Max.y }, withAlpha(color::accent, t));

    const auto labelEnd = ImGui::FindRenderedTextEnd(label);
    const float textY = pos.y + (bb.GetHeight() - ImGui::GetFontSize()) * 0.5f;
    drawList->AddText({ pos.x + 10.0f, textY }, lerpColor(color::text, color::accent, t), label, labelEnd);
    if (note && *note) {
        const auto noteEnd = ImGui::FindRenderedTextEnd(note);
        const float noteWidth = ImGui::CalcTextSize(note, noteEnd).x;
        drawList->AddText({ bb.Max.x - noteWidth - 6.0f, textY }, color::textDim, note, noteEnd);
    }
    return pressed;
}

// подпись-значение в одну строку: слева серым, справа значением
static void readout(const char* label, const char* value, ImU32 valueColor = color::text) noexcept
{
    auto* window = ImGui::GetCurrentWindow();
    if (window->SkipItems)
        return;
    const auto pos = window->DC.CursorPos;
    const float width = ImGui::GetContentRegionAvail().x;
    ImGui::ItemSize(ImRect{ pos, { pos.x + width, pos.y + rowHeight } });

    auto* drawList = window->DrawList;
    const float textY = pos.y + (rowHeight - ImGui::GetFontSize()) * 0.5f;
    drawList->AddText({ pos.x, textY }, color::textDim, label, ImGui::FindRenderedTextEnd(label));
    const auto valueEnd = ImGui::FindRenderedTextEnd(value);
    const float valueWidth = ImGui::CalcTextSize(value, valueEnd).x;
    drawList->AddText({ pos.x + width - valueWidth, textY }, valueColor, value, valueEnd);
}

// Вид сверху на свою позу, как её получил сервер. Камера смотрит вверх, остальное — относительно неё:
// так сразу видно, насколько AA уводит голову и где на сервере стоит тело.
static void drawServerPose(float size) noexcept
{
    constexpr float deg = 3.14159265f / 180.0f;
    auto* drawList = ImGui::GetWindowDrawList();
    const auto origin = ImGui::GetCursorScreenPos();
    const float dial = std::min(size, 230.0f);
    const ImVec2 center{ origin.x + size * 0.5f, origin.y + dial * 0.5f };
    const float radius = dial * 0.40f;

    float sentPitch = 0.0f, sentYaw = 0.0f, desync = 0.0f, roll = 0.0f;
    const bool alive = localPlayer && localPlayer.get().isAlive();
    const bool have = antiaim::sentServerPose(sentPitch, sentYaw, desync, roll) && alive && config.antiAim.enabled;
    float cameraPitch = 0.0f, cameraYaw = 0.0f;
    antiaim::cameraAngles(cameraPitch, cameraYaw);
    if (!have) {
        sentYaw = cameraYaw;
        sentPitch = cameraPitch;
        desync = 0.0f;
        roll = 0.0f;
    }

    // циферблат
    drawList->AddRectFilled({ origin.x, origin.y }, { origin.x + size, origin.y + dial }, IM_COL32(15, 15, 15, 255));
    drawList->AddRect({ origin.x, origin.y }, { origin.x + size, origin.y + dial }, color::outline);
    drawList->AddCircle(center, radius, color::groupLine, 64);
    drawList->AddCircle(center, radius * 0.55f, withAlpha(color::groupLine, 0.5f), 48);
    for (int a = 0; a < 360; a += 45) {
        const float r = static_cast<float>(a) * deg;
        const float inner = (a % 90 == 0) ? radius * 0.88f : radius * 0.94f;
        drawList->AddLine({ center.x - std::sin(r) * inner, center.y - std::cos(r) * inner },
                          { center.x - std::sin(r) * radius, center.y - std::cos(r) * radius },
                          withAlpha(color::groupLine, a % 90 == 0 ? 1.0f : 0.6f));
    }

    // экранное направление абсолютного yaw: камера всегда вверх
    const auto point = [&](float absYaw, float length) {
        const float r = std::remainder(absYaw - cameraYaw, 360.0f) * deg;
        return ImVec2{ center.x - std::sin(r) * length, center.y - std::cos(r) * length };
    };

    // сектор между отправленным взглядом и телом — то, что враг должен разрезольвить
    if (have && std::abs(desync) > 0.5f) {
        const float from = std::remainder(sentYaw - cameraYaw, 360.0f);
        drawList->PathLineTo(center);
        const int steps = 24;
        for (int i = 0; i <= steps; ++i) {
            const float r = (from + desync * static_cast<float>(i) / static_cast<float>(steps)) * deg;
            drawList->PathLineTo({ center.x - std::sin(r) * radius * 0.92f, center.y - std::cos(r) * radius * 0.92f });
        }
        drawList->PathFillConvex(withAlpha(color::accent, 0.14f));
    }

    // камера (куда смотришь на самом деле)
    drawList->AddLine(center, point(cameraYaw, radius), withAlpha(color::text, 0.45f), 1.0f);
    drawList->AddText({ center.x - 22.0f, origin.y + 6.0f }, withAlpha(color::text, 0.45f), "камера");

    // отправленный взгляд — его видят враги
    if (have) {
        const auto head = point(sentYaw, radius * 0.92f);
        drawList->AddLine(center, head, color::accent, 2.0f);
        drawList->AddCircleFilled(head, 3.0f, color::accent);
        // тело на сервере
        const auto body = point(sentYaw + desync, radius * 0.72f);
        drawList->AddLine(center, body, color::good, 2.0f);
        drawList->AddCircleFilled(body, 3.0f, color::good);
    }

    // фигурка в центре
    drawList->AddCircleFilled(center, 4.0f, color::text);

    // шкала pitch справа: вверх — голова в небо, вниз — в пол
    const float barX = origin.x + size - 16.0f;
    const float barTop = origin.y + 14.0f;
    const float barBottom = origin.y + dial - 14.0f;
    drawList->AddRectFilled({ barX, barTop }, { barX + 3.0f, barBottom }, color::frame);
    drawList->AddLine({ barX - 2.0f, (barTop + barBottom) * 0.5f }, { barX + 6.0f, (barTop + barBottom) * 0.5f }, color::groupLine);
    const float pitchT = std::clamp((sentPitch + 89.0f) / 178.0f, 0.0f, 1.0f);
    const float pitchY = barTop + (barBottom - barTop) * pitchT;
    drawList->AddRectFilled({ barX - 3.0f, pitchY - 1.5f }, { barX + 6.0f, pitchY + 1.5f }, have ? color::accent : color::textDim);

    ImGui::Dummy({ size, dial + 6.0f });

    if (!alive) {
        hint("Зайди в игру — здесь будет видно, какие углы уходят на сервер: взгляд (зелёная линия),\nтело после десинка (светло-зелёная) и настоящий взгляд камеры (серая).");
        return;
    }
    if (!config.antiAim.enabled) {
        hint("Антиаимы выключены — на сервер уходит настоящий взгляд.");
        return;
    }

    char text[64];
    std::snprintf(text, sizeof(text), "%+.0f°", std::remainder(sentYaw - cameraYaw, 360.0f));
    readout("Взгляд от камеры", text, color::accent);
    std::snprintf(text, sizeof(text), "%+.0f°", desync);
    readout("Тело от взгляда", text, std::abs(desync) > 0.5f ? color::good : color::textDim);
    std::snprintf(text, sizeof(text), "%+.0f°", sentPitch);
    readout("Pitch", text);
    if (std::abs(roll) > 0.5f) {
        std::snprintf(text, sizeof(text), "%+.0f°", roll);
        readout("Roll", text);
    }
    readout("Состояние", antiaim::stateName());
    const int side = antiaim::desyncSide();
    readout("Сторона", side > 0 ? "вправо" : side < 0 ? "влево" : "—");

    const auto manual = antiaim::manualDirection();
    const char* manualName = manual == antiaim::Manual::Left ? "влево"
        : manual == antiaim::Manual::Right ? "вправо"
        : manual == antiaim::Manual::Back ? "назад"
        : manual == antiaim::Manual::Forward ? "вперёд" : nullptr;
    if (manualName)
        readout("Ручной AA", manualName, color::accent);
    if (antiaim::isFreestanding())
        readout("Фристенд", "прячет голову", color::accent);
    if (antiaim::defensiveActive())
        readout("Дефенсив", "скрытые углы", color::good);
}

// ---------- страница «Углы» ----------
static void drawAaAngles(float width, float height) noexcept
{
    auto& aa = config.antiAim;

    ImGui::BeginGroup();
    if (beginGroup("Основное", { width, rowsHeight(7) })) {
        toggle("Антиаимы", &aa.enabled);
        static constexpr const char* pitches[]{ "Выкл", "Вниз", "Вверх", "Ноль" };
        combo("Pitch", &aa.pitch, pitches, IM_ARRAYSIZE(pitches));
        static constexpr const char* bases[]{ "Выкл", "Назад", "На врагов" };
        combo("База yaw", &aa.yawBase, bases, IM_ARRAYSIZE(bases));
        tooltip("От чего считать углы билдера: «Назад» — от направления движения/взгляда,\n«На врагов» — спиной к ближайшему врагу.");
        static constexpr const char* safeHeads[]{ "Выкл", "Обычный", "Умный" };
        combo("Safe head", &aa.safeHead, safeHeads, IM_ARRAYSIZE(safeHeads));
        tooltip("Обычный — прыжок с ножом, враг не сильно ниже: голова прямо, без десинка.\n"
                "Умный — по углу, под которым враг видит голову: в «опасной» полосе десинк выключается.");
        toggle("Легит AA на E", &aa.legitAa);
        tooltip("Пока зажата E — смотрим, куда смотрим (дефьюз, подбор оружия), десинк остаётся.");
        toggle("Плавные переходы", &aa.naturality);
        tooltip("При смене состояния (стоя → бег → прыжок) углы билдера перетекают\nвместо скачка, по которому его резольвер ловит смену.");
        if (aa.naturality)
            sliderFloat("Время перехода", &aa.naturalityTime, 0.05f, 1.0f, "%.2f с");
    }
    endGroup();
    ImGui::Dummy({ 0.0f, 10.0f });

    if (beginGroup("Ручной AA", { width, rowsHeight(5) })) {
        keybind("Влево", aa.manualLeft);
        keybind("Вправо", aa.manualRight);
        keybind("Назад", aa.manualBack);
        keybind("Вперёд", aa.manualForward);
        toggle("Статика на ручном", &aa.staticOnManual);
        tooltip("На ручном AA — без модификатора и джиттера десинка, десинк на полную:\nсам выбрал сторону, крутить её незачем.");
    }
    endGroup();
    ImGui::Dummy({ 0.0f, 10.0f });

    const float left = std::max(height - rowsHeight(7) - rowsHeight(5) - 20.0f, rowsHeight(2));
    if (beginGroup("Укрытия", { width, left })) {
        toggle("Фристенд", &aa.freestanding);
        tooltip("Голову — за ближайшее укрытие от врага.");
        if (aa.freestanding) {
            keybind("Клавиша фристенда", aa.freestandKey);
            toggle("Статика на фристенде", &aa.freestandStatic);
            tooltip("Голова уже за укрытием — джиттер только выдал бы её.");
        }
        toggle("Edge yaw", &aa.edgeYaw);
        tooltip("Стена вплотную (до ~32 ед. от глаз) — встаём лицом в неё, голова уходит в стену.\nРаботает, когда нет ручного AA и фристенда.");
        if (aa.edgeYaw) {
            keybind("Клавиша edge yaw", aa.edgeKey);
            toggle("Статика на edge yaw", &aa.edgeStatic);
        }
    }
    endGroup();
    ImGui::EndGroup();

    ImGui::SameLine(0.0f, 12.0f);

    if (beginGroup("Что видит сервер", { width, height })) {
        drawServerPose(ImGui::GetContentRegionAvail().x);
    }
    endGroup();
}

// ---------- страница «Билдер» ----------
static void drawAaBuilder(float width, float height) noexcept
{
    auto& aa = config.antiAim;
    static int selected = 0;
    static AntiAimState clipboard;
    static bool haveClipboard = false;
    selected = std::clamp(selected, 0, static_cast<int>(aaStateNames.size()) - 1);

    const int count = static_cast<int>(aaStateNames.size());
    ImGui::BeginGroup();
    if (beginGroup("Состояния", { width, rowsHeight(count + 3) })) {
        for (int i = 0; i < count; ++i) {
            const bool isDefault = i == static_cast<int>(AaState::Default);
            const char* note = isDefault ? "основа" : aa.states[i].override ? "своё" : "как общее";
            if (listRow(aaStateNames[i], selected == i, note))
                selected = i;
        }
        ImGui::Dummy({ 0.0f, 4.0f });
        hint("Состояние без своих настроек берёт «Общее». «Фейклаг» — когда ни DT, ни дефенсив не включены.");
    }
    endGroup();
    ImGui::Dummy({ 0.0f, 10.0f });

    const float left = std::max(height - rowsHeight(count + 3) - 10.0f, rowsHeight(2));
    if (beginGroup("Флики", { width, left })) {
        toggle("Флики", &aa.flicks);
        tooltip("Рывок yaw вбок на тиках, которые враг не может разрезольвить.");
        if (aa.flicks) {
            static constexpr const char* triggers[]{ "При дефенсиве", "Всегда" };
            combo("Когда", &aa.flickTrigger, triggers, IM_ARRAYSIZE(triggers));
            tooltip("«При дефенсиве» — на тиках рывка (дефенсив, телепорт, сдвиг DT) и пару тиков после:\n"
                    "в это время его лагкомп по тебе не работает. «Всегда» — ещё и раз в N пакетов.");
            static constexpr const char* sides[]{ "Влево/вправо", "Рандом", "Свой yaw" };
            combo("Сторона", &aa.flickSide, sides, IM_ARRAYSIZE(sides));
            if (aa.flickSide == static_cast<int>(FlickSide::Custom))
                sliderInt("Yaw флика", &aa.flickYaw, -180, 180, "%+d°");
            else
                sliderInt("Угол флика", &aa.flickAngle, 30, 180, "%d°");
            toggle("Свой pitch", &aa.flickPitchOverride);
            if (aa.flickPitchOverride)
                sliderInt("Pitch флика", &aa.flickPitch, -89, 89, "%d°");
            if (aa.flickTrigger == static_cast<int>(FlickTrigger::Always))
                sliderInt("Раз в N пакетов", &aa.flickEvery, 3, 32);
        }
    }
    endGroup();
    ImGui::EndGroup();

    ImGui::SameLine(0.0f, 12.0f);

    char title[64];
    std::snprintf(title, sizeof(title), "Углы: %s", aaStateNames[selected]);
    if (beginGroup(title, { width, height })) {
        auto& st = aa.states[selected];
        const bool isDefault = selected == static_cast<int>(AaState::Default);

        const float third = (ImGui::GetContentRegionAvail().x - 8.0f) / 3.0f;
        if (ImGui::Button("Копировать", { third, 0.0f })) {
            clipboard = st;
            haveClipboard = true;
        }
        ImGui::SameLine(0.0f, 4.0f);
        if (ImGui::Button("Вставить", { third, 0.0f }) && haveClipboard) {
            st = clipboard;
            st.override = !isDefault;   // вставили в состояние — значит, хотят свои настройки
        }
        ImGui::SameLine(0.0f, 4.0f);
        if (ImGui::Button("Сброс", { third, 0.0f })) {
            const bool keep = st.override;
            st = {};
            st.override = keep;
        }
        ImGui::Dummy({ 0.0f, 4.0f });

        if (!isDefault)
            toggle("Свои настройки", &st.override);

        if (!isDefault && !st.override) {
            hint("В этом состоянии работает «Общее». Включи «Свои настройки», чтобы задать углы отдельно.");
        } else {
            sliderInt("Смещение yaw", &st.yawOffset, -180, 180, "%d°");
            sliderInt("Добавка влево", &st.addLeft, -90, 90, "%+d°");
            sliderInt("Добавка вправо", &st.addRight, -90, 90, "%+d°");
            tooltip("Сдвиг yaw по текущей стороне десинка: с джиттер-десинком голова уходит\nза телом, а не стоит посередине.");

            sectionTitle("Модификатор");
            static constexpr const char* modifiers[]{ "Выкл", "Джиттер", "Центр-джиттер", "Спин", "Рандом", "N-way", "Вращение", "Скиттер" };
            combo("Тип", &st.modifier, modifiers, IM_ARRAYSIZE(modifiers));
            const auto modifier = static_cast<YawModifier>(st.modifier);
            if (modifier == YawModifier::Spin) {
                sliderInt("Скорость спина", &st.spinSpeed, 1, 60);
            } else if (modifier != YawModifier::Off) {
                sliderInt("Размах", &st.degree, 0, 180, "%d°");
                if (modifier == YawModifier::FiveWay)
                    sliderInt("Положений", &st.ways, 3, 8);
                if (modifier == YawModifier::Rotate)
                    sliderInt("Скорость вращения", &st.spinSpeed, 1, 60);
                sliderInt("Рандомизация", &st.randomize, 0, 45, "±%d°");
                tooltip("На каждом переключении размах меняется на случайную величину в этих пределах.");
            }

            sectionTitle("Десинк");
            static constexpr const char* desyncModes[]{ "Выкл", "Статика", "Джиттер", "Рандом" };
            combo("Режим", &st.desyncMode, desyncModes, IM_ARRAYSIZE(desyncModes));
            if (st.desyncMode != static_cast<int>(DesyncMode::Off)) {
                sliderInt("Влево", &st.desyncLeft, 0, 60, "%d°");
                sliderInt("Вправо", &st.desyncRight, 0, 60, "%d°");
                if (st.desyncMode == static_cast<int>(DesyncMode::Static))
                    toggle("Другая сторона", &st.desyncInvert);
            }

            sectionTitle("Ритм");
            static constexpr const char* delays[]{ "Нет", "Статичная", "Плавающая", "Случайная", "Качели" };
            combo("Задержка", &st.delayMode, delays, IM_ARRAYSIZE(delays));
            tooltip("Модификатор и джиттер десинка шагают раз в N отправленных пакетов.\n"
                    "«Плавающая» — 1, 2, … N по кругу; «Случайная» — от 1 до N; «Качели» — N и 1 по очереди.\n"
                    "Сбивает ритм, по которому его резольвер угадывает фазу.");
            if (st.delayMode != static_cast<int>(SwitchDelay::None))
                sliderInt("Пакетов", &st.delayTicks, 1, 16);
        }
    }
    endGroup();
}

// ---------- страница «Десинк» ----------
static void drawAaDesync(float width, float height) noexcept
{
    auto& aa = config.antiAim;

    ImGui::BeginGroup();
    if (beginGroup("Десинк", { width, rowsHeight(4) })) {
        toggle("Десинк", &aa.desync);
        tooltip("Тело на сервере уведено от взгляда. Живёт на чокнутых тиках — фейклаг держит минимум один.");
        keybind("Инвертер", aa.inverter);
        sliderInt("Extended yaw", &aa.extendedYaw, 0, 180, aa.extendedYaw ? "%d°" : "выкл");
        tooltip("Стоя на месте тянем LBY за собой, как в Persistence/NL: чокнутая команда несёт yaw,\n"
                "повёрнутый на это значение в сторону десинка, и микродвижение — сервер считает нас\n"
                "идущими и ставит LBY ровно сюда. Отправленная команда несёт настоящий взгляд.\n"
                "Работает только стоя на земле и при включённом десинке. 120° — обычное значение.");
        sliderInt("Roll (наклон)", &aa.roll, -50, 50, aa.roll ? "%+d°" : "выкл");
        tooltip("Наклон модели вбок. Знак — сторона, инвертер меняет её. В движении и в воздухе\n"
                "ограничен ±40°. Пока roll включён — модификатор и дефенсив-углы выключены.\n"
                "На валв-серверах roll режется — это для комьюнити-HvH.");
    }
    endGroup();
    ImGui::Dummy({ 0.0f, 10.0f });

    const float left = std::max(height - rowsHeight(4) - 10.0f, rowsHeight(2));
    if (beginGroup("Фейклаг", { width, left })) {
        combo("Режим", &aa.fakeLagMode, fakeLagModeNames.data(), static_cast<int>(fakeLagModeNames.size()));
        tooltip("Статика — ровно столько тиков, сколько на ползунке.\n"
                "Случайно — каждый чок свой, от 1 до ползунка: ровный рисунок чока враги читают.\n"
                "Адаптивный — максимум, при котором шаг за чок остаётся под 64 юнитами:\n"
                "дальше сервер считает это телепортом и перестаёт честно откатывать.\n"
                "Ломать лагкомп — наоборот, перешагнуть эти 64 юнита: запись по нам сервер\n"
                "не интерполирует. На земле скорости не хватает — работает в полёте и на распрыжке.");
        sliderInt("Тиков", &aa.fakeLag, 1, 14, "%d тик.");
        sliderInt("В воздухе", &aa.fakeLagAir, 0, 14, aa.fakeLagAir > 0 ? "%d тик." : "как обычно");
        sliderInt("Пока стоим", &aa.fakeLagStanding, 0, 14, aa.fakeLagStanding > 0 ? "%d тик." : "как обычно");
        ImGui::Dummy({ 0.0f, 6.0f });
        if (tickbase::fastFireEnabled())
            hint("DT в режиме «Быстрая стрельба» — фейклаг ограничен 1 тиком: серверу нужен запас под сдвиг.");
        else
            hint("При включённом DT фейклаг ограничивается 1 тиком автоматически.");
    }
    endGroup();
    ImGui::EndGroup();

    ImGui::SameLine(0.0f, 12.0f);

    ImGui::BeginGroup();
    if (beginGroup("Анимации", { width, rowsHeight(5) })) {
        toggle("Слайд-волк", &aa.slideWalk);
        tooltip("Ноги не крутятся при ходьбе — это видят и враги.");
        toggle("Аним брейкер", &aa.animBreaker);
        if (aa.animBreaker) {
            static constexpr const char* breakModes[]{ "Выкл", "Статика", "Мунвок" };
            combo("Ноги на земле", &aa.breakerGround, breakModes, IM_ARRAYSIZE(breakModes));
            tooltip("Статика — ноги не шагают (включает слайд-волк, это видят и враги).\nМунвок — у себя ноги идут задом.");
            combo("Ноги в воздухе", &aa.breakerAir, breakModes, IM_ARRAYSIZE(breakModes));
            toggle("Без кивка при приземлении", &aa.breakerLandPitch);
        }
    }
    endGroup();
    ImGui::Dummy({ 0.0f, 10.0f });

    const float rest = std::max(height - rowsHeight(5) - 10.0f, rowsHeight(2));
    if (beginGroup("Проверка у себя", { width, rest })) {
        auto& view = config.view;
        toggle("Показывать AA в 3-м лице", &view.showFakeAngles);
        toggle("Модель сервера", &view.serverModel);
        tooltip("В 3-м лице поверх своей модели — призрак, как тебя видит сервер: ноги и тело\n"
                "уведены десинком, голова — по отправленному взгляду, наклон — roll.");
        if (view.serverModel)
            colorEdit("Цвет призрака", view.serverModelColor);
        colorToggle("Позиция на сервере", &view.serverBox, view.serverBoxColor);
        tooltip("Пока пакеты в чоке (лаг-пик, фейклаг) — коробка там, где модель стоит\n"
                "на сервере, и сколько тиков в чоке. Видно и от 1-го лица.");
        ImGui::Dummy({ 0.0f, 6.0f });
        hint("Углы по состояниям — на странице «Билдер». Скрытые углы рывков — на «Дефенсиве».");
    }
    endGroup();
    ImGui::EndGroup();
}

// ---------- страница «Дефенсив» ----------
// Окно дефенсива — тики, когда тикбейс откатился и сервер не пишет по нам лагкомп:
// враг видит эти углы, но ни бектрекнуть, ни разрезольвить их не может.
static void drawAaDefensive(float width, float height) noexcept
{
    auto& aa = config.antiAim;
    auto& df = config.defensive;

    ImGui::BeginGroup();
    if (beginGroup("Окно", { width, rowsHeight(9) })) {
        toggle("Включить дефенсив", &df.enabled);
        tooltip("Рывок тикбейса: сервер видит, что наш m_nTickBase ушёл дальше допустимого, и откатывает\n"
                "его назад. Пока тикбейс ниже уже виденного максимума, новых записей лагкомпа по нам нет.\n\n"
                "Заряд у дефенсива свой, галка DT не нужна. Исключение — DT в «Быстрой стрельбе»:\n"
                "там заряд принадлежит второй пуле, и дефенсив молчит.");
        static constexpr const char* triggerNames[]{ "Пик", "Пока видят", "Враг рядом", "По клавише", "После выстрела" };
        multiCombo("Когда рвать", &df.triggers, triggerNames, IM_ARRAYSIZE(triggerNames));
        tooltip("Пик — вот-вот выйдем врагу на глаза (записи лагкомпа на выглядывании у него нет).\n"
                "«Пока видят» — держать окна всё время, пока нас видно.\n"
                "«Враг рядом» — даже когда не видит (живее, но заряд тратится впустую).\n"
                "«После выстрела» — разрешить рывок сразу после своего выстрела.");
        toggle("В воздухе", &df.inAir);
        tooltip("В прыжке окна всегда: лагкомп по нам в воздухе не пишется.\n"
                "С DT заряд под это уступает (вторая пуля в прыжке всё равно не попадает).");
        if (df.triggers & DefKey)
            keybind("Клавиша", df.key);
        if (df.triggers & DefPeek)
            sliderInt("Предсказание пика", &df.peekTicks, 2, 16, "%d тик.");
        combo("Стиль заряда", &df.chargeStyle, chargeStyleNames.data(), static_cast<int>(chargeStyleNames.size()));
        tooltip("Чем платим за окно.\n"
                "«Чоком» — команда создаётся, но уходит позже: движение и мышь не теряются, а\n"
                "серверная модель просто отстаёт от нашей на длину чока. Пачка и есть окно,\n"
                "интервал тут не нужен.\n"
                "«Пропуском тиков» — вызов CL_Move пропускается, команды нет вовсе: персонаж на\n"
                "этом тике не идёт (при «1 из 3» это треть скорости), а рывок потом прогоняет пачку\n"
                "за один тик — отсюда и остановки, и отдача назад.");
        const bool choking = static_cast<ChargeStyle>(df.chargeStyle) == ChargeStyle::Choke;
        sliderInt(choking ? "Тиков чока" : "Тиков заряда", &df.chargeTicks, 4, 14);
        tooltip(choking
            ? "Длина пачки. Она же — и длина окна, и то, насколько серверная модель отстаёт от нашей.\n"
              "Сервер обрабатывает за свой тик не больше sv_maxusrcmdprocessticks команд, а хвост\n"
              "выкидывает — и тогда игрока тянет назад. Cvar серверный и обычно не виден клиенту,\n"
              "поэтому без него пачка держится в 8 командах (чок 7) — столько проходит и там, где\n"
              "лимит урезан. Если cvar реплицирован, берётся его значение, и ползунок работает весь."
            : "Сколько тиков копить под рывок. Сервер откатывает тикбейс, как только промах больше\n"
              "sv_clockcorrection_msecs (~60 мс ≈ 4 тика) — полные 13 ему не нужны.\n"
              "Меньше заряд → короче зарядка и чаще окна, но само окно короче.");
        sliderInt("Плавность зарядки", &df.chargeSpread, 1, 6, "1 из %d тик.");
        tooltip("Зарядка — это пропуск вызова CL_Move: на пропущенном тике не создаётся команда,\n"
                "персонаж стоит. Подряд (1) — 10 таких тиков = 150 мс без управления, это и есть\n"
                "«дёргает и не даёт двигаться». 3-4 — пропуски по одному, каждый по 15 мс.\n"
                "Стоя пропуски всё равно идут подряд: там их не видно. Этот же ползунок задаёт\n"
                "плавность зарядки второй пули DT (для неё минимум 3).");
        if (!choking) {
            sliderInt("Интервал между рывками", &df.interval, 1, 64, "%d тик.");
            tooltip("Минимум тиков между двумя рывками — как mode_duration у Neverlose: дефенсив работает\n"
                    "импульсами, а не очередью. 8-16 — рабочий компромисс.");
        }
    }
    endGroup();
    ImGui::Dummy({ 0.0f, 10.0f });

    const float left = std::max(height - rowsHeight(9) - 10.0f, rowsHeight(2));
    if (beginGroup("Замер по серверу", { width, left })) {
        if (!df.enabled) {
            hint("Выключен: рывков нет, заряд не копится, тики не пропускаются.");
        } else if (tickbase::fastFireEnabled()) {
            hint("DT в режиме «Быстрая стрельба» — заряд принадлежит второй пуле, дефенсив молчит.\n"
                 "Переключи DT на «Дефенсив» или выключи его совсем.");
        } else if (!tickbase::isInstalled()) {
            hint("Детур CL_Move не встал — рывки невозможны.");
        } else {
            char text[48];
            std::snprintf(text, sizeof(text), "%d%%", defensive::chargePercent());
            readout("Заряд", text);
            std::snprintf(text, sizeof(text), "%d из %d тик.", defensive::windowLeft(), defensive::windowLength());
            readout("Окно сейчас", text, defensive::windowLeft() > 0 ? color::good : color::textDim);
            std::snprintf(text, sizeof(text), "%d", defensive::pulseCount());
            readout("Рывков", text);
            const float rollback = defensive::measuredRollback();
            switch (defensive::verdict()) {
            case defensive::Verdict::RollsBack:
                std::snprintf(text, sizeof(text), "%.1f тик.", rollback);
                readout("Откат сервера", text, color::good);
                break;
            case defensive::Verdict::Ignores:
                readout("Откат сервера", "нет", color::bad);
                break;
            default:
                readout("Откат сервера", defensive::pulseCount() == 0 ? "замера не было" : "проверяем", color::textDim);
                break;
            }
            hint(defensive::statusLine());
        }
    }
    endGroup();
    ImGui::EndGroup();

    ImGui::SameLine(0.0f, 12.0f);

    static int selected = 0;
    static DefensiveSnap clipboard;
    static bool haveClipboard = false;
    selected = std::clamp(selected, 0, static_cast<int>(snapStateNames.size()) - 1);

    if (beginGroup("Скрытые углы", { width, height })) {
        toggle("Включить скрытые углы", &aa.defensiveAa);
        tooltip("Какие pitch и yaw показывать в окне дефенсива. Враг их видит, но бектрекнуть\n"
                "или разрезольвить не может — записей лагкомпа на эти тики у него нет.");

        combo("Состояние##snap", &selected, snapStateNames.data(), static_cast<int>(snapStateNames.size()));
        auto& s = aa.snaps[selected];
        const bool isDefault = selected == static_cast<int>(SnapState::Default);
        if (isDefault) {
            static constexpr const char* modes[]{ "Выкл", "Своё" };
            int mode = s.mode == static_cast<int>(SnapMode::Custom) ? 1 : 0;
            if (combo("Режим", &mode, modes, IM_ARRAYSIZE(modes)))
                s.mode = mode ? static_cast<int>(SnapMode::Custom) : static_cast<int>(SnapMode::Default);
        } else {
            static constexpr const char* modes[]{ "Как общее", "Выкл", "Своё" };
            combo("Режим", &s.mode, modes, IM_ARRAYSIZE(modes));
        }

        const float half = (ImGui::GetContentRegionAvail().x - 4.0f) * 0.5f;
        if (ImGui::Button("Копировать##snap", { half, 0.0f })) {
            clipboard = s;
            haveClipboard = true;
        }
        ImGui::SameLine(0.0f, 4.0f);
        if (ImGui::Button("Вставить##snap", { half, 0.0f }) && haveClipboard)
            s = clipboard;
        ImGui::Dummy({ 0.0f, 4.0f });

        if (s.mode != static_cast<int>(SnapMode::Custom)) {
            hint(isDefault ? "Выкл — в окне дефенсива остаются обычные углы AA."
                           : s.mode == static_cast<int>(SnapMode::Off) ? "В этом состоянии скрытых углов нет."
                                                                       : "Работают настройки «Общего».");
        } else {
            static constexpr const char* pitchTypes[]{ "Выкл", "Статика", "Джиттер", "Рандом", "Рандом-статика", "Спин", "Спин за окно", "Камера", "На цель" };
            combo("Pitch##snap", &s.pitch, pitchTypes, IM_ARRAYSIZE(pitchTypes));
            tooltip("Статика — угол 1. Джиттер — угол 1 / угол 2. Рандом — между ними каждый тик,\n"
                    "Рандом-статика — один случайный на окно. Спин — от 1 к 2 по кругу,\n"
                    "Спин за окно — от 1 к 2 ровно за окно. Камера / На цель — куда смотришь / на врага.");
            const auto pitch = static_cast<SnapPitch>(s.pitch);
            if (pitch != SnapPitch::Off && pitch != SnapPitch::Camera && pitch != SnapPitch::AtTarget) {
                sliderInt("Угол 1##snappitch", &s.pitchAngle, -89, 89, "%d°");
                if (pitch != SnapPitch::Static)
                    sliderInt("Угол 2##snappitch", &s.pitchAngle2, -89, 89, "%d°");
                if (pitch == SnapPitch::Spin)
                    sliderInt("Скорость##snappitch", &s.pitchSpeed, 1, 100);
            }

            sectionTitle("Yaw");
            static constexpr const char* yawTypes[]{ "Выкл", "Статика", "Джиттер", "Рандом", "Рандом-джиттер", "Рандом-статика", "Спин", "Спин за окно", "Спин-джиттер", "90 качели", "180 волна", "Наоборот (у стены)" };
            combo("Тип##snapyaw", &s.yaw, yawTypes, IM_ARRAYSIZE(yawTypes));
            tooltip("Угол считается от направления на врага: 180 — спиной (обычный AA), 0 — лицом.\n"
                    "«Наоборот (у стены)» — голова с открытой стороны, пока обычный фристенд прячет её:\n"
                    "приманка для его резольвера, а настоящие тики — за стеной.");
            const auto yaw = static_cast<SnapYaw>(s.yaw);
            if (yaw != SnapYaw::Off && yaw != SnapYaw::Opposite) {
                sliderInt(yaw == SnapYaw::Static ? "Угол##snapyaw" : "Размах##snapyaw", &s.yawAngle, 0, 360, "%d°");
                if (yaw == SnapYaw::Jitter || yaw == SnapYaw::SpinJitter)
                    sliderInt("Задержка##snapyaw", &s.yawDelay, 0, 10, "%d пак.");
                if (yaw == SnapYaw::Spin || yaw == SnapYaw::SpinJitter || yaw == SnapYaw::Way90 || yaw == SnapYaw::Sway180)
                    sliderInt("Скорость##snapyaw", &s.yawSpeed, 1, 100);
            }

            sliderInt("Длительность", &s.duration, 1, 13, "%d тик.");
            tooltip("Сколько тиков окна занимать скрытыми углами (13 — всё окно).");
        }

        if (aa.defensiveAa && !tickbase::fastFireEnabled()) {
            const bool anySnap = std::any_of(aa.snaps.begin(), aa.snaps.end(),
                [](const DefensiveSnap& sn) { return sn.mode == static_cast<int>(SnapMode::Custom); });
            if (!anySnap) {
                ImGui::Dummy({ 0.0f, 6.0f });
                hint("Ни у одного состояния не задано «Своё» — скрытых углов нет, и окна не запрашиваются.");
            }
        }
    }
    endGroup();
}

// ---------- страница «Анти-брут» ----------
static void drawAaAntiBrute(float width, float height) noexcept
{
    auto& aa = config.antiAim;

    if (beginGroup("Анти-брутфорс", { width, height })) {
        toggle("Анти-брутфорс", &aa.antiBrute);
        tooltip("Враг выстрелил рядом с нами (пуля прошла в 80 ед.) или мы выстрелили сами —\n"
                "его резольвер подбирал сторону под наш AA, меняем AA.");
        if (aa.antiBrute) {
            static constexpr const char* triggers[]{ "Враг выстрелил", "Мы выстрелили" };
            multiCombo("Когда", &aa.antiBruteTriggers, triggers, IM_ARRAYSIZE(triggers));
            static constexpr const char* modes[]{ "Смена стороны", "Подстройка", "Стойка пресетов", "Контр-резольвер" };
            combo("Что делать", &aa.antiBruteMode, modes, IM_ARRAYSIZE(modes));
            tooltip("Смена стороны — десинк на другую сторону.\n"
                    "Подстройка — углы билдера сдвигаются на случайную долю (до «Силы»).\n"
                    "Стойка пресетов — загружается следующий пресет из стойки справа.\n"
                    "Контр-резольвер — по импакту каждой пули врага видно, в какую сторону десинка он\n"
                    "целился; по истории выстрелов учится его резольвер (статика / перебор / следит / центр)\n"
                    "и на каждый цикл выбирает сторону и величину десинка, куда он вероятнее НЕ выстрелит.\n"
                    "Пока по врагу нет данных — работает билдер.");
            if (aa.antiBruteMode == static_cast<int>(AntiBruteMode::Counter)) {
                if (const char* style = antiaim::counterStyle(); style && *style)
                    ImGui::TextDisabled("Текущий враг: %s", style);
            }
            if (aa.antiBruteMode == static_cast<int>(AntiBruteMode::Adjust))
                sliderInt("Сила", &aa.antiBrutePower, 1, 100, "%d%%");
            if (aa.antiBruteMode == static_cast<int>(AntiBruteMode::PresetRack)) {
                static constexpr const char* orders[]{ "Случайно", "По очереди" };
                combo("Порядок", &aa.antiBruteOrder, orders, IM_ARRAYSIZE(orders));
            }
            sliderFloat("Вернуть как было через", &aa.antiBruteReset, 0.0f, 15.0f, aa.antiBruteReset > 0.0f ? "%.1f с" : "никогда");
        }
    }
    endGroup();

    ImGui::SameLine(0.0f, 12.0f);

    if (beginGroup("Стойка пресетов", { width, height })) {
        char text[64];
        std::snprintf(text, sizeof(text), "%d из 16", static_cast<int>(aa.presetRack.size()));
        readout("Пресетов в стойке", text);
        ImGui::Dummy({ 0.0f, 4.0f });

        const float half = (ImGui::GetContentRegionAvail().x - 4.0f) * 0.5f;
        if (ImGui::Button("Добавить текущий", { half, 0.0f }) && aa.presetRack.size() < 16)
            aa.presetRack.push_back({ aa.states, aa.snaps });
        ImGui::SameLine(0.0f, 4.0f);
        if (ImGui::Button("Очистить", { half, 0.0f }))
            aa.presetRack.clear();

        static int current = 0;
        if (!aa.presetRack.empty()) {
            current = std::clamp(current, 0, static_cast<int>(aa.presetRack.size()) - 1);
            ImGui::Dummy({ 0.0f, 4.0f });
            sliderInt("Пресет №", &current, 0, static_cast<int>(aa.presetRack.size()) - 1, "%d");
            const float third = (ImGui::GetContentRegionAvail().x - 8.0f) / 3.0f;
            if (ImGui::Button("Загрузить", { third, 0.0f })) {
                aa.states = aa.presetRack[current].states;
                aa.snaps = aa.presetRack[current].snaps;
            }
            ImGui::SameLine(0.0f, 4.0f);
            if (ImGui::Button("Заменить", { third, 0.0f }))
                aa.presetRack[current] = { aa.states, aa.snaps };
            ImGui::SameLine(0.0f, 4.0f);
            if (ImGui::Button("Удалить", { third, 0.0f }))
                aa.presetRack.erase(aa.presetRack.begin() + current);
        }

        ImGui::Dummy({ 0.0f, 6.0f });
        hint("Пресет — снимок всего билдера и дефенсив-углов. Настрой билдер, нажми «Добавить текущий»,\n"
             "перенастрой — добавь ещё. В режиме «Стойка пресетов» каждый промах врага загружает\n"
             "следующий. Стойка сохраняется в конфиг.");
    }
    endGroup();
}

static int aaPage = 0;

int& antiAimPage() noexcept
{
    return aaPage;
}

bool antiAimStatus(char* text, std::size_t size) noexcept
{
    if (!localPlayer || !localPlayer.get().isAlive() || !config.antiAim.enabled)
        return false;
    const int side = antiaim::desyncSide();
    std::snprintf(text, size, "%s · десинк %s%s", antiaim::stateName(),
        side > 0 ? "вправо" : side < 0 ? "влево" : "нет",
        antiaim::defensiveActive() ? " · дефенсив" : "");
    return true;
}

void drawAntiAimTab() noexcept
{
    const int page = aaPage;
    const float width = (ImGui::GetContentRegionAvail().x - 12.0f) * 0.5f;
    const float height = ImGui::GetContentRegionAvail().y;
    switch (page) {
    case 1: drawAaBuilder(width, height); break;
    case 2: drawAaDesync(width, height); break;
    case 3: drawAaDefensive(width, height); break;
    case 4: drawAaAntiBrute(width, height); break;
    default: drawAaAngles(width, height); break;
    }
}

// Подсказка мелким серым под строками карточки
static void cardNote(const char* text) noexcept
{
    ImGui::Dummy({ 0.0f, 2.0f });
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(color::textDim));
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x);
    ImGui::TextUnformatted(text);
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
}

// «Мир → Прицел и HUD»: прицел снайпера, гранаты, HUD чита, ESP вне видимости
void drawWorldGroup() noexcept
{
    auto& view = config.view;
    const float width = (ImGui::GetContentRegionAvail().x - 12.0f) * 0.5f;
    constexpr float gap = 10.0f;

    ImGui::BeginGroup();
    if (beginGroup("Прицел", { width, 0.0f })) {
        toggle("Без зума в прицеле", &view.removeScopeZoom);
        if (view.removeScopeZoom) {
            toggle("Второй зум приближает", &view.secondZoom);
            if (view.secondZoom)
                sliderFloat("Увеличение второго зума", &view.secondZoomFactor, 1.2f, 6.0f, "x%.1f");
        }
        toggle("Свой прицел снайпера", &view.customScope);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Градиентные линии вместо чёрного прицела (работает с «Без зума в прицеле»).");
        if (view.customScope) {
            sliderInt("Длина линий", &view.scopeSize, 10, 300);
            sliderInt("Отступ от центра", &view.scopeGap, 0, 100);
            toggle("T-прицел (без верхней)", &view.scopeT);
        }
        toggle("Круг разброса", &view.spreadCircle);
        toggle("Мин. урон у прицела", &view.damageIndicator);
    }
    endGroup();
    ImGui::Dummy({ 0.0f, gap });

    if (beginGroup("Гранаты", { width, 0.0f })) {
        toggle("Траектория гранаты", &view.grenadePrediction);
        toggle("Таймеры дыма и огня", &view.grenadeTimers);
    }
    endGroup();
    ImGui::EndGroup();

    ImGui::SameLine(0.0f, 12.0f);

    ImGui::BeginGroup();
    if (beginGroup("HUD", { width, 0.0f })) {
        toggle("Ватермарка", &view.watermark);
        toggle("Панель биндов (Hotkeys)", &view.hotkeys);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Активные бинды с клавишами, как HUD «Hotkeys» в NewerWin.\nПри открытом меню панель видна всегда — тащи за шапку.");
        toggle("Логи выстрелов на экране", &view.screenLogs);
        toggle("Индикатор LC", &view.lcIndicator);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Зелёный — сейчас идёт дефенсив (лагкомп сломан), полоска — сколько тиков осталось.");
        toggle("Предупреждение о замедлении", &view.slowdownWarning);
        toggle("Метки попаданий", &view.shotMarkers);
        if (view.shotMarkers) {
            toggle("Метки промахов", &view.missMarkers);
            sliderFloat("Время меток", &view.shotMarkerTime, 0.5f, 6.0f, "%.1f сек");
        }
    }
    endGroup();
    ImGui::Dummy({ 0.0f, gap });

    if (beginGroup("ESP вне видимости", { width, 0.0f })) {
        sliderFloat("Держать дормантов", &view.dormantTime, 1.5f, 20.0f, "%.1f сек");
        toggle("Ровный ESP", &view.syncRender);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Однопоточный рендер (mat_queue_mode 0): ESP рисуется по тому же кадру,\nчто на экране, и не дёргается. Может немного снизить FPS.");
        cardNote("Дальность ESP ограничена тем, что присылает сервер. Игроки вне видимости — по последней позиции и по звукам шагов и выстрелов.");
    }
    endGroup();
    ImGui::EndGroup();
}

void drawEspFlagsSetting() noexcept
{
    static constexpr const char* flagNames[]{ "Броня (HK/K)", "Деньги", "Кит / дефьюз", "Скоуп", "Перезарядка",
        "Ослеплён", "Бомба", "DT заряжен", "Ломает лагкомп", "Режим резольвера", "Убивает (LETHAL)" };
    multiCombo("Флаги ESP", &config.view.espFlags, flagNames, IM_ARRAYSIZE(flagNames));
}

void drawEspRageGroup(const ImVec2& size) noexcept
{
    auto& view = config.view;
    if (beginGroup("ESP и рейдж", size)) {
        drawEspFlagsSetting();
        toggle("Точки мультипоинта", &view.aimDot);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Точки мультипоинта, куда рейдж может выстрелить:\nзелёные — убивает, жёлтые — урон не ниже мин.\nКольцо — выбранная точка, красное — сюда выстрелил.");
        toggle("Скелет при выстреле", &view.shotCapsules);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Скелет цели в момент выстрела (2.5 с):\nзелёный — попал, красный — мимо, белый — ждём ответа сервера.");
        toggle("Снаряжение по Tab", &view.tabLoadout);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Иконки оружия, гранат, брони и кита/бомбы прямо в строке врага\nв таблице (Tab), посередине между ником и деньгами.");
    }
    endGroup();
}

// «Мир → Приколюхи»: эффекты, экран и руки, чат
void drawVisualsTab() noexcept
{
    auto& view = config.view;
    const float width = (ImGui::GetContentRegionAvail().x - 12.0f) * 0.5f;
    constexpr float gap = 10.0f;

    ImGui::BeginGroup();
    if (beginGroup("Эффекты", { width, 0.0f })) {
        toggle("Цифры урона", &view.damageNumbers);
        toggle("Кольца шагов врагов", &view.footsteps);
        toggle("Вспышка при убийстве", &view.killFlash);
    }
    endGroup();
    ImGui::Dummy({ 0.0f, gap });

    if (beginGroup("Чат", { width, 0.0f })) {
        toggle("Трештолк", &view.trashTalk);
        if (view.trashTalk) {
            static constexpr const char* when[]{ "При килле", "При смерти" };
            multiCombo("Когда писать", &view.trashTalkWhen, when, IM_ARRAYSIZE(when));
        }
    }
    endGroup();
    ImGui::EndGroup();

    ImGui::SameLine(0.0f, 12.0f);

    ImGui::BeginGroup();
    if (beginGroup("Экран", { width, 0.0f })) {
        toggle("Aspect ratio", &view.aspectRatio);
        if (view.aspectRatio) {
            sliderFloat("Соотношение", &view.aspectRatioValue, 0.8f, 2.4f, "%.2f");
            static constexpr const char* presets[]{ "5:4", "4:3", "3:2", "16:10", "16:9" };
            static constexpr float values[]{ 1.25f, 1.3333f, 1.5f, 1.6f, 1.7778f };
            ImGui::Dummy({ 0.0f, 2.0f });
            beginSegments();
            for (int i = 0; i < IM_ARRAYSIZE(presets); ++i) {
                if (i)
                    ImGui::SameLine(0.0f, 2.0f);
                if (pill(presets[i], std::abs(view.aspectRatioValue - values[i]) < 0.005f))
                    view.aspectRatioValue = values[i];
            }
            endSegments();
            ImGui::Dummy({ 0.0f, 2.0f });
        }
    }
    endGroup();
    ImGui::Dummy({ 0.0f, gap });

    if (beginGroup("Руки (вьюмодель)", { width, 0.0f })) {
        toggle("Своя вьюмодель", &view.viewmodel);
        if (view.viewmodel) {
            sliderInt("FOV рук", &view.viewmodelFov, 40, 120);
            sliderFloat("Смещение X", &view.viewmodelX, -10.0f, 10.0f, "%.1f");
            sliderFloat("Смещение Y", &view.viewmodelY, -10.0f, 10.0f, "%.1f");
            sliderFloat("Смещение Z", &view.viewmodelZ, -10.0f, 10.0f, "%.1f");
        }
    }
    endGroup();
    ImGui::EndGroup();
}

bool colorEdit3(const char* label, float* rgb) noexcept
{
    float rgba[4]{ rgb[0], rgb[1], rgb[2], 1.0f };
    const bool changed = colorEdit(label, rgba);
    if (changed) {
        rgb[0] = rgba[0];
        rgb[1] = rgba[1];
        rgb[2] = rgba[2];
    }
    return changed;
}

namespace
{

struct WorldPreset {
    const char* name;
    float wall[3], prop[3], sky[3];
    bool fog;
    float fogColor[3], fogStart, fogEnd, fogDensity;
    bool ambient;
    float ambientColor[3];
    const char* skybox;
};

constexpr WorldPreset worldPresets[]{
    { "Ночь",    { 0.22f, 0.22f, 0.30f }, { 0.40f, 0.40f, 0.50f }, { 0.25f, 0.25f, 0.45f }, true, { 0.05f, 0.06f, 0.12f }, 300.0f, 5000.0f, 0.50f, true, { 0.04f, 0.04f, 0.10f }, "sky_csgo_night02" },
    { "Закат",   { 0.95f, 0.68f, 0.55f }, { 1.00f, 0.75f, 0.62f }, { 1.00f, 0.60f, 0.45f }, true, { 1.00f, 0.55f, 0.35f }, 200.0f, 3500.0f, 0.35f, true, { 0.10f, 0.04f, 0.02f }, "sky_day02_05" },
    { "Неон",    { 0.35f, 0.25f, 0.60f }, { 0.70f, 0.35f, 1.00f }, { 0.70f, 0.30f, 1.00f }, true, { 0.55f, 0.10f, 0.90f },   0.0f, 2600.0f, 0.45f, true, { 0.16f, 0.00f, 0.28f }, "sky_lunacy" },
    { "Сакура",  { 1.00f, 0.72f, 0.85f }, { 1.00f, 0.80f, 0.90f }, { 1.00f, 0.65f, 0.85f }, true, { 1.00f, 0.60f, 0.80f }, 300.0f, 4000.0f, 0.30f, true, { 0.10f, 0.03f, 0.07f }, "sky_csgo_cloudy01" },
    { "Туман",   { 0.62f, 0.62f, 0.65f }, { 0.68f, 0.68f, 0.70f }, { 0.75f, 0.75f, 0.78f }, true, { 0.72f, 0.72f, 0.75f },   0.0f, 1500.0f, 0.85f, false, { 0.0f, 0.0f, 0.0f }, "sky_csgo_cloudy01" },
    { "Кислота", { 0.50f, 0.80f, 0.35f }, { 0.55f, 0.90f, 0.40f }, { 0.45f, 1.00f, 0.30f }, true, { 0.30f, 0.85f, 0.20f }, 100.0f, 2600.0f, 0.45f, true, { 0.03f, 0.12f, 0.02f }, "sky_l4d_rural02_ldr" },
    { "Лёд",     { 0.65f, 0.80f, 1.00f }, { 0.75f, 0.88f, 1.00f }, { 0.60f, 0.85f, 1.00f }, true, { 0.70f, 0.85f, 1.00f }, 200.0f, 3500.0f, 0.40f, true, { 0.02f, 0.05f, 0.10f }, "vertigoblue_hdr" },
};

void applyWorldPreset(const WorldPreset& p, bool* skyboxEnabled, int* skybox, const char* const* skyboxNames, int skyboxCount) noexcept
{
    auto& w = config.world;
    std::copy(std::begin(p.wall), std::end(p.wall), w.wallColor);
    std::copy(std::begin(p.prop), std::end(p.prop), w.propColor);
    std::copy(std::begin(p.sky), std::end(p.sky), w.skyColor);
    w.skyColorOn = true;
    w.fog = p.fog;
    std::copy(std::begin(p.fogColor), std::end(p.fogColor), w.fogColor);
    w.fogStart = p.fogStart;
    w.fogEnd = p.fogEnd;
    w.fogDensity = p.fogDensity;
    w.ambient = p.ambient;
    std::copy(std::begin(p.ambientColor), std::end(p.ambientColor), w.ambientColor);
    w.enabled = true;
    for (int i = 0; i < skyboxCount; ++i) {
        if (std::strcmp(skyboxNames[i], p.skybox) == 0) {
            *skybox = i;
            *skyboxEnabled = true;
            break;
        }
    }
}

}

// «Мир → Мир»: цвета карты, небо, туман, подсветка, искры
void drawWorldLookTab(bool* skyboxEnabled, int* skybox, const char* const* skyboxNames, int skyboxCount) noexcept
{
    auto& w = config.world;
    auto& view = config.view;
    const float width = (ImGui::GetContentRegionAvail().x - 12.0f) * 0.5f;
    constexpr float gap = 10.0f;

    ImGui::BeginGroup();
    if (beginGroup("Вид мира", { width, 0.0f })) {
        toggle("Включить", &w.enabled);
        const char* presetNames[IM_ARRAYSIZE(worldPresets) + 1]{ "Свой" };
        for (int i = 0; i < IM_ARRAYSIZE(worldPresets); ++i)
            presetNames[i + 1] = worldPresets[i].name;
        if (combo("Пресет", &w.preset, presetNames, IM_ARRAYSIZE(presetNames)) && w.preset > 0)
            applyWorldPreset(worldPresets[w.preset - 1], skyboxEnabled, skybox, skyboxNames, skyboxCount);
        if (colorEdit3("Стены", w.wallColor)) w.preset = 0;
        if (colorEdit3("Пропы", w.propColor)) w.preset = 0;
        sliderFloat("Прозрачность пропов", &w.propAlpha, 0.1f, 1.0f, "%.2f");
        cardNote("Пресет ставит всё сразу: цвета, туман, подсветку и небо. Новые текстуры карты докрашиваются раз в секунду.");
    }
    endGroup();
    ImGui::Dummy({ 0.0f, gap });

    if (beginGroup("Небо", { width, 0.0f })) {
        toggle("Цвет неба", &w.skyColorOn);
        if (w.skyColorOn && colorEdit3("Небо", w.skyColor)) w.preset = 0;
        toggle("Скайбокс", skyboxEnabled);
        if (*skyboxEnabled)
            combo("Небо (скайбокс)", skybox, skyboxNames, skyboxCount);
    }
    endGroup();
    ImGui::EndGroup();

    ImGui::SameLine(0.0f, 12.0f);

    ImGui::BeginGroup();
    if (beginGroup("Туман", { width, 0.0f })) {
        toggle("Свой туман", &w.fog);
        if (w.fog) {
            colorEdit3("Цвет тумана", w.fogColor);
            sliderFloat("Начало", &w.fogStart, 0.0f, 3000.0f, "%.0f ед.");
            sliderFloat("Конец", &w.fogEnd, 200.0f, 10000.0f, "%.0f ед.");
            sliderFloat("Плотность", &w.fogDensity, 0.0f, 1.0f, "%.2f");
        }
    }
    endGroup();
    ImGui::Dummy({ 0.0f, gap });

    if (beginGroup("Подсветка", { width, 0.0f })) {
        toggle("Подсветка", &w.ambient);
        if (w.ambient)
            colorEdit3("Цвет подсветки", w.ambientColor);
    }
    endGroup();
    ImGui::Dummy({ 0.0f, gap });

    if (beginGroup("Искры от пуль", { width, 0.0f })) {
        toggle("Искры", &view.impactSparks);
        if (view.impactSparks) {
            colorEdit("Цвет искр", view.sparkColor);
            sliderInt("Количество", &view.sparkCount, 4, 60);
            sliderFloat("Разлёт", &view.sparkSize, 0.3f, 3.0f, "x%.1f");
        }
    }
    endGroup();
    ImGui::EndGroup();
}

void drawAutoBuyTab() noexcept
{
    auto& b = config.autoBuy;
    const float width = (ImGui::GetContentRegionAvail().x - 12.0f) * 0.5f;
    if (beginGroup("Автобай", { width, ImGui::GetContentRegionAvail().y })) {
        toggle("Включить", &b.enabled);
        static constexpr const char* primaries[]{ "Ничего", "Скаут", "AWP", "Автоснайпа" };
        combo("Основное", &b.primary, primaries, IM_ARRAYSIZE(primaries));
        static constexpr const char* secondaries[]{ "Ничего", "Дигл / R8", "Дуалы", "P250", "Tec-9 / Five-SeveN" };
        combo("Пистолет", &b.secondary, secondaries, IM_ARRAYSIZE(secondaries));
        static constexpr const char* extras[]{ "Броня+шлем", "Зевс", "HE", "Дым", "Молотов", "Флешка", "Кусачки" };
        multiCombo("Ещё", &b.extras, extras, IM_ARRAYSIZE(extras));
        ImGui::Dummy({ 0.0f, 6.0f });
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(color::textDim));
        ImGui::TextWrapped("Покупает в начале каждого раунда, как только ты жив. Порядок: основное, броня, пистолет, зевс, гранаты, кусачки — не хватит денег, и дешёвое просто не купится. Дигл или R8 — что стоит в лоадауте.");
        ImGui::PopStyleColor();
    }
    endGroup();
}

void drawMovementTab() noexcept
{
    auto& m = config.movement;
    const float width = (ImGui::GetContentRegionAvail().x - 12.0f) * 0.5f;
    const float height = ImGui::GetContentRegionAvail().y;

    if (beginGroup("Пик и присед", { width, height })) {
        keybind("Автопик", m.autoPeekKey);
        static constexpr const char* peekModes[]{ "После выстрела", "Выстрел или отпустил WASD" };
        combo("Возврат", &m.autoPeekMode, peekModes, IM_ARRAYSIZE(peekModes));
        sliderInt("Тиков телепорта", &m.peekTeleportTicks, 6, 14);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Заряд под телепорт назад: 14 тиков — ~55 ед. на полной скорости,\n6 — ~27. Набирается за укрытием; выстрел DT тратит только «Тиков сдвига».\nЕсли сервер режет пачки команд (телепорт дёргает назад) — уменьши.");
        ImGui::Dummy({ 0.0f, 6.0f });
        keybind("Фейкдак", m.fakeDuckKey);
        if (!tickbase::isInstalled()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(color::bad));
            ImGui::TextWrapped("Фейкдак недоступен: CL_Move не найден");
            ImGui::PopStyleColor();
        }

        ImGui::Dummy({ 0.0f, 6.0f });
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(color::textDim));
        ImGui::TextWrapped("Автопик: держи клавишу — точка запоминается, после выстрела персонаж сам бежит назад. С включённым DT — разгон и телепорт назад на его заряде, без DT — просто бег назад. Фейкдак на время удержания выключает DT.");
        ImGui::PopStyleColor();
    }
    endGroup();

    ImGui::SameLine(0.0f, 12.0f);

    if (beginGroup("Прыжки и прочее", { width, height })) {
        toggle("Бхоп", &m.bunnyHop);
        static constexpr const char* strafeModes[]{ "Выкл", "По камере", "WASD" };
        combo("Автострейф", &m.autoStrafe, strafeModes, IM_ARRAYSIZE(strafeModes));
        toggle("Быстрая лестница", &m.fastLadder);
        toggle("Супертосс", &m.superToss);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Граната летит туда, куда смотрит прицел, даже на бегу и в прыжке:\n"
                              "в тик броска угол подбирается так, чтобы своя скорость не уводила её в сторону.\n"
                              "Траектория гранаты рисуется с учётом супертосса.");
        toggle("Без урона от падения", &m.noFallDamage);
        ImGui::Dummy({ 0.0f, 6.0f });
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(color::textDim));
        ImGui::TextWrapped("Автострейф в воздухе сам набирает скорость. «По камере» — летишь куда смотришь (S — назад), «WASD» — направление клавишами. Старые bunny hop / auto strafe из «Основное» выключи.");
        ImGui::PopStyleColor();
    }
    endGroup();
}

}

// ======================= размытие фона (OpenGL) =======================
// Кадр игры копируется из заднего буфера, уменьшается вчетверо (два шага ×2 с линейной
// фильтрацией) и проходит два раза раздельным гауссом 9 отсчётов (по горизонтали и вертикали).
// Всё, что трогаем в состоянии GL, сохраняется и возвращается: дальше рисует ImGui и сама игра.
namespace hvh::blur
{

namespace
{

bool requested = false;
bool failed = false;
int width = 0, height = 0;          // размер экрана, под который созданы буферы
GLuint program = 0, vao = 0;
GLint uniformTex = -1, uniformDir = -1;
// 0 — полный размер (снимок), 1 — половина, 2/3 — четверть (пинг-понг размытия)
GLuint fbo[4]{}, tex[4]{};
int texW[4]{}, texH[4]{};

GLuint compile(GLenum type, const char* source) noexcept
{
    const GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

bool createProgram() noexcept
{
    // полноэкранный треугольник без буферов вершин — координаты из номера вершины
    static constexpr const char* vertex =
        "#version 130\n"
        "out vec2 uv;\n"
        "void main() {\n"
        "    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));\n"
        "    uv = p;\n"
        "    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
        "}\n";
    // гаусс 9 отсчётов через 5 выборок с линейной фильтрацией между текселями
    static constexpr const char* fragment =
        "#version 130\n"
        "in vec2 uv;\n"
        "out vec4 outColor;\n"
        "uniform sampler2D source;\n"
        "uniform vec2 direction;\n"
        "void main() {\n"
        "    vec3 c = texture(source, uv).rgb * 0.2270270270;\n"
        "    c += texture(source, uv + direction * 1.3846153846).rgb * 0.3162162162;\n"
        "    c += texture(source, uv - direction * 1.3846153846).rgb * 0.3162162162;\n"
        "    c += texture(source, uv + direction * 3.2307692308).rgb * 0.0702702703;\n"
        "    c += texture(source, uv - direction * 3.2307692308).rgb * 0.0702702703;\n"
        "    outColor = vec4(c, 1.0);\n"
        "}\n";
    const GLuint vs = compile(GL_VERTEX_SHADER, vertex);
    const GLuint fs = compile(GL_FRAGMENT_SHADER, fragment);
    if (!vs || !fs)
        return false;
    program = glCreateProgram();
    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glBindAttribLocation(program, 0, "unused");
    glLinkProgram(program);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (!ok) {
        glDeleteProgram(program);
        program = 0;
        return false;
    }
    uniformTex = glGetUniformLocation(program, "source");
    uniformDir = glGetUniformLocation(program, "direction");
    glGenVertexArrays(1, &vao);
    return true;
}

void destroyTargets() noexcept
{
    glDeleteFramebuffers(4, fbo);
    glDeleteTextures(4, tex);
    for (int i = 0; i < 4; ++i)
        fbo[i] = tex[i] = 0;
    width = height = 0;
}

bool createTargets(int w, int h) noexcept
{
    destroyTargets();
    const int sizes[4][2]{ { w, h }, { std::max(w / 2, 1), std::max(h / 2, 1) },
                           { std::max(w / 4, 1), std::max(h / 4, 1) }, { std::max(w / 4, 1), std::max(h / 4, 1) } };
    glGenTextures(4, tex);
    glGenFramebuffers(4, fbo);
    for (int i = 0; i < 4; ++i) {
        texW[i] = sizes[i][0];
        texH[i] = sizes[i][1];
        glBindTexture(GL_TEXTURE_2D, tex[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, texW[i], texH[i], 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo[i]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex[i], 0);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
            return false;
    }
    width = w;
    height = h;
    return true;
}

void blurPass(int from, int to, float dx, float dy) noexcept
{
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo[to]);
    glViewport(0, 0, texW[to], texH[to]);
    glBindTexture(GL_TEXTURE_2D, tex[from]);
    glUniform2f(uniformDir, dx / static_cast<float>(texW[from]), dy / static_cast<float>(texH[from]));
    glDrawArrays(GL_TRIANGLES, 0, 3);
}

}

void request() noexcept
{
    requested = true;
}

void process() noexcept
{
    if (!requested || failed)
        return;
    requested = false;

    const auto& display = ImGui::GetIO().DisplaySize;
    const auto& scale = ImGui::GetIO().DisplayFramebufferScale;
    const int w = static_cast<int>(display.x * scale.x), h = static_cast<int>(display.y * scale.y);
    if (w <= 0 || h <= 0)
        return;

    // ---- сохраняем состояние GL ----
    GLint lastProgram, lastTexture, lastActive, lastVao, lastReadFb, lastDrawFb, lastViewport[4], lastArray, lastReadBuffer;
    glGetIntegerv(GL_CURRENT_PROGRAM, &lastProgram);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &lastActive);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &lastTexture);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &lastVao);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &lastArray);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &lastReadFb);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &lastDrawFb);
    glGetIntegerv(GL_VIEWPORT, lastViewport);
    const GLboolean lastBlend = glIsEnabled(GL_BLEND), lastScissor = glIsEnabled(GL_SCISSOR_TEST),
        lastDepth = glIsEnabled(GL_DEPTH_TEST), lastCull = glIsEnabled(GL_CULL_FACE), lastSrgb = glIsEnabled(GL_FRAMEBUFFER_SRGB);
    GLint lastSampler = 0;
    glGetIntegerv(GL_SAMPLER_BINDING, &lastSampler);

    bool ok = (program || createProgram()) && (width == w && height == h ? true : createTargets(w, h));
    if (ok) {
        glDisable(GL_BLEND);
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_CULL_FACE);
        glDisable(GL_FRAMEBUFFER_SRGB);
        glBindSampler(0, 0);

        // старые ошибки игры не наши — сбрасываем, чтобы проверить только свой снимок
        for (int i = 0; i < 16 && glGetError() != GL_NO_ERROR; ++i) {}

        // снимок кадра из того буфера, куда сейчас рисуется (туда же ляжет и меню),
        // и два шага уменьшения
        const GLuint source = static_cast<GLuint>(lastDrawFb);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, source);
        glGetIntegerv(GL_READ_BUFFER, &lastReadBuffer);
        glReadBuffer(source == 0 ? GL_BACK : GL_COLOR_ATTACHMENT0);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo[0]);
        glBlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        glReadBuffer(lastReadBuffer);
        if (glGetError() != GL_NO_ERROR)
            ok = false;
        for (int i = 0; i < 2; ++i) {
            glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo[i]);
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo[i + 1]);
            glBlitFramebuffer(0, 0, texW[i], texH[i], 0, 0, texW[i + 1], texH[i + 1], GL_COLOR_BUFFER_BIT, GL_LINEAR);
        }

        // размытие: 2 → 3 (гориз.), 3 → 2 (верт.), дважды с растущим шагом
        glUseProgram(program);
        glUniform1i(uniformTex, 0);
        glBindVertexArray(vao);
        for (const float step : { 1.0f, 2.0f }) {
            blurPass(2, 3, step, 0.0f);
            blurPass(3, 2, 0.0f, step);
        }
        if (glGetError() != GL_NO_ERROR)
            ok = false;
    }
    if (!ok) {
        failed = true;  // шейдер не собрался или FBO не создался — меню просто без размытия
    }

    // ---- возвращаем состояние ----
    glUseProgram(lastProgram);
    glBindTexture(GL_TEXTURE_2D, lastTexture);
    glActiveTexture(lastActive);
    glBindVertexArray(lastVao);
    glBindBuffer(GL_ARRAY_BUFFER, lastArray);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, lastReadFb);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, lastDrawFb);
    glViewport(lastViewport[0], lastViewport[1], lastViewport[2], lastViewport[3]);
    glBindSampler(0, lastSampler);
    if (lastBlend) glEnable(GL_BLEND); else glDisable(GL_BLEND);
    if (lastScissor) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
    if (lastDepth) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    if (lastCull) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
    if (lastSrgb) glEnable(GL_FRAMEBUFFER_SRGB); else glDisable(GL_FRAMEBUFFER_SRGB);
}

ImTextureID texture() noexcept
{
    return failed || !tex[2] ? nullptr : reinterpret_cast<ImTextureID>(static_cast<std::uintptr_t>(tex[2]));
}

void uv(const ImVec2& min, const ImVec2& max, ImVec2& uv0, ImVec2& uv1) noexcept
{
    // текстура снята из GL — строки снизу вверх
    const auto& display = ImGui::GetIO().DisplaySize;
    uv0 = { min.x / display.x, 1.0f - min.y / display.y };
    uv1 = { max.x / display.x, 1.0f - max.y / display.y };
}

void shutdown() noexcept
{
    if (width)
        destroyTargets();
    if (program)
        glDeleteProgram(program);
    if (vao)
        glDeleteVertexArrays(1, &vao);
    program = vao = 0;
}

}

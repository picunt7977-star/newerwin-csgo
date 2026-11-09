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

#include <InputUtil.h>
#include <CSGO/LocalPlayer.h>
#include <GameData.h>

namespace hvh::ui
{

namespace
{

constexpr float rowHeight = 19.0f;
constexpr float rounding = 0.0f;
constexpr float indent = 20.0f;   // подписи и поля сдвинуты под чекбокс, как в gamesense

ImVec4 toVec4(ImU32 col) noexcept
{
    return ImGui::ColorConvertU32ToFloat4(col);
}

ImU32 withAlpha(ImU32 col, float alpha) noexcept
{
    const auto a = static_cast<int>(((col >> IM_COL32_A_SHIFT) & 0xFF) * std::clamp(alpha, 0.0f, 1.0f));
    return (col & ~IM_COL32_A_MASK) | (static_cast<ImU32>(a) << IM_COL32_A_SHIFT);
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
    style.WindowRounding = 0.0f;
    style.ChildRounding = 0.0f;
    style.FrameRounding = rounding;
    style.PopupRounding = rounding;
    style.GrabRounding = rounding;
    style.ScrollbarRounding = rounding;
    style.TabRounding = rounding;
    style.WindowBorderSize = 0.0f;
    style.ChildBorderSize = 0.0f;
    style.PopupBorderSize = 1.0f;
    style.FrameBorderSize = 1.0f;
    style.WindowPadding = { 12.0f, 12.0f };
    style.FramePadding = { 8.0f, 3.0f };
    style.ItemSpacing = { 8.0f, 4.0f };
    style.ItemInnerSpacing = { 6.0f, 4.0f };
    style.ScrollbarSize = 6.0f;
    style.GrabMinSize = 8.0f;

    auto* c = style.Colors;
    c[ImGuiCol_Text] = toVec4(color::text);
    c[ImGuiCol_TextDisabled] = toVec4(color::textDim);
    c[ImGuiCol_WindowBg] = toVec4(color::windowBg);
    c[ImGuiCol_ChildBg] = toVec4(color::groupBg);
    c[ImGuiCol_PopupBg] = toVec4(IM_COL32(29, 29, 29, 255));
    c[ImGuiCol_Border] = toVec4(color::outline);
    c[ImGuiCol_FrameBg] = toVec4(color::frame);
    c[ImGuiCol_FrameBgHovered] = toVec4(color::frameHover);
    c[ImGuiCol_FrameBgActive] = toVec4(color::frameHover);
    c[ImGuiCol_TitleBg] = toVec4(color::sidebarBg);
    c[ImGuiCol_TitleBgActive] = toVec4(color::sidebarBg);
    c[ImGuiCol_MenuBarBg] = toVec4(color::sidebarBg);
    c[ImGuiCol_ScrollbarBg] = { 0, 0, 0, 0 };
    c[ImGuiCol_ScrollbarGrab] = toVec4(color::frameHover);
    c[ImGuiCol_ScrollbarGrabHovered] = toVec4(withAlpha(color::accent, 0.6f));
    c[ImGuiCol_ScrollbarGrabActive] = toVec4(color::accent);
    c[ImGuiCol_CheckMark] = toVec4(color::accent);
    c[ImGuiCol_SliderGrab] = toVec4(color::accent);
    c[ImGuiCol_SliderGrabActive] = toVec4(color::accent2);
    c[ImGuiCol_Button] = toVec4(color::frame);
    c[ImGuiCol_ButtonHovered] = toVec4(color::frameHover);
    c[ImGuiCol_ButtonActive] = toVec4(withAlpha(color::accent, 0.5f));
    c[ImGuiCol_Header] = toVec4(withAlpha(color::accent, 0.22f));
    c[ImGuiCol_HeaderHovered] = toVec4(withAlpha(color::accent, 0.32f));
    c[ImGuiCol_HeaderActive] = toVec4(withAlpha(color::accent, 0.45f));
    c[ImGuiCol_Separator] = toVec4(color::groupLine);
    c[ImGuiCol_Tab] = toVec4(color::frame);
    c[ImGuiCol_TabHovered] = toVec4(withAlpha(color::accent, 0.5f));
    c[ImGuiCol_TabActive] = toVec4(withAlpha(color::accent, 0.35f));
    c[ImGuiCol_TextSelectedBg] = toVec4(withAlpha(color::accent, 0.35f));
    c[ImGuiCol_NavHighlight] = toVec4(color::accent);
    c[ImGuiCol_ModalWindowDimBg] = { 0, 0, 0, 0.5f };
}

bool beginGroup(const char* title, const ImVec2& requestedSize) noexcept
{
    // Название лежит на верхней линии и вылезает над группой — у верхнего края оставляем ему место
    ImVec2 size = requestedSize;
    if (const float y = ImGui::GetCursorPosY(); y < 8.0f) {
        ImGui::SetCursorPosY(8.0f);
        size.y -= 8.0f - y;
    }
    // рамка и название рисуются в окне-родителе
    auto* parentList = ImGui::GetWindowDrawList();
    const auto pos = ImGui::GetCursorScreenPos();
    const ImVec2 max{ pos.x + size.x, pos.y + size.y };
    parentList->AddRectFilled(pos, max, color::groupBg);
    parentList->AddRect(pos, max, color::outline);
    parentList->AddRect({ pos.x + 1.0f, pos.y + 1.0f }, { max.x - 1.0f, max.y - 1.0f }, color::groupLine);
    ImFont* titleFont = fonts.menuBold ? fonts.menuBold : ImGui::GetFont();
    const float titleSize = titleFont->FontSize;
    const auto titleWidth = titleFont->CalcTextSizeA(titleSize, FLT_MAX, 0.0f, title, ImGui::FindRenderedTextEnd(title)).x;
    parentList->AddRectFilled({ pos.x + 10.0f, pos.y - 1.0f }, { pos.x + 18.0f + titleWidth, pos.y + 2.0f }, color::groupBg);
    parentList->AddText(titleFont, titleSize, { pos.x + 14.0f, pos.y - titleSize * 0.5f }, color::text, title, ImGui::FindRenderedTextEnd(title));

    ImGui::PushStyleColor(ImGuiCol_ChildBg, { 0.0f, 0.0f, 0.0f, 0.0f });
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, { 14.0f, 14.0f });
    const bool open = ImGui::BeginChild(title, size, false, ImGuiWindowFlags_AlwaysUseWindowPadding);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
    ImGui::PushItemWidth(-1.0f);
    return open;
}

void endGroup() noexcept
{
    ImGui::PopItemWidth();
    ImGui::Dummy({ 0.0f, 4.0f });
    ImGui::EndChild();
}

bool colorEdit(const char* label, float* rgba) noexcept
{
    auto* window = ImGui::GetCurrentWindow();
    if (window->SkipItems)
        return false;

    const auto pos = window->DC.CursorPos;
    const float width = ImGui::GetContentRegionAvail().x;
    const auto labelEnd = ImGui::FindRenderedTextEnd(label);
    window->DrawList->AddText({ pos.x + indent, pos.y + (rowHeight - ImGui::GetFontSize()) * 0.5f }, color::text, label, labelEnd);

    // прямоугольник цвета справа, по клику — палитра ImGui
    ImGui::SetCursorScreenPos({ pos.x + width - 20.0f, pos.y + 4.0f });
    ImGui::PushID(label);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, { 0.0f, 0.0f });
    ImGui::PushItemWidth(20.0f);
    const bool changed = ImGui::ColorEdit4("##color", rgba, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel | ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_AlphaPreviewHalf);
    ImGui::PopItemWidth();
    ImGui::PopStyleVar(2);
    ImGui::PopID();
    ImGui::SetCursorScreenPos(pos);
    ImGui::ItemSize(ImRect{ pos, { pos.x + width, pos.y + rowHeight } });
    return changed;
}

// Квадратик 8×8 с градиентом слева, подпись справа — как в gamesense
void checkboxBox(ImDrawList* d, const ImVec2& at, bool on, bool hovered) noexcept
{
    const ImVec2 max{ at.x + 8.0f, at.y + 8.0f };
    if (on)
        d->AddRectFilledMultiColor(at, max, color::accent, color::accent, color::accent2, color::accent2);
    else
        d->AddRectFilledMultiColor(at, max, hovered ? IM_COL32(80, 80, 80, 255) : IM_COL32(68, 68, 68, 255), hovered ? IM_COL32(80, 80, 80, 255) : IM_COL32(68, 68, 68, 255), IM_COL32(48, 48, 48, 255), IM_COL32(48, 48, 48, 255));
    d->AddRect({ at.x - 1.0f, at.y - 1.0f }, { max.x + 1.0f, max.y + 1.0f }, color::outline);
}

bool toggle(const char* label, bool* value) noexcept
{
    auto* window = ImGui::GetCurrentWindow();
    if (window->SkipItems)
        return false;

    const auto id = window->GetID(label);
    const auto pos = window->DC.CursorPos;
    const auto labelEnd = ImGui::FindRenderedTextEnd(label);
    const float labelWidth = ImGui::CalcTextSize(label, labelEnd).x;
    const ImRect bb{ pos, { pos.x + indent + labelWidth + 4.0f, pos.y + rowHeight } };

    ImGui::ItemSize(ImRect{ pos, { pos.x + ImGui::GetContentRegionAvail().x, pos.y + rowHeight } });
    if (!ImGui::ItemAdd(bb, id))
        return false;

    bool hovered, held;
    const bool pressed = ImGui::ButtonBehavior(bb, id, &hovered, &held);
    if (pressed) {
        *value = !*value;
        ImGui::MarkItemEdited(id);
    }

    auto* drawList = window->DrawList;
    checkboxBox(drawList, { pos.x, pos.y + (rowHeight - 8.0f) * 0.5f }, *value, hovered);
    drawList->AddText({ pos.x + indent, pos.y + (rowHeight - ImGui::GetFontSize()) * 0.5f }, color::text, label, labelEnd);
    return pressed;
}

template <typename T>
static bool slider(const char* label, T* value, T min, T max, const char* format) noexcept
{
    auto* window = ImGui::GetCurrentWindow();
    if (window->SkipItems)
        return false;

    const auto id = window->GetID(label);
    const auto pos = window->DC.CursorPos;
    const float width = ImGui::GetContentRegionAvail().x;
    const float height = 32.0f;
    const ImRect bb{ pos, { pos.x + width, pos.y + height } };
    const ImRect track{ { pos.x + indent, pos.y + 18.0f }, { pos.x + width, pos.y + 24.0f } };

    ImGui::ItemSize(bb);
    if (!ImGui::ItemAdd(bb, id))
        return false;

    bool hovered, held;
    ImGui::ButtonBehavior(ImRect{ { bb.Min.x, track.Min.y - 8.0f }, { bb.Max.x, track.Max.y + 8.0f } }, id, &hovered, &held);

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

    auto* drawList = window->DrawList;
    const auto labelEnd = ImGui::FindRenderedTextEnd(label);
    drawList->AddText({ pos.x + indent, pos.y + 1.0f }, color::text, label, labelEnd);

    char buffer[32];
    if constexpr (std::is_integral_v<T>)
        std::snprintf(buffer, sizeof(buffer), format, *value);
    else
        std::snprintf(buffer, sizeof(buffer), format, static_cast<double>(*value));
    const auto valueSize = ImGui::CalcTextSize(buffer);

    // полоса с градиентом в чёрной рамке, число — на конце заливки, с тенью
    const float ratio = std::clamp(static_cast<float>(*value - min) / static_cast<float>(max - min), 0.0f, 1.0f);
    const float fillX = track.Min.x + track.GetWidth() * ratio;
    drawList->AddRectFilledMultiColor(track.Min, track.Max, IM_COL32(52, 52, 52, 255), IM_COL32(52, 52, 52, 255), IM_COL32(68, 68, 68, 255), IM_COL32(68, 68, 68, 255));
    drawList->AddRectFilledMultiColor(track.Min, { fillX, track.Max.y }, color::accent, color::accent, color::accent2, color::accent2);
    drawList->AddRect({ track.Min.x - 1.0f, track.Min.y - 1.0f }, { track.Max.x + 1.0f, track.Max.y + 1.0f }, hovered || held ? IM_COL32(70, 70, 70, 255) : color::outline);
    const float textX = std::clamp(fillX - valueSize.x * 0.5f, track.Min.x, track.Max.x - valueSize.x);
    drawList->AddText({ textX + 1.0f, track.Min.y - 3.0f }, color::outline, buffer);
    drawList->AddText({ textX, track.Min.y - 4.0f }, IM_COL32(255, 255, 255, 255), buffer);

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

static void smallLabel(const char* label) noexcept
{
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + indent);
    ImGui::TextUnformatted(label, ImGui::FindRenderedTextEnd(label));
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + indent);
}

bool combo(const char* label, int* value, const char* const items[], int count) noexcept
{
    smallLabel(label);
    ImGui::PushID(label);
    ImGui::SetNextItemWidth(-1.0f);
    const bool changed = ImGui::Combo("##combo", value, items, count);
    ImGui::PopID();
    ImGui::Dummy({ 0.0f, 2.0f });
    return changed;
}

bool multiCombo(const char* label, int* flags, const char* const items[], int count) noexcept
{
    smallLabel(label);

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
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::BeginCombo("##multi", preview.c_str())) {
        for (int i = 0; i < count; ++i) {
            const bool selected = *flags & (1 << i);
            if (ImGui::Selectable(items[i], selected, ImGuiSelectableFlags_DontClosePopups)) {
                *flags ^= 1 << i;
                changed = true;
            }
        }
        ImGui::EndCombo();
    }
    ImGui::PopID();
    ImGui::Dummy({ 0.0f, 2.0f });
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

    const bool listening = ImGui::GetActiveID() == id;
    char keyName[48];
    std::snprintf(keyName, sizeof(keyName), "[%s]", listening ? "..." : (key.isSet() ? key.toString() : "-"));
    const auto keySize = ImGui::CalcTextSize(keyName);
    const ImRect button{ { bb.Max.x - keySize.x - 4.0f, pos.y }, { bb.Max.x, pos.y + rowHeight } };

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

    auto* drawList = window->DrawList;
    drawList->AddText({ pos.x + indent, pos.y + (rowHeight - ImGui::GetFontSize()) * 0.5f }, color::text, label, ImGui::FindRenderedTextEnd(label));
    drawList->AddText({ button.Min.x + 4.0f, pos.y + (rowHeight - ImGui::GetFontSize()) * 0.5f }, listening ? color::accent : (hovered ? color::text : IM_COL32(110, 110, 110, 255)), keyName);
}

bool pill(const char* label, bool selected) noexcept
{
    auto* window = ImGui::GetCurrentWindow();
    const auto id = window->GetID(label);
    const auto textSize = ImGui::CalcTextSize(label, ImGui::FindRenderedTextEnd(label));
    const auto pos = window->DC.CursorPos;
    const ImRect bb{ pos, { pos.x + textSize.x + 20.0f, pos.y + 24.0f } };

    ImGui::ItemSize(bb);
    if (!ImGui::ItemAdd(bb, id))
        return false;

    bool hovered, held;
    const bool pressed = ImGui::ButtonBehavior(bb, id, &hovered, &held);
    const float t = animate(id, selected);
    const float h = animate(id + 1, hovered, 16.0f);

    auto* drawList = window->DrawList;
    const ImU32 top = h > 0.5f ? IM_COL32(40, 40, 40, 255) : IM_COL32(35, 35, 35, 255);
    drawList->AddRectFilledMultiColor(bb.Min, bb.Max, top, top, IM_COL32(27, 27, 27, 255), IM_COL32(27, 27, 27, 255));
    drawList->AddRect(bb.Min, bb.Max, color::outline);
    drawList->AddRect({ bb.Min.x + 1.0f, bb.Min.y + 1.0f }, { bb.Max.x - 1.0f, bb.Max.y - 1.0f }, IM_COL32(50, 50, 50, 255));
    drawList->AddText({ pos.x + 10.0f, pos.y + (24.0f - textSize.y) * 0.5f }, lerpColor(color::text, color::accent, t), label, ImGui::FindRenderedTextEnd(label));
    return pressed;
}

void drawIcon(ImDrawList* d, Icon icon, const ImVec2& c, float s, ImU32 col) noexcept
{
    const float r = s * 0.5f;
    const float th = 1.6f;
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
    }
}

bool sidebarTab(const char* label, Icon icon, bool selected) noexcept
{
    auto* window = ImGui::GetCurrentWindow();
    const auto id = window->GetID(label);
    const auto pos = window->DC.CursorPos;
    const float width = ImGui::GetContentRegionAvail().x;
    const ImRect bb{ pos, { pos.x + width, pos.y + 64.0f } };

    ImGui::ItemSize(bb);
    if (!ImGui::ItemAdd(bb, id))
        return false;

    bool hovered, held;
    const bool pressed = ImGui::ButtonBehavior(bb, id, &hovered, &held);
    const float h = animate(id + 1, hovered, 16.0f);

    // в окне-родителе: выбранная вкладка сливается с областью контента (без правой линии)
    auto* d = window->RootWindow->DrawList;
    if (selected) {
        d->AddRectFilled(bb.Min, { bb.Max.x + 1.0f, bb.Max.y }, color::windowBg);
        d->AddLine({ bb.Min.x, bb.Min.y }, { bb.Max.x + 1.0f, bb.Min.y }, color::outline);
        d->AddLine({ bb.Min.x, bb.Min.y + 1.0f }, { bb.Max.x + 1.0f, bb.Min.y + 1.0f }, color::groupLine);
        d->AddLine({ bb.Min.x, bb.Max.y - 1.0f }, { bb.Max.x + 1.0f, bb.Max.y - 1.0f }, color::groupLine);
        d->AddLine({ bb.Min.x, bb.Max.y }, { bb.Max.x + 1.0f, bb.Max.y }, color::outline);
    }
    const ImU32 col = selected ? IM_COL32(210, 210, 210, 255) : lerpColor(IM_COL32(90, 90, 90, 255), IM_COL32(150, 150, 150, 255), h);
    drawIcon(d, icon, { bb.Min.x + width * 0.5f, bb.Min.y + 32.0f }, 24.0f, col);
    if (hovered)
        ImGui::SetTooltip("%s", label);
    return pressed;
}

}

namespace hvh::gui
{

using namespace hvh::ui;

void updateInput() noexcept
{
    antiaim::updateInput();
    config.exploits.doubleTapKey.handleToggle();
    config.rage.forceShotKey.handleToggle();
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

// Ватермарка как у gamesense: двойная рамка, радужная полоска сверху,
// «newer» белым + «win» акцентом | ник | задержка | тикрейт | время
void drawWatermark(ImDrawList* d) noexcept
{
    if (!config.view.watermark)
        return;

    char nick[32] = "";
    int tickRate = 64;
    {
        GameData::Lock lock;
        const auto& local = GameData::local();
        if (local.exists) {
            std::snprintf(nick, sizeof(nick), "%s", local.name);
            tickRate = local.tickRate;
        }
    }

    const std::time_t now = std::time(nullptr);
    std::tm tm{};
    localtime_r(&now, &tm);

    char rest[128];
    if (*nick)
        std::snprintf(rest, sizeof(rest), " | %s | delay: %dms | %dtick | %02d:%02d:%02d", nick, GameData::getNetOutgoingLatency(), tickRate, tm.tm_hour, tm.tm_min, tm.tm_sec);
    else
        std::snprintf(rest, sizeof(rest), " | %02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);

    ImFont* font = fonts.menu ? fonts.menu : ImGui::GetFont();
    const float fontSize = font->FontSize;
    const auto sizeOf = [&](const char* text) { return font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, text); };
    const auto wNewer = sizeOf("newer");
    const auto wWin = sizeOf("win");
    const auto wRest = sizeOf(rest);

    constexpr float padX = 6.0f, padY = 4.0f, border = 4.0f, margin = 10.0f;
    const float textW = wNewer.x + wWin.x + wRest.x;
    const float textH = wNewer.y;
    const ImVec2 size{ textW + padX * 2.0f + border * 2.0f, textH + padY * 2.0f + border * 2.0f + 2.0f };
    const auto& display = ImGui::GetIO().DisplaySize;
    const ImVec2 pos{ display.x - size.x - margin, margin };
    const ImVec2 max{ pos.x + size.x, pos.y + size.y };

    // рамка: чёрная, серая, полоса, серая — как у окна меню
    d->AddRectFilled(pos, max, IM_COL32(0, 0, 0, 255));
    d->AddRectFilled({ pos.x + 1.0f, pos.y + 1.0f }, { max.x - 1.0f, max.y - 1.0f }, IM_COL32(60, 60, 60, 255));
    d->AddRectFilled({ pos.x + 2.0f, pos.y + 2.0f }, { max.x - 2.0f, max.y - 2.0f }, IM_COL32(40, 40, 40, 255));
    d->AddRectFilled({ pos.x + border - 1.0f, pos.y + border - 1.0f }, { max.x - border + 1.0f, max.y - border + 1.0f }, IM_COL32(60, 60, 60, 255));
    const ImVec2 inner{ pos.x + border, pos.y + border };
    const ImVec2 innerMax{ max.x - border, max.y - border };
    d->AddRectFilled(inner, innerMax, color::windowBg);

    // радужная полоска (вторая строка — тёмная копия)
    const float mid = (inner.x + innerMax.x) * 0.5f;
    const ImU32 cyan = IM_COL32(55, 177, 218, 255), magenta = IM_COL32(204, 70, 205, 255), yellow = IM_COL32(204, 227, 54, 255);
    const ImU32 cyanD = IM_COL32(28, 89, 109, 255), magentaD = IM_COL32(102, 35, 103, 255), yellowD = IM_COL32(102, 114, 27, 255);
    d->AddRectFilledMultiColor(inner, { mid, inner.y + 1.0f }, cyan, magenta, magenta, cyan);
    d->AddRectFilledMultiColor({ mid, inner.y }, { innerMax.x, inner.y + 1.0f }, magenta, yellow, yellow, magenta);
    d->AddRectFilledMultiColor({ inner.x, inner.y + 1.0f }, { mid, inner.y + 2.0f }, cyanD, magentaD, magentaD, cyanD);
    d->AddRectFilledMultiColor({ mid, inner.y + 1.0f }, { innerMax.x, inner.y + 2.0f }, magentaD, yellowD, yellowD, magentaD);

    // текст с тенью
    float x = inner.x + padX;
    const float y = inner.y + 2.0f + padY;
    const auto put = [&](const char* text, ImU32 col, float width) {
        d->AddText(font, fontSize, { x + 1.0f, y + 1.0f }, IM_COL32(0, 0, 0, 200), text);
        d->AddText(font, fontSize, { x, y }, col, text);
        x += width;
    };
    put("newer", IM_COL32(255, 255, 255, 255), wNewer.x);
    put("win", color::accent, wWin.x);
    put(rest, color::text, wRest.x);
}

static int selectedWeapon = 0;

void drawRageTab() noexcept
{
    auto& rage = config.rage;

    for (int i = 0; i < static_cast<int>(weaponGroupNames.size()); ++i) {
        if (i)
            ImGui::SameLine(0.0f, 6.0f);
        if (pill(weaponGroupNames[i], selectedWeapon == i))
            selectedWeapon = i;
    }
    ImGui::Dummy({ 0.0f, 4.0f });

    const float width = (ImGui::GetContentRegionAvail().x - 12.0f) * 0.5f;
    const float height = ImGui::GetContentRegionAvail().y;

    if (beginGroup("Основное", { width, height })) {
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
        if (rage.forceShot)
            keybind("Клавиша форсшута", rage.forceShotKey);
        toggle("Резольвер", &rage.resolver);
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
                ImGui::SetTooltip("Выкл — точки, общие для всех сторон десинка, просто в приоритете\n"
                                  "(остальные оцениваются по доверию к стороне, голова — строже).\n"
                                  "После промаха / Всегда — стрелять ТОЛЬКО в такие точки.");
            ImGui::Checkbox("Строгий отбор выстрелов", &rage.strictShots);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("По десинкерам — только точки, общие для всех сторон; без прогноза по прыгающим\n"
                                  "и джиттерящим; голова в прыжке/на джиттере не бьётся; сквозь стену — с запасом урона.\n"
                                  "Логи 04–09.11: такие выстрелы попадали в 65%%, отброшенные — в 36%%.");
        }
        sliderFloat("FOV", &rage.fov, 1.0f, 180.0f, "%.0f°");
        keybind("Мин. урон (override)", rage.minDamageOverrideKey);
        keybind("Только в тело", rage.forceBodyKey);
        toggle("Дормант-аимбот", &rage.dormantAim);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Враг за пределами видимости (дормант), но слышен — шаги, выстрел —\n"
                              "или виден союзникам на радаре: стрельба в живот через стену,\n"
                              "если пробивает на мин. урон. Только когда видимых целей нет.");
        if (rage.dormantAim)
            sliderInt("Мин. точность", &rage.dormantAccuracy, 25, 95, "%d%%");

        ImGui::Dummy({ 0.0f, 6.0f });
        ImGui::PushFont(fonts.bold);
        ImGui::TextUnformatted("Нож и зевс");
        ImGui::PopFont();
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
        if (rage.zeusBot)
            sliderInt("Хитшанс зевса", &rage.zeusHitchance, 30, 100, "%d%%");

        ImGui::Dummy({ 0.0f, 6.0f });
        ImGui::PushFont(fonts.bold);
        ImGui::TextUnformatted("Эксплойты");
        ImGui::PopFont();
        auto& ex = config.exploits;
        toggle("Double tap", &ex.doubleTap);
        keybind("Клавиша DT", ex.doubleTapKey);
        static constexpr const char* dtModes[]{ "Быстрая стрельба", "Дефенсив" };
        combo("Режим DT", &ex.doubleTapMode, dtModes, IM_ARRAYSIZE(dtModes));
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Быстрая стрельба — сдвиг на выстреле, вторая пуля сразу. На это время\nдефенсив молчит: заряд один, и он принадлежит выстрелу.\nДефенсив — выстрел обычный, весь заряд уходит на рывки дефенсива.\nСкаут и AWP всегда в дефенсиве (второй пули нет, DT-фикс на серверах).");
        sliderInt("Тиков сдвига", &ex.shiftTicks, 6, 14);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Заряд под вторую пулю DT (быстрая стрельба). У дефенсива свой ползунок ниже.");
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
        if (!tickbase::isInstalled()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(color::bad));
            ImGui::TextWrapped("CL_Move не найден — DT и десинк недоступны в этом билде игры");
            ImGui::PopStyleColor();
        }
    }
    endGroup();

    ImGui::SameLine(0.0f, 12.0f);

    auto& weapon = rage.weapons[selectedWeapon];
    if (beginGroup(weaponGroupNames[selectedWeapon], { width, height })) {
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
        // выше 100 — «HP+N»: подпись без %d, число уже вписано
        char minDamageFormat[16];
        std::snprintf(minDamageFormat, sizeof(minDamageFormat), "HP+%d", weapon.minDamage - 100);
        sliderInt("Мин. урон", &weapon.minDamage, 1, 120, weapon.minDamage > 100 ? minDamageFormat : "%d");
        sliderInt("Мин. урон (override)", &weapon.minDamageOverride, 1, 120);
        static constexpr const char* autoStops[]{ "Выкл", "Полный", "Точность" };
        combo("Автостоп", &weapon.autoStop, autoStops, IM_ARRAYSIZE(autoStops));
        if (weapon.autoStop != static_cast<int>(AutoStop::Off))
            toggle("Тормозить между выстрелами", &weapon.autoStopEarly);
        toggle("Автоскоуп", &weapon.autoScope);
        toggle("Предпочитать тело", &weapon.preferBody);
        toggle("В тело, если летально", &weapon.bodyIfLethal);
    }
    endGroup();
}

static void sectionTitle(const char* title) noexcept
{
    ImGui::Dummy({ 0.0f, 6.0f });
    ImGui::PushFont(fonts.bold);
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
    return 32.0f + static_cast<float>(n) * rowHeight;
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

    const float left = std::max(height - rowsHeight(7) - rowsHeight(5) - 20.0f, rowsHeight(6));
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

    const float left = std::max(height - rowsHeight(count + 3) - 10.0f, rowsHeight(8));
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

    const float left = std::max(height - rowsHeight(4) - 10.0f, rowsHeight(6));
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

    const float rest = std::max(height - rowsHeight(5) - 10.0f, rowsHeight(5));
    if (beginGroup("Проверка у себя", { width, rest })) {
        auto& view = config.view;
        toggle("Показывать AA в 3-м лице", &view.showFakeAngles);
        toggle("Модель сервера", &view.serverModel);
        tooltip("В 3-м лице поверх своей модели — призрак, как тебя видит сервер: ноги и тело\n"
                "уведены десинком, голова — по отправленному взгляду, наклон — roll.");
        if (view.serverModel)
            colorEdit("Цвет призрака", view.serverModelColor);
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

    const float left = std::max(height - rowsHeight(9) - 10.0f, rowsHeight(5));
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
            static constexpr const char* modes[]{ "Смена стороны", "Подстройка", "Стойка пресетов" };
            combo("Что делать", &aa.antiBruteMode, modes, IM_ARRAYSIZE(modes));
            tooltip("Смена стороны — десинк на другую сторону.\n"
                    "Подстройка — углы билдера сдвигаются на случайную долю (до «Силы»).\n"
                    "Стойка пресетов — загружается следующий пресет из стойки справа.");
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

void drawAntiAimTab() noexcept
{
    static int page = 0;
    static constexpr const char* pages[]{ "Углы", "Билдер", "Десинк", "Дефенсив", "Анти-брут" };
    for (int i = 0; i < IM_ARRAYSIZE(pages); ++i) {
        if (i)
            ImGui::SameLine(0.0f, 6.0f);
        if (pill(pages[i], page == i))
            page = i;
    }
    // справа от пилюль — короткая сводка: что сейчас реально уходит на сервер
    if (localPlayer && localPlayer.get().isAlive() && config.antiAim.enabled) {
        char text[96];
        const int side = antiaim::desyncSide();
        std::snprintf(text, sizeof(text), "%s · десинк %s%s", antiaim::stateName(),
            side > 0 ? "вправо" : side < 0 ? "влево" : "нет",
            antiaim::defensiveActive() ? " · дефенсив" : "");
        const auto textSize = ImGui::CalcTextSize(text);
        ImGui::SameLine(0.0f, 0.0f);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(ImGui::GetContentRegionAvail().x - textSize.x - 2.0f, 8.0f));
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (24.0f - textSize.y) * 0.5f);   // 24 — высота пилюли
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(color::textDim));
        ImGui::TextUnformatted(text);
        ImGui::PopStyleColor();
    }
    ImGui::Dummy({ 0.0f, 4.0f });

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

void drawWorldGroup() noexcept
{
    auto& view = config.view;
    toggle("Без зума в прицеле", &view.removeScopeZoom);
    if (view.removeScopeZoom) {
        toggle("Второй зум приближает", &view.secondZoom);
        if (view.secondZoom)
            sliderFloat("Увеличение второго зума", &view.secondZoomFactor, 1.2f, 6.0f, "x%.1f");
    }
    toggle("Траектория гранаты", &view.grenadePrediction);
    sliderFloat("ESP дормантов", &view.dormantTime, 1.5f, 20.0f, "%.1f сек");
    toggle("Логи выстрелов на экране", &view.screenLogs);
    toggle("Ватермарка", &view.watermark);
    toggle("Ровный ESP", &view.syncRender);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Однопоточный рендер (mat_queue_mode 0): ESP рисуется по тому же кадру,\nчто на экране, и не дёргается. Может немного снизить FPS.");

    ImGui::Dummy({ 0.0f, 6.0f });
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(color::textDim));
    ImGui::TextWrapped("Дальность ESP ограничена тем, что присылает сервер. Игроки вне видимости показываются по последней позиции и по звукам шагов/выстрелов.");
    ImGui::PopStyleColor();
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

void drawVisualsTab() noexcept
{
    auto& view = config.view;
    const float width = (ImGui::GetContentRegionAvail().x - 12.0f) * 0.5f;
    const float height = ImGui::GetContentRegionAvail().y;

    if (beginGroup("Приколюхи", { width, height })) {
        toggle("Цифры урона", &view.damageNumbers);
        toggle("Кольца шагов врагов", &view.footsteps);
        toggle("Вспышка при убийстве", &view.killFlash);
        toggle("Таймеры дыма и огня", &view.grenadeTimers);
        toggle("Круг разброса", &view.spreadCircle);
        ImGui::Dummy({ 0.0f, 6.0f });
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(color::textDim));
        ImGui::TextWrapped("Флаги ESP, точка прицела, скелет при выстреле и Tab — во вкладке «Визуалы».");
        ImGui::PopStyleColor();
    }
    endGroup();

    ImGui::SameLine(0.0f, 12.0f);

    if (beginGroup("Экран и чат", { width, height })) {
        toggle("Aspect ratio", &view.aspectRatio);
        if (view.aspectRatio) {
            sliderFloat("Соотношение", &view.aspectRatioValue, 0.8f, 2.4f, "%.2f");
            static constexpr const char* presets[]{ "5:4", "4:3", "3:2", "16:10", "16:9" };
            static constexpr float values[]{ 1.25f, 1.3333f, 1.5f, 1.6f, 1.7778f };
            for (int i = 0; i < IM_ARRAYSIZE(presets); ++i) {
                if (i)
                    ImGui::SameLine(0.0f, 4.0f);
                if (ImGui::SmallButton(presets[i]))
                    view.aspectRatioValue = values[i];
            }
        }
        toggle("Вьюмодель", &view.viewmodel);
        if (view.viewmodel) {
            sliderInt("FOV рук", &view.viewmodelFov, 40, 120);
            sliderFloat("Смещение X", &view.viewmodelX, -10.0f, 10.0f, "%.1f");
            sliderFloat("Смещение Y", &view.viewmodelY, -10.0f, 10.0f, "%.1f");
            sliderFloat("Смещение Z", &view.viewmodelZ, -10.0f, 10.0f, "%.1f");
        }
        toggle("Свой прицел снайпера", &view.customScope);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Градиентные линии вместо чёрного прицела (работает с «Без зума в прицеле»).");
        if (view.customScope) {
            sliderInt("Длина линий", &view.scopeSize, 10, 300);
            sliderInt("Отступ от центра", &view.scopeGap, 0, 100);
            toggle("T-прицел (без верхней)", &view.scopeT);
        }
        toggle("Мин. урон у прицела", &view.damageIndicator);
        toggle("Метки попаданий", &view.shotMarkers);
        if (view.shotMarkers) {
            toggle("Метки промахов", &view.missMarkers);
            sliderFloat("Время меток", &view.shotMarkerTime, 0.5f, 6.0f, "%.1f сек");
        }
        toggle("Предупреждение о замедлении", &view.slowdownWarning);
        toggle("Список биндов", &view.keybindList);
        toggle("Индикатор LC", &view.lcIndicator);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Зелёный — сейчас идёт дефенсив (лагкомп сломан), полоска — сколько тиков осталось.");
        ImGui::Dummy({ 0.0f, 6.0f });
        toggle("Трештолк", &view.trashTalk);
        if (view.trashTalk) {
            static constexpr const char* when[]{ "При килле", "При смерти" };
            multiCombo("Когда писать", &view.trashTalkWhen, when, IM_ARRAYSIZE(when));
        }
    }
    endGroup();
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

void drawWorldLookTab(bool* skyboxEnabled, int* skybox, const char* const* skyboxNames, int skyboxCount) noexcept
{
    auto& w = config.world;
    auto& view = config.view;
    const float width = (ImGui::GetContentRegionAvail().x - 12.0f) * 0.5f;
    const float height = ImGui::GetContentRegionAvail().y;

    if (beginGroup("Вид мира", { width, height })) {
        toggle("Включить", &w.enabled);
        const char* presetNames[IM_ARRAYSIZE(worldPresets) + 1]{ "Свой" };
        for (int i = 0; i < IM_ARRAYSIZE(worldPresets); ++i)
            presetNames[i + 1] = worldPresets[i].name;
        if (combo("Пресет", &w.preset, presetNames, IM_ARRAYSIZE(presetNames)) && w.preset > 0)
            applyWorldPreset(worldPresets[w.preset - 1], skyboxEnabled, skybox, skyboxNames, skyboxCount);
        if (colorEdit3("Стены", w.wallColor)) w.preset = 0;
        if (colorEdit3("Пропы", w.propColor)) w.preset = 0;
        sliderFloat("Прозрачность пропов", &w.propAlpha, 0.1f, 1.0f, "%.2f");
        toggle("Цвет неба", &w.skyColorOn);
        if (w.skyColorOn && colorEdit3("Небо", w.skyColor)) w.preset = 0;
        toggle("Скайбокс", skyboxEnabled);
        if (*skyboxEnabled)
            combo("Небо (скайбокс)", skybox, skyboxNames, skyboxCount);
        ImGui::Dummy({ 0.0f, 6.0f });
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(color::textDim));
        ImGui::TextWrapped("Пресет ставит всё сразу: цвета, туман, подсветку и небо. Дальше можно подкрутить руками. Новые текстуры карты докрашиваются раз в секунду.");
        ImGui::PopStyleColor();
    }
    endGroup();

    ImGui::SameLine(0.0f, 12.0f);

    if (beginGroup("Туман, свет, искры", { width, height })) {
        toggle("Свой туман", &w.fog);
        if (w.fog) {
            colorEdit3("Цвет тумана", w.fogColor);
            sliderFloat("Начало", &w.fogStart, 0.0f, 3000.0f, "%.0f ед.");
            sliderFloat("Конец", &w.fogEnd, 200.0f, 10000.0f, "%.0f ед.");
            sliderFloat("Плотность", &w.fogDensity, 0.0f, 1.0f, "%.2f");
        }
        toggle("Подсветка", &w.ambient);
        if (w.ambient)
            colorEdit3("Цвет подсветки", w.ambientColor);
        ImGui::Dummy({ 0.0f, 6.0f });
        toggle("Искры от пуль", &view.impactSparks);
        if (view.impactSparks) {
            colorEdit("Цвет искр", view.sparkColor);
            sliderInt("Количество", &view.sparkCount, 4, 60);
            sliderFloat("Разлёт", &view.sparkSize, 0.3f, 3.0f, "x%.1f");
        }
    }
    endGroup();
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

#include <algorithm>
#include <cmath>
#include <cstdio>
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
#include "Tickbase.h"

#include <InputUtil.h>
#include <CSGO/LocalPlayer.h>

namespace hvh::ui
{

namespace
{

constexpr float rowHeight = 26.0f;
constexpr float rounding = 6.0f;

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
    style.WindowRounding = 10.0f;
    style.ChildRounding = 8.0f;
    style.FrameRounding = rounding;
    style.PopupRounding = rounding;
    style.GrabRounding = rounding;
    style.ScrollbarRounding = rounding;
    style.TabRounding = rounding;
    style.WindowBorderSize = 0.0f;
    style.ChildBorderSize = 0.0f;
    style.PopupBorderSize = 1.0f;
    style.FrameBorderSize = 0.0f;
    style.WindowPadding = { 12.0f, 12.0f };
    style.FramePadding = { 10.0f, 5.0f };
    style.ItemSpacing = { 8.0f, 6.0f };
    style.ItemInnerSpacing = { 6.0f, 4.0f };
    style.ScrollbarSize = 6.0f;
    style.GrabMinSize = 8.0f;

    auto* c = style.Colors;
    c[ImGuiCol_Text] = toVec4(color::text);
    c[ImGuiCol_TextDisabled] = toVec4(color::textDim);
    c[ImGuiCol_WindowBg] = toVec4(color::windowBg);
    c[ImGuiCol_ChildBg] = toVec4(color::groupBg);
    c[ImGuiCol_PopupBg] = toVec4(IM_COL32(16, 19, 26, 252));
    c[ImGuiCol_Border] = toVec4(color::groupLine);
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

bool beginGroup(const char* title, const ImVec2& size) noexcept
{
    ImGui::PushStyleColor(ImGuiCol_ChildBg, toVec4(color::groupBg));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, { 14.0f, 10.0f });
    const bool open = ImGui::BeginChild(title, size, false, ImGuiWindowFlags_AlwaysUseWindowPadding);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();

    auto* drawList = ImGui::GetWindowDrawList();
    const auto pos = ImGui::GetWindowPos();
    const float width = ImGui::GetWindowWidth();

    // заголовок группы с акцентной точкой и линией
    ImGui::PushFont(fonts.bold);
    drawList->AddCircleFilled({ pos.x + 16.0f, pos.y + 19.0f }, 3.0f, color::accent);
    drawList->AddText({ pos.x + 26.0f, pos.y + 10.0f }, color::text, title);
    ImGui::PopFont();
    drawList->AddLine({ pos.x, pos.y + 36.0f }, { pos.x + width, pos.y + 36.0f }, color::groupLine);

    ImGui::SetCursorPosY(46.0f);
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
    window->DrawList->AddText({ pos.x, pos.y + (rowHeight - ImGui::GetFontSize()) * 0.5f }, color::text, label, labelEnd);

    // квадратик цвета справа, по клику — палитра ImGui
    ImGui::SetCursorScreenPos({ pos.x + width - 32.0f, pos.y + 4.0f });
    ImGui::PushID(label);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f);
    const bool changed = ImGui::ColorEdit4("##color", rgba, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel | ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_AlphaPreviewHalf);
    ImGui::PopStyleVar();
    ImGui::PopID();
    ImGui::SetCursorScreenPos(pos);
    ImGui::ItemSize(ImRect{ pos, { pos.x + width, pos.y + rowHeight } });
    return changed;
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

    const float t = animate(id, *value);
    const float h = animate(id + 1, hovered, 16.0f);
    auto* drawList = window->DrawList;

    const auto labelEnd = ImGui::FindRenderedTextEnd(label);
    drawList->AddText({ pos.x, pos.y + (rowHeight - ImGui::GetFontSize()) * 0.5f }, lerpColor(color::textDim, color::text, std::max(t, h * 0.6f)), label, labelEnd);

    // переключатель-«пилюля» справа
    const ImVec2 switchMin{ bb.Max.x - 32.0f, pos.y + 6.0f };
    const ImVec2 switchMax{ bb.Max.x, pos.y + 20.0f };
    drawList->AddRectFilled(switchMin, switchMax, lerpColor(lerpColor(color::frame, color::frameHover, h), withAlpha(color::accent, 0.9f), t), 7.0f);
    const float knobX = switchMin.x + 7.0f + (switchMax.x - switchMin.x - 14.0f) * t;
    if (t > 0.01f)
        drawList->AddCircleFilled({ knobX, pos.y + 13.0f }, 9.0f, withAlpha(color::accent, 0.18f * t));
    drawList->AddCircleFilled({ knobX, pos.y + 13.0f }, 5.0f, lerpColor(color::textDim, IM_COL32(255, 255, 255, 255), t));

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
    const float height = 38.0f;
    const ImRect bb{ pos, { pos.x + width, pos.y + height } };
    const ImRect track{ { pos.x, pos.y + 26.0f }, { pos.x + width, pos.y + 30.0f } };

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

    const float h = animate(id, hovered || held, 16.0f);
    auto* drawList = window->DrawList;
    const auto labelEnd = ImGui::FindRenderedTextEnd(label);
    drawList->AddText({ pos.x, pos.y + 2.0f }, lerpColor(color::textDim, color::text, h * 0.7f + 0.3f), label, labelEnd);

    char buffer[32];
    if constexpr (std::is_integral_v<T>)
        std::snprintf(buffer, sizeof(buffer), format, *value);
    else
        std::snprintf(buffer, sizeof(buffer), format, static_cast<double>(*value));
    const auto valueSize = ImGui::CalcTextSize(buffer);
    drawList->AddText({ bb.Max.x - valueSize.x, pos.y + 2.0f }, color::accent, buffer);

    const float ratio = std::clamp(static_cast<float>(*value - min) / static_cast<float>(max - min), 0.0f, 1.0f);
    const float fillX = track.Min.x + track.GetWidth() * ratio;
    drawList->AddRectFilled(track.Min, track.Max, color::frame, 2.0f);
    drawList->AddRectFilledMultiColor(track.Min, { fillX, track.Max.y }, color::accent, color::accent2, color::accent2, color::accent);
    drawList->AddCircleFilled({ fillX, track.Min.y + 2.0f }, 4.0f + 2.0f * h, IM_COL32(255, 255, 255, 255));

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
    ImGui::PushStyleColor(ImGuiCol_Text, toVec4(color::textDim));
    ImGui::TextUnformatted(label, ImGui::FindRenderedTextEnd(label));
    ImGui::PopStyleColor();
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
    const char* keyName = listening ? "..." : (key.isSet() ? key.toString() : "—");
    const auto keySize = ImGui::CalcTextSize(keyName);
    const ImRect button{ { bb.Max.x - keySize.x - 18.0f, pos.y + 3.0f }, { bb.Max.x, pos.y + rowHeight - 3.0f } };

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
    drawList->AddText({ pos.x, pos.y + (rowHeight - ImGui::GetFontSize()) * 0.5f }, color::textDim, label, ImGui::FindRenderedTextEnd(label));
    drawList->AddRectFilled(button.Min, button.Max, listening ? withAlpha(color::accent, 0.35f) : (hovered ? color::frameHover : color::frame), 5.0f);
    drawList->AddText({ button.Min.x + 9.0f, pos.y + (rowHeight - ImGui::GetFontSize()) * 0.5f }, listening ? color::text : color::textDim, keyName);
}

bool pill(const char* label, bool selected) noexcept
{
    auto* window = ImGui::GetCurrentWindow();
    const auto id = window->GetID(label);
    const auto textSize = ImGui::CalcTextSize(label, ImGui::FindRenderedTextEnd(label));
    const auto pos = window->DC.CursorPos;
    const ImRect bb{ pos, { pos.x + textSize.x + 24.0f, pos.y + 28.0f } };

    ImGui::ItemSize(bb);
    if (!ImGui::ItemAdd(bb, id))
        return false;

    bool hovered, held;
    const bool pressed = ImGui::ButtonBehavior(bb, id, &hovered, &held);
    const float t = animate(id, selected);
    const float h = animate(id + 1, hovered, 16.0f);

    auto* drawList = window->DrawList;
    drawList->AddRectFilled(bb.Min, bb.Max, lerpColor(withAlpha(color::frame, 0.6f + 0.4f * h), withAlpha(color::accent, 0.2f), t), 14.0f);
    if (t > 0.01f)
        drawList->AddRect(bb.Min, bb.Max, withAlpha(color::accent, t), 14.0f);
    drawList->AddText({ pos.x + 12.0f, pos.y + (28.0f - textSize.y) * 0.5f }, lerpColor(color::textDim, color::text, std::max(t, h * 0.5f)), label, ImGui::FindRenderedTextEnd(label));
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
    const ImRect bb{ pos, { pos.x + width, pos.y + 34.0f } };

    ImGui::ItemSize(bb);
    if (!ImGui::ItemAdd(bb, id))
        return false;

    bool hovered, held;
    const bool pressed = ImGui::ButtonBehavior(bb, id, &hovered, &held);
    const float t = animate(id, selected, 14.0f);
    const float h = animate(id + 1, hovered, 16.0f);

    auto* d = window->DrawList;
    if (t > 0.01f || h > 0.01f)
        d->AddRectFilled(bb.Min, bb.Max, lerpColor(withAlpha(color::frame, 0.5f * h), withAlpha(color::accent, 0.13f), t), 7.0f);
    if (t > 0.01f)
        d->AddRectFilled({ bb.Min.x, bb.Min.y + 8.0f }, { bb.Min.x + 3.0f, bb.Max.y - 8.0f }, withAlpha(color::accent, t), 2.0f);

    const auto col = lerpColor(lerpColor(color::textDim, color::text, h * 0.6f), color::accent, t);
    drawIcon(d, icon, { bb.Min.x + 22.0f, bb.Min.y + 17.0f }, 15.0f, col);
    d->AddText({ bb.Min.x + 40.0f, bb.Min.y + (34.0f - ImGui::GetFontSize()) * 0.5f }, lerpColor(lerpColor(color::textDim, color::text, h * 0.6f), color::text, t), label, ImGui::FindRenderedTextEnd(label));
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
    if (config.view.removeScopeZoom && localPlayer.get().isScoped()) {
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
    }

    // список под прицелом
    float y = center.y + 24.0f;
    const auto line = [&](const char* text, ImU32 col) {
        const auto size = ImGui::CalcTextSize(text);
        drawList->AddText({ center.x - size.x * 0.5f + 1.0f, y + 1.0f }, IM_COL32(0, 0, 0, 160), text);
        drawList->AddText({ center.x - size.x * 0.5f, y }, col, text);
        y += size.y + 1.0f;
    };

    if (movement::isFakeDucking())
        line("FD", color::accent2);
    else if (config.exploits.doubleTap && (!config.exploits.doubleTapKey.isSet() || config.exploits.doubleTapKey.isToggled()))
        line("DT", tickbase::isCharged() ? color::good : color::bad);
    if (antiaim::isFreestanding())
        line("FS", color::accent);
    if (config.exploits.defensive && !movement::isFakeDucking())
        line("DEF", tickbase::isCharged() ? color::good : color::bad);
    if (config.movement.autoPeekKey.isSet() && config.movement.autoPeekKey.isDown())
        line("PEEK", color::accent);
    if (config.antiAim.enabled && config.antiAim.desync)
        line(antiaim::desyncSide() > 0 ? "DESYNC <" : "DESYNC >", color::accent);
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
                              "Без него рейдж в такой ситуации просто не стреляет («фотка»).");
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
            toggle("Точки, общие для обеих сторон", &rage.resolverSafeWhenUnsure);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Сторона десинка не доказана (нет находки по слоям, режим ещё не попадал):\n"
                                  "бить только точки, которые попадают в ОБЕ стороны — обычно это тело.\n"
                                  "Попадание почти гарантировано, а «край головы» при неверной стороне — промах.\n"
                                  "Если таких точек под мин. урон нет, стрельба по угаданной стороне остаётся.");
        }
        sliderFloat("FOV", &rage.fov, 1.0f, 180.0f, "%.0f°");
        keybind("Мин. урон (override)", rage.minDamageOverrideKey);
        keybind("Только в тело", rage.forceBodyKey);

        ImGui::Dummy({ 0.0f, 6.0f });
        ImGui::PushFont(fonts.bold);
        ImGui::TextUnformatted("Эксплойты");
        ImGui::PopFont();
        auto& ex = config.exploits;
        toggle("Double tap", &ex.doubleTap);
        keybind("Клавиша DT", ex.doubleTapKey);
        sliderInt("Тиков сдвига", &ex.shiftTicks, 6, 14);
        keybind("Телепорт", ex.teleportKey);
        toggle("Дефенсив", &ex.defensive);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Враг только что увидел тебя — рывок на заряженные тики:\nего лагкомп и бектрек по тебе ломаются. Нужен заряд (DT/телепорт).");
        keybind("Автопик", config.movement.autoPeekKey);
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

void drawAntiAimTab() noexcept
{
    auto& aa = config.antiAim;
    const float width = (ImGui::GetContentRegionAvail().x - 12.0f) * 0.5f;
    const float height = ImGui::GetContentRegionAvail().y;

    if (beginGroup("Углы", { width, height })) {
        toggle("Включить", &aa.enabled);
        static constexpr const char* pitches[]{ "Выкл", "Вниз", "Вверх", "Ноль" };
        combo("Pitch", &aa.pitch, pitches, IM_ARRAYSIZE(pitches));
        static constexpr const char* bases[]{ "Выкл", "Назад", "На врагов" };
        combo("Yaw", &aa.yawBase, bases, IM_ARRAYSIZE(bases));
        sliderInt("Смещение yaw", &aa.yawOffset, -180, 180, "%d°");
        toggle("Фристенд", &aa.freestanding);
        if (aa.freestanding)
            keybind("Клавиша фристенда", aa.freestandKey);
        static constexpr const char* modifiers[]{ "Выкл", "Джиттер", "Центр-джиттер", "Спин", "Рандом", "5-way" };
        combo("Модификатор", &aa.yawModifier, modifiers, IM_ARRAYSIZE(modifiers));
        if (aa.yawModifier == static_cast<int>(YawModifier::Spin))
            sliderInt("Скорость спина", &aa.spinSpeed, 1, 60);
        else if (aa.yawModifier != static_cast<int>(YawModifier::Off))
            sliderInt("Диапазон", &aa.modifierRange, 0, 180, "%d°");

        ImGui::Dummy({ 0.0f, 6.0f });
        ImGui::PushFont(fonts.bold);
        ImGui::TextUnformatted("Флики (дефенсив AA)");
        ImGui::PopFont();
        toggle("Флики", &aa.flicks);
        if (aa.flicks) {
            static constexpr const char* triggers[]{ "При дефенсиве", "Всегда" };
            combo("Когда", &aa.flickTrigger, triggers, IM_ARRAYSIZE(triggers));
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("«При дефенсиве» — на тиках рывка (дефенсив, телепорт, сдвиг DT) и пару тиков после:\n"
                    "в это время его лагкомп по тебе не работает, а модель на сервере смотрит вбок.\n"
                    "Нужен включённый «Дефенсив» в эксплойтах или DT. «Всегда» — ещё и раз в N пакетов.");
            static constexpr const char* sides[]{ "Влево/вправо", "Рандом", "Свой yaw" };
            combo("Сторона", &aa.flickSide, sides, IM_ARRAYSIZE(sides));
            if (aa.flickSide == static_cast<int>(FlickSide::Custom)) {
                sliderInt("Yaw флика", &aa.flickYaw, -180, 180, "%+d°");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Смещение от обычного yaw AA на тике флика. Минус — влево, плюс — вправо, 180 — назад.");
            } else {
                sliderInt("Угол флика", &aa.flickAngle, 30, 180, "%d°");
            }
            toggle("Свой pitch на флике", &aa.flickPitchOverride);
            if (aa.flickPitchOverride) {
                sliderInt("Pitch флика", &aa.flickPitch, -89, 89, "%d°");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("-89 — вверх, 0 — прямо, 89 — вниз.");
            }
            if (aa.flickTrigger == static_cast<int>(FlickTrigger::Always))
                sliderInt("Раз в N пакетов", &aa.flickEvery, 3, 32);
        }

        ImGui::Dummy({ 0.0f, 6.0f });
        ImGui::PushFont(fonts.bold);
        ImGui::TextUnformatted("Дефенсив AA");
        ImGui::PopFont();
        toggle("Включить дефенсив", &aa.defensive);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Пока враг целится в тебя (его взгляд в пределах «FOV» от направления на нас):\n"
                              "pitch вниз, тело уведено вбок от его прицела, сторона дёргается на каждом пакете.\n"
                              "Его резольвер и бектрек по тебе сбиваются.");
        if (aa.defensive) {
            sliderInt("FOV обнаружения", &aa.defensiveFov, 1, 90, "%d°");
            sliderInt("Увод yaw", &aa.defensiveYaw, 0, 180, "%d°");
            sliderInt("Pitch", &aa.defensivePitch, -89, 89, "%d°");
            toggle("Дёргать сторону", &aa.defensiveJitter);
        }

        ImGui::Dummy({ 0.0f, 6.0f });
        ImGui::PushFont(fonts.bold);
        ImGui::TextUnformatted("Ручной AA");
        ImGui::PopFont();
        keybind("Влево", aa.manualLeft);
        keybind("Вправо", aa.manualRight);
        keybind("Назад", aa.manualBack);
    }
    endGroup();

    ImGui::SameLine(0.0f, 12.0f);

    if (beginGroup("Десинк и фейклаг", { width, height })) {
        toggle("Десинк", &aa.desync);
        sliderInt("Влево", &aa.desyncLeft, 0, 60, "%d°");
        sliderInt("Вправо", &aa.desyncRight, 0, 60, "%d°");
        toggle("Джиттер десинка", &aa.desyncJitter);
        toggle("Анти-брутфорс", &aa.antiBrute);
        keybind("Инвертер", aa.inverter);
        sliderInt("Фейклаг", &aa.fakeLag, 1, 14, "%d тик.");
        toggle("Слайд-волк", &aa.slideWalk);

        ImGui::Dummy({ 0.0f, 6.0f });
        ImGui::PushFont(fonts.bold);
        ImGui::TextUnformatted("Вид");
        ImGui::PopFont();
        toggle("Показывать AA в 3-м лице", &config.view.showFakeAngles);

        ImGui::Dummy({ 0.0f, 6.0f });
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(color::textDim));
        ImGui::TextWrapped("При включённом DT фейклаг автоматически ограничен 1 тиком.");
        ImGui::PopStyleColor();
    }
    endGroup();
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
    toggle("Ровный ESP", &view.syncRender);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Однопоточный рендер (mat_queue_mode 0): ESP рисуется по тому же кадру,\nчто на экране, и не дёргается. Может немного снизить FPS.");

    ImGui::Dummy({ 0.0f, 6.0f });
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(color::textDim));
    ImGui::TextWrapped("Дальность ESP ограничена тем, что присылает сервер. Игроки вне видимости показываются по последней позиции и по звукам шагов/выстрелов.");
    ImGui::PopStyleColor();
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
        toggle("Возврат, когда отпустил WASD", &m.autoPeekOnRelease);
        ImGui::Dummy({ 0.0f, 6.0f });
        keybind("Фейкдак", m.fakeDuckKey);
        if (!tickbase::isInstalled()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(color::bad));
            ImGui::TextWrapped("Фейкдак недоступен: CL_Move не найден");
            ImGui::PopStyleColor();
        }

        ImGui::Dummy({ 0.0f, 6.0f });
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(color::textDim));
        ImGui::TextWrapped("Автопик: держи клавишу — точка запоминается, после выстрела персонаж сам бежит назад. Фейкдак на время удержания выключает DT.");
        ImGui::PopStyleColor();
    }
    endGroup();

    ImGui::SameLine(0.0f, 12.0f);

    if (beginGroup("Прыжки и прочее", { width, height })) {
        toggle("Бхоп", &m.bunnyHop);
        static constexpr const char* strafeModes[]{ "Выкл", "По камере", "WASD" };
        combo("Автострейф", &m.autoStrafe, strafeModes, IM_ARRAYSIZE(strafeModes));
        toggle("Быстрая лестница", &m.fastLadder);
        ImGui::Dummy({ 0.0f, 6.0f });
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(color::textDim));
        ImGui::TextWrapped("Автострейф в воздухе сам набирает скорость. «По камере» — летишь куда смотришь (S — назад), «WASD» — направление клавишами. Старые bunny hop / auto strafe из «Основное» выключи.");
        ImGui::PopStyleColor();
    }
    endGroup();
}

}

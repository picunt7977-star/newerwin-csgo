#pragma once

#include <algorithm>
#include <cfloat>
#include <cmath>

#include <imgui/imgui.h>

#include "HvhGui.h"

// Общий стиль всего, что чит рисует поверх игры: флаги ESP, индикаторы у прицела, лента выстрелов,
// цифры урона. Один шрифт, одна палитра, один контур текста, одни плашки — чтобы экран читался
// как одно целое, а не как набор надписей разных цветов и размеров.
//
// Смысл цветов везде один:
//   text  — просто сведения            good — хорошо для нас (попал, убивает, окно открыто)
//   dim   — второстепенное             bad  — плохо/опасно (промах, бомба, враг ломает лагкомп)
//   info  — состояние врага            warn — внимание (перезаряжается, ослеплён, не наповал)
//   accent — наши режимы и эксплойты (цвет акцента меню)
namespace hvh::hud
{

namespace color
{
    inline constexpr ImU32 text  = IM_COL32(241, 243, 247, 255);
    inline constexpr ImU32 dim   = IM_COL32(168, 175, 190, 255);
    inline constexpr ImU32 good  = IM_COL32(112, 218, 128, 255);
    inline constexpr ImU32 bad   = IM_COL32(242, 92, 92, 255);
    inline constexpr ImU32 warn  = IM_COL32(246, 202, 92, 255);
    inline constexpr ImU32 info  = IM_COL32(124, 192, 255, 255);
    inline constexpr ImU32 plate = IM_COL32(12, 14, 19, 210);     // подложка плашек — как фон меню
    inline constexpr ImU32 track = IM_COL32(255, 255, 255, 28);   // пустая часть полоски
}

[[nodiscard]] inline ImU32 accent() noexcept { return ui::color::accent; }

[[nodiscard]] inline ImU32 withAlpha(ImU32 value, float alpha) noexcept
{
    const auto a = static_cast<unsigned>(std::clamp(alpha, 0.0f, 1.0f) * static_cast<float>((value >> IM_COL32_A_SHIFT) & 0xFF));
    return (value & ~IM_COL32_A_MASK) | (a << IM_COL32_A_SHIFT);
}

// мелкий шрифт — флаги, подписи; жирный — то, что надо увидеть сразу (тег события, цифра урона)
[[nodiscard]] inline ImFont* smallFont() noexcept
{
    return ui::fonts.uiSmall ? ui::fonts.uiSmall : ui::fonts.menu ? ui::fonts.menu : ImGui::GetFont();
}

[[nodiscard]] inline ImFont* boldFont() noexcept
{
    return ui::fonts.uiBold ? ui::fonts.uiBold : smallFont();
}

[[nodiscard]] inline ImVec2 measure(ImFont* font, const char* string) noexcept
{
    return font->CalcTextSizeA(font->FontSize, FLT_MAX, 0.0f, string);
}

// текст с тонким тёмным контуром: читается и на снегу, и на тёмной стене
inline void text(ImDrawList* drawList, ImFont* font, ImVec2 position, ImU32 value, const char* string) noexcept
{
    const float alpha = static_cast<float>((value >> IM_COL32_A_SHIFT) & 0xFF) / 255.0f;
    const ImU32 outline = withAlpha(IM_COL32(0, 0, 0, 255), alpha * 0.8f);
    position = { std::floor(position.x), std::floor(position.y) };
    static constexpr float offsets[4][2]{ { 1.0f, 0.0f }, { -1.0f, 0.0f }, { 0.0f, 1.0f }, { 0.0f, -1.0f } };
    for (const auto& offset : offsets)
        drawList->AddText(font, font->FontSize, { position.x + offset[0], position.y + offset[1] }, outline, string);
    drawList->AddText(font, font->FontSize, position, value, string);
}

// текст по центру точки (по горизонтали)
inline void textCentered(ImDrawList* drawList, ImFont* font, ImVec2 center, ImU32 value, const char* string) noexcept
{
    text(drawList, font, { center.x - measure(font, string).x * 0.5f, center.y }, value, string);
}

// плашка: тёмная скруглённая подложка + текст. Возвращает размер — чтобы ставить плашки в ряд/столбик
inline ImVec2 chip(ImDrawList* drawList, ImVec2 position, ImU32 value, const char* string, float alpha = 1.0f) noexcept
{
    ImFont* font = smallFont();
    const ImVec2 size = measure(font, string);
    const ImVec2 padding{ 6.0f, 2.0f };
    const ImVec2 full{ size.x + padding.x * 2.0f, size.y + padding.y * 2.0f };
    position = { std::floor(position.x), std::floor(position.y) };
    drawList->AddRectFilled(position, { position.x + full.x, position.y + full.y }, withAlpha(color::plate, alpha), 4.0f);
    drawList->AddText(font, font->FontSize, { position.x + padding.x, position.y + padding.y }, withAlpha(value, alpha), string);
    return full;
}

// полоска заполнения (заряд, замедление, окно LC)
inline void bar(ImDrawList* drawList, ImVec2 position, float width, float height, float fraction, ImU32 value, float alpha = 1.0f) noexcept
{
    position = { std::floor(position.x), std::floor(position.y) };
    const float rounding = height * 0.5f;
    drawList->AddRectFilled({ position.x - 1.0f, position.y - 1.0f }, { position.x + width + 1.0f, position.y + height + 1.0f },
        withAlpha(IM_COL32(0, 0, 0, 170), alpha), rounding + 1.0f);
    drawList->AddRectFilled(position, { position.x + width, position.y + height }, withAlpha(color::track, alpha), rounding);
    const float filled = width * std::clamp(fraction, 0.0f, 1.0f);
    if (filled >= 1.0f)
        drawList->AddRectFilled(position, { position.x + filled, position.y + height }, withAlpha(value, alpha), rounding);
}

// точка с тёмной обводкой и мягким ореолом (точки мультипоинта, попадания)
inline void dot(ImDrawList* drawList, ImVec2 center, float radius, ImU32 value, float alpha = 1.0f) noexcept
{
    drawList->AddCircleFilled(center, radius * 2.4f, withAlpha(value, alpha * 0.16f));
    drawList->AddCircleFilled(center, radius + 1.0f, withAlpha(IM_COL32(0, 0, 0, 255), alpha * 0.85f));
    drawList->AddCircleFilled(center, radius, withAlpha(value, alpha));
}

// линия с тёмной обводкой
inline void line(ImDrawList* drawList, ImVec2 from, ImVec2 to, ImU32 value, float alpha = 1.0f, float thickness = 1.5f) noexcept
{
    drawList->AddLine(from, to, withAlpha(IM_COL32(0, 0, 0, 255), alpha * 0.75f), thickness + 1.5f);
    drawList->AddLine(from, to, withAlpha(value, alpha), thickness);
}

// плавное появление: 0 → 1 за duration секунд, с торможением к концу
[[nodiscard]] inline float easeOut(float age, float duration) noexcept
{
    const float t = std::clamp(age / duration, 0.0f, 1.0f);
    return 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);
}

}

#pragma once

#include <imgui/imgui.h>

// Размытие фона под меню: кадр игры → уменьшение → гауссово размытие шейдером (OpenGL).
// Реализация — в HvhGui.cpp (новый .cpp потребовал бы перегенерации cmake).
namespace hvh::blur
{

// Из GUI: в этом кадре нужен размытый фон (меню открыто). Без запроса ничего не считается.
void request() noexcept;
// Из swapWindowHook: после того как игра отрисовала кадр, до отрисовки ImGui
void process() noexcept;
// Текстура с размытым кадром (0 — ещё нет) и UV для прямоугольника экрана
[[nodiscard]] ImTextureID texture() noexcept;
void uv(const ImVec2& min, const ImVec2& max, ImVec2& uv0, ImVec2& uv1) noexcept;
// Освобождение GL-объектов при выгрузке
void shutdown() noexcept;

}

#pragma once

#include <imgui/imgui.h>

class EngineInterfaces;
class ClientInterfaces;
struct Memory;

namespace hvh::preview
{

// Из игрового потока (конец кадра): снять кости модели игрока для предпросмотра в меню
void capture(const EngineInterfaces& engineInterfaces, const ClientInterfaces& clientInterfaces, const Memory& memory) noexcept;
// Модель меню (агент или по умолчанию) в базовой позе, без API движка.
// pin — показывать её и в игре (вкладка агентов), пока вызывается каждый кадр
void captureMenu(bool pin = false) noexcept;
// Модель для меню (выбранный агент); пустая строка — модель по умолчанию
void setMenuModel(const char* path) noexcept;

// Нарисовать модель в прямоугольнике. visible/hidden — цвета чамсов (0 — слой выключен).
// Возвращает false, если модели ещё нет (не в игре); bounds — экранная рамка модели для ESP.
// textured — с текстурами модели (иначе однотонно цветом чамсов)
bool draw(ImDrawList* drawList, const ImVec2& min, const ImVec2& max, ImU32 visible, ImU32 hidden, ImVec2& boundsMin, ImVec2& boundsMax, bool textured = false) noexcept;

}

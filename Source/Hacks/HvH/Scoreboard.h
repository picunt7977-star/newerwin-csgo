#pragma once

class ClientInterfaces;
class EngineInterfaces;
class OtherInterfaces;
struct Memory;
struct ImDrawList;

namespace hvh::scoreboard
{

// Из CreateMove: пока открыта таблица (Tab), раз в 0.25 с собирает снаряжение врагов
void update(const Memory& memory, const OtherInterfaces& interfaces, const ClientInterfaces& clientInterfaces,
    const EngineInterfaces& engineInterfaces, bool open, float realtime) noexcept;
// Поток рендера: панель со снаряжением справа от таблицы
void draw(ImDrawList* drawList) noexcept;

}

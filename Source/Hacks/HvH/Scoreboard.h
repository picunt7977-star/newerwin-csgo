#pragma once

class ClientInterfaces;
class EngineInterfaces;
class OtherInterfaces;
struct Memory;

namespace hvh::scoreboard
{

// Из CreateMove: пока открыта таблица (Tab), раз в 0.25 с вставляет иконки снаряжения врагов в их строки
void update(const Memory& memory, const OtherInterfaces& interfaces, const ClientInterfaces& clientInterfaces,
    const EngineInterfaces& engineInterfaces, bool open, float realtime) noexcept;

}

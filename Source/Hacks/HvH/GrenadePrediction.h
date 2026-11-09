#pragma once

struct ImDrawList;
class EngineInterfaces;
struct Memory;
namespace csgo { struct UserCmd; }

namespace hvh::grenade
{

// Из CreateMove: просчитать полёт гранаты в руке (трейсы — только из игрового потока)
void update(const EngineInterfaces& engineInterfaces, const Memory& memory) noexcept;
void draw(ImDrawList* drawList) noexcept;

// Супертосс: в тик броска гранаты подменяет угол команды (тихо), чтобы скорость игрока не уводила гранату.
// Звать после антиаима, до фикса движения. true — угол подменён в этом тике.
bool superToss(csgo::UserCmd* cmd, const Memory& memory) noexcept;

}

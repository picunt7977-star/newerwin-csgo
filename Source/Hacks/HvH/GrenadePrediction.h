#pragma once

struct ImDrawList;
class EngineInterfaces;
struct Memory;

namespace hvh::grenade
{

// Из CreateMove: просчитать полёт гранаты в руке (трейсы — только из игрового потока)
void update(const EngineInterfaces& engineInterfaces, const Memory& memory) noexcept;
void draw(ImDrawList* drawList) noexcept;

}

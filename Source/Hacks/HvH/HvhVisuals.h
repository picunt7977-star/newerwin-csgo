#pragma once

class ClientInterfaces;
class OtherInterfaces;
struct ImDrawList;
struct Memory;
struct ImVec2;

namespace csgo
{
    struct UserCmd;
}

// HvH-визуалы поверх обычного ESP: флаги врагов, точка прицела рейджа, снаряжение по Tab,
// цифры урона, круг разброса, кольца шагов, вспышка при убийстве, хитбоксы цели при выстреле.
namespace hvh::visuals
{

// из CreateMove (игровой поток): открыта ли таблица (Tab) и разброс своего оружия
void onCreateMove(const csgo::UserCmd* cmd) noexcept;
// из CreateMove: дым и огонь в мире для таймеров
void collectGrenades(const ClientInterfaces& clientInterfaces, const Memory& memory) noexcept;
// события игры
void onDamage(int victimIndex, int damage, bool headshot) noexcept;
void onKill() noexcept;
// bullet_impact своей пули (игровой поток): точка попадания и откуда стреляли — для искр
void onImpact(float x, float y, float z, float fromX, float fromY, float fromZ) noexcept;

// предпросмотр в меню: пример флагов ESP, position — правый верхний угол рамки модели
void drawPreviewFlags(ImDrawList* drawList, ImVec2 position) noexcept;

// поток рендера; realtime — memory.globalVars->realtime (на нём живут снимки рейджа)
void draw(ImDrawList* drawList, float realtime) noexcept;

}

// Вид мира (WorldConfig): цвет стен/пропов/неба, туман, подсветка. Вызывать в RENDER_START.
namespace hvh::world
{

void apply(const OtherInterfaces& interfaces, bool inGame) noexcept;
// свой туман включён — хук shouldDrawFog должен пускать туман, даже если в Osiris стоит «No fog»
[[nodiscard]] bool fogActive() noexcept;

}

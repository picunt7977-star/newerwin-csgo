#pragma once

struct ImDrawList;
class EngineInterfaces;
class ClientInterfaces;
struct Memory;

namespace csgo
{
    struct UserCmd;
    struct Engine;
    struct Cvar;
}

namespace hvh::movement
{

// До предсказания: бхоп и автострейф (viewYaw — куда смотрит камера)
void bunnyHop(csgo::UserCmd* cmd) noexcept;
void autoStrafe(csgo::UserCmd* cmd, float viewYaw, const csgo::Cvar& cvar, float interval) noexcept;

// До предсказания: фейкдак (приседание по чокнутым тикам)
void fakeDuck(csgo::UserCmd* cmd) noexcept;
// телепорт по клавише и дефенсив (рывок, когда враг только что нас увидел)
void exploits(const EngineInterfaces& engineInterfaces, const ClientInterfaces& clientInterfaces, const Memory& memory) noexcept;
[[nodiscard]] bool isFakeDucking() noexcept;

// Автобай: round_start ставит покупку в очередь, CreateMove отправляет её, когда мы живы
void onRoundStart() noexcept;
void autoBuy(const csgo::Engine& engine) noexcept;

// После рейджбота: автопик. viewYaw — куда смотрит камера (до сайлента/антиаима)
void autoPeek(csgo::UserCmd* cmd, float viewYaw, bool shot) noexcept;

// Перевод движения из системы камеры в систему итоговых углов команды
void fixMovement(csgo::UserCmd* cmd, float viewYaw) noexcept;

// После fixMovement: быстрая лестница (своими углами и кнопками)
void fastLadder(csgo::UserCmd* cmd, float viewPitch, float viewYaw) noexcept;

// «Без зума»: каждый кадр возвращаем мышиный поворот к обычной сенсе, игра в скоупе её режет
void keepZoomSensitivity(const csgo::Engine& engine, const csgo::Cvar& cvar) noexcept;
// при «без зума»: своё увеличение второго уровня скопа (1 — без увеличения)
[[nodiscard]] float scopeZoomFactor() noexcept;
// «Ровный ESP»: mat_queue_mode 0, пока включено (выключили — возвращаем авто)
void syncRendering(const csgo::Cvar& cvar) noexcept;

void draw(ImDrawList* drawList) noexcept;

}

#pragma once

struct ImDrawList;
class EngineInterfaces;
class ClientInterfaces;
struct Memory;
class OtherInterfaces;

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
void fakeDuck(csgo::UserCmd* cmd, float interval) noexcept;
// Каждый тик: сглаживание своей модели на время эксплойтных рывков и чтение серверных лимитов
// (sv_maxusrcmdprocessticks — сколько команд сервер обрабатывает за раз)
void exploitTuning(const csgo::Cvar& cvar) noexcept;
// телепорт по клавише и дефенсив (рывок, когда враг только что нас увидел)
void exploits(const EngineInterfaces& engineInterfaces, const ClientInterfaces& clientInterfaces, const Memory& memory) noexcept;
[[nodiscard]] bool isFakeDucking() noexcept;
// под ФД: можно ли стрелять на этом тике (фаза «стоя», модель встала); без ФД — всегда да
[[nodiscard]] bool fakeDuckShotTick() noexcept;
// присед (0..1) на отправленном тике и минимальный за цикл — для индикатора FD
void fakeDuckStats(float& sent, float& low) noexcept;

// Автобай: round_start ставит покупку в очередь, CreateMove отправляет её, когда мы живы
void onRoundStart() noexcept;
void autoBuy(const csgo::Engine& engine) noexcept;

// После рейджбота: автопик. viewYaw — куда смотрит камера (до сайлента/антиаима)
void autoPeek(csgo::UserCmd* cmd, float viewYaw, bool shot, float interval) noexcept;

// После рейджбота, до autoPeek: AI peek (сам выходит туда, откуда пробиваем врага). rageHasTarget — рейдж
// уже нашёл точку на этом тике (тогда стоп и выстрел — его дело)
void aiPeek(csgo::UserCmd* cmd, float viewPitch, float viewYaw, bool rageHasTarget, const EngineInterfaces& engineInterfaces,
            const ClientInterfaces& clientInterfaces, const OtherInterfaces& interfaces, const Memory& memory) noexcept;
[[nodiscard]] bool aiPeekMoving() noexcept;
// До рейджбота: авто хайдшоты (стоим/шаг/присед — DT уступает хайдшотам)
void autoHideShots(const csgo::UserCmd* cmd) noexcept;

// FrameStageNotify RENDER_START после оригинала: аним брейкер своей модели
void animBreaker(float realtime) noexcept;

// Перевод движения из системы камеры в систему итоговых углов команды
void fixMovement(csgo::UserCmd* cmd, float viewYaw) noexcept;

// После fixMovement: быстрая лестница (своими углами и кнопками)
void fastLadder(csgo::UserCmd* cmd, float viewPitch, float viewYaw) noexcept;
// до движкового предсказания — запомнить скорость падения; после — jumpbug, если этот тик приземлит
void noFallDamagePre() noexcept;
void noFallDamage(csgo::UserCmd* cmd, int flagsBeforePrediction) noexcept;

// «Без зума»: каждый кадр возвращаем мышиный поворот к обычной сенсе, игра в скоупе её режет
void keepZoomSensitivity(const csgo::Engine& engine, const csgo::Cvar& cvar) noexcept;
// при «без зума»: своё увеличение второго уровня скопа (1 — без увеличения)
[[nodiscard]] float scopeZoomFactor() noexcept;
// «Ровный ESP»: mat_queue_mode 0, пока включено (выключили — возвращаем авто)
void syncRendering(const csgo::Cvar& cvar) noexcept;

void draw(ImDrawList* drawList) noexcept;

}

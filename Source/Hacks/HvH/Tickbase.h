#pragma once

#include <cstddef>
#include <span>

namespace hvh::tickbase
{

// Ставит детур на CL_Move в engine_client.so и делает bSendPacket управляемым.
bool install(std::span<const std::byte> engineCode) noexcept;
void uninstall() noexcept;
[[nodiscard]] bool isInstalled() noexcept;

// Решение на текущую команду: true — пакет уйдёт, false — команда чокается.
// Принимается в начале CL_Move, до CreateMove, поэтому антиаим уже знает, какой это тик.
[[nodiscard]] bool sendPacket() noexcept;
[[nodiscard]] int chokedCommands() noexcept;

// Рейджбот сообщает о выстреле — DT сдвигает заряженные тики сразу после него
void onShot() noexcept;
// Следующая команда уйдёт без чока (рейдж будет стрелять)
void sendNextPacket() noexcept;
// Телепорт/дефенсив: сдвинуть заряженные тики без выстрела
void requestShift() noexcept;
[[nodiscard]] bool isShifting() noexcept;
[[nodiscard]] int chargedTicks() noexcept;
[[nodiscard]] bool isCharged() noexcept;
// Заряд ушёл на дефенсив (LC-триггер) и сейчас набирается заново — для индикатора DT это «включён»
[[nodiscard]] bool isDefensiveRecharging() noexcept;
// Чок фейкдака (≤14): снижается, если сервер выкидывает хвост пачки
[[nodiscard]] int fakeDuckChoke() noexcept;
void setFakeDuckChoke(int value) noexcept;
// DT включён (галка + клавиша-переключатель, не фейкдак) — от этого зависит телепорт автопика
[[nodiscard]] bool doubleTapEnabled() noexcept;
// Автопик выглядывает: при заряде от 4 тиков не дозаряжаться (заряд = пропуск команд, персонаж бы замер)
void holdShotShift(bool hold) noexcept;

// Фейкдак: чок до 14 тиков, DT на это время выключен
void setFakeDuck(bool active) noexcept;

// В игре/жив ли локальный игрок — ставится из CreateMove каждый тик
void setActive(bool active) noexcept;

// Окно дефенсива: наш m_nTickBase ниже максимума, который уже видел сервер, — новые записи лагкомпа
// по нам не пишутся (сервер не кладёт запись с тем же или меньшим simtime), и враг видит углы этих
// тиков, но не может по ним ни бектрекнуть, ни разрезольвить. Считается как lc_left в hysteria:
// максимум тикбейса − текущий − 1. Плюс пара тиков сразу после нашего сдвига.
void updateLagComp(int tickBase) noexcept;   // из CreateMove, каждый тик
[[nodiscard]] int defensiveTicksLeft() noexcept;
// AA просит ломать лагкомп (Force LC / LC-триггеры): держать заряд и сдвигать его, как только готов
void requestBreakLc() noexcept;
// Из CreateMove: заряд копим на земле и на взлёте; на спуске и при приземлении — нет: зарядка пропускает
// тики, и пропущенный тик приземления съедает прыжок распрыжки
void setMovement(bool onGround, bool jumpHeld, float verticalSpeed) noexcept;
// При включённом DT дефенсив рывков не делает (телепорт и мигающий DT) — только скрытые углы AA
[[nodiscard]] bool defensiveAllowed() noexcept;
// DT в режиме быстрой стрельбы (сдвиг на выстреле) / в дефенсивном режиме (заряд — на рывки при пике)
[[nodiscard]] bool fastFireEnabled() noexcept;
[[nodiscard]] bool defensiveDoubleTap() noexcept;
// Рейдж каждый тик: в руках оружие с перезарядкой > 0.4 с (скаут, AWP) — у него DT всегда дефенсивный
void setSlowWeapon(bool slow) noexcept;
// Нас сейчас видит враг (из movement::exploits): дефенсив DT в это время не дозаряжается
void setThreat(bool visible) noexcept;

}

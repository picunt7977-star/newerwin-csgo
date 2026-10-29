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
// Телепорт/дефенсив: сдвинуть заряженные тики без выстрела
void requestShift() noexcept;
[[nodiscard]] bool isShifting() noexcept;
[[nodiscard]] int chargedTicks() noexcept;
[[nodiscard]] bool isCharged() noexcept;

// Фейкдак: чок до 14 тиков, DT на это время выключен
void setFakeDuck(bool active) noexcept;

// В игре/жив ли локальный игрок — ставится из CreateMove каждый тик
void setActive(bool active) noexcept;

}

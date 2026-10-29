#pragma once

namespace csgo
{
    struct UserCmd;
}

class EngineInterfaces;
class ClientInterfaces;
struct Memory;

namespace hvh::antiaim
{

void updateInput() noexcept;
void run(csgo::UserCmd* cmd, const EngineInterfaces& engineInterfaces, const ClientInterfaces& clientInterfaces, const Memory& memory, bool shotThisTick) noexcept;

// для индикаторов
enum class Manual { None, Left, Right, Back };
[[nodiscard]] Manual manualDirection() noexcept;
[[nodiscard]] int desyncSide() noexcept;
[[nodiscard]] bool isFreestanding() noexcept;

// угол последней отправленной команды — его видит сервер и другие игроки
void storeSentAngles(float pitch, float yaw) noexcept;

// анти-брутфорс: подписка на bullet_impact (вызывать каждый кадр), сброс на начале раунда
void updateEventListener(const EngineInterfaces& engineInterfaces, const Memory& memory, bool forceRemove = false) noexcept;
void onRoundStart() noexcept;
[[nodiscard]] bool sentAngles(float& pitch, float& yaw) noexcept;

}

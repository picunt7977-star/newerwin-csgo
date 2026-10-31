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
enum class Manual { None, Left, Right, Back, Forward };
[[nodiscard]] Manual manualDirection() noexcept;
[[nodiscard]] int desyncSide() noexcept;
[[nodiscard]] bool isFreestanding() noexcept;
[[nodiscard]] const char* stateName() noexcept;   // состояние билдера, по которому сейчас AA
[[nodiscard]] bool defensiveActive() noexcept;    // сейчас скрытые углы дефенсива
// roll (наклон) этого тика: ставится в cmd->viewangles.z в самом конце CreateMove
[[nodiscard]] float rollAngle() noexcept;
// после fixMovement: поправить forward/side под наклон, иначе с roll персонаж идёт вбок
void fixRollMovement(csgo::UserCmd* cmd) noexcept;

// угол последней отправленной команды — его видит сервер и другие игроки
void storeSentAngles(float pitch, float yaw) noexcept;

// анти-брутфорс: подписка на bullet_impact (вызывать каждый кадр), сброс на начале раунда
void updateEventListener(const EngineInterfaces& engineInterfaces, const Memory& memory, bool forceRemove = false) noexcept;
void onRoundStart() noexcept;
[[nodiscard]] bool sentAngles(float& pitch, float& yaw) noexcept;

}

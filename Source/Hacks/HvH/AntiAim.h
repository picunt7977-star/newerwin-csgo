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

// контр-резольвер: по нам попали (hitgroup 1 — голова уточняет геометрию разбора выстрелов)
void onHurtByEnemy(int attackerIndex, int hitgroup, float realtime) noexcept;
// как, по мнению контр-резольвера, резольвит нас текущий враг («следит», «перебор» …), "" — нет данных
[[nodiscard]] const char* counterStyle() noexcept;

// угол последней отправленной команды — его видит сервер и другие игроки
void storeSentAngles(float pitch, float yaw) noexcept;

// анти-брутфорс: подписка на bullet_impact (вызывать каждый кадр), сброс на начале раунда
void updateEventListener(const EngineInterfaces& engineInterfaces, const Memory& memory, bool forceRemove = false) noexcept;
void onRoundStart() noexcept;
[[nodiscard]] bool sentAngles(float& pitch, float& yaw) noexcept;
// взгляд камеры этого тика до подмены: меню показывает, насколько AA уводит углы от настоящих
void cameraAngles(float& pitch, float& yaw) noexcept;
// Поза, которую видит сервер, по последнему отправленному пакету: взгляд, увод тела десинком
// (сторона × величина из билдера, 0 — без десинка) и roll
[[nodiscard]] bool sentServerPose(float& pitch, float& yaw, float& desync, float& roll) noexcept;

// extended yaw: для линий у ног — настоящий LBY с сервера и отправленный взгляд (рисуется в HvhVisuals)
struct ExtendedView {
    bool active = false;
    float origin[3]{};
    float lby = 0.0f;       // m_flLowerBodyYawTarget, как его прислал сервер
    float eyeYaw = 0.0f;    // отправленный взгляд
    float flickYaw = 0.0f;  // куда ломаем LBY
};
[[nodiscard]] ExtendedView extendedView() noexcept;

}

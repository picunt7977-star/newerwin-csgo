#pragma once

struct ImDrawList;

namespace csgo
{
    struct UserCmd;
}

class EngineInterfaces;
class ClientInterfaces;
class OtherInterfaces;
struct Memory;

namespace hvh::ragebot
{

// Возвращает true, если на этом тике выстрелили (антиаим тогда не трогает углы)
bool run(csgo::UserCmd* cmd, const EngineInterfaces& engineInterfaces, const ClientInterfaces& clientInterfaces, const OtherInterfaces& interfaces, const Memory& memory, float lerp) noexcept;

// для индикаторов
[[nodiscard]] bool minDamageOverrideActive() noexcept;
[[nodiscard]] bool forceBodyActive() noexcept;
[[nodiscard]] int currentTarget() noexcept;
[[nodiscard]] const char* status() noexcept;
// текущая поправка резольвера для игрока (0 — без поправки)
[[nodiscard]] float resolverOffsetFor(int index) noexcept;

// события игры для подсчёта промахов
void onPlayerHurt(int victimIndex, int attackerIndex, int localIndex, int hitgroup, int damage, int healthLeft) noexcept;
// свои bullet_impact — для логов промахов (вызывать каждый кадр; forceRemove — при выгрузке)
void updateEventListener(const EngineInterfaces& engineInterfaces, const Memory& memory, bool forceRemove = false) noexcept;
void onRoundStart() noexcept;
void onPlayerDeath(int victimIndex, int localIndex) noexcept;
// строка только в ~/csgo-hvh/logs/ДАТА.log, без консоли
void logToFile(const char* text) noexcept;
// попадания/промахи коротко на экране (последние 8, гаснут через 6 с)
void drawLogs(ImDrawList* drawList) noexcept;
// NET_UPDATE_POSTDATAUPDATE_START: запомнить присланные сервером углы взгляда, пока их не перезаписала интерполяция
void captureNetworkAngles(const EngineInterfaces& engineInterfaces, const ClientInterfaces& clientInterfaces) noexcept;

}

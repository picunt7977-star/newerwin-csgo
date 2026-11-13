#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

struct ImDrawList;

namespace csgo
{
    struct UserCmd;
    struct matrix3x4;
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
[[nodiscard]] bool forceShotActive() noexcept;
[[nodiscard]] int currentTarget() noexcept;
// цель есть и оружие готово (или вот-вот) — дефенсив в это время не чокает и не рвёт тикбейс,
// иначе выстрел ждёт в пачке и уходит на сервер с опозданием
[[nodiscard]] bool aimingNow() noexcept;
[[nodiscard]] const char* status() noexcept;
// текущая поправка резольвера для игрока (0 — без поправки)
[[nodiscard]] float resolverOffsetFor(int index) noexcept;

// Для ESP и визуалов: снимок под замком, читается из потока рендера
struct TargetView {
    float updatedAt = 0.0f;         // realtime, когда игрок последний раз был живым врагом
    float aimAt = 0.0f;             // когда рейдж последний раз нашёл по нему точку
    float aimPoint[3]{};
    bool aimLethal = false;         // урона в этой точке хватает убить
    int aimDamage = 0;
    int aimGroup = 0;               // хитгруппа: 1 голова, 2 грудь, 3 живот, 4-5 руки, 6-7 ноги
    bool aimedNow = false;          // по этой точке выстрелили
    bool breakingLagComp = false;   // ломает лагкомп (телепорт/дефенсив)
    bool charged = false;           // simtime сильно отстаёт — у врага заряжен DT
    const char* resolver = "";
    // все проверенные точки мультипоинта: 0 — не пробить, 1 — урон, 2 — убивает
    float pointsAt = 0.0f;
    int pointCount = 0;
    float points[64][3]{};
    unsigned char pointState[64]{};
};
[[nodiscard]] TargetView targetView(int index) noexcept;
// бектрек-чамсы: кости самой старой записи, по которой рейдж ещё может попасть (RECORD_BONES = 128 штук);
// false — записи нет, враг стоит или ломает лагкомп
[[nodiscard]] bool backtrackBones(int index, csgo::matrix3x4* out, float realtime) noexcept;

// скелет цели в момент последнего выстрела (кости, по которым стреляли)
struct ShotView {
    float at = 0.0f;
    int result = 0;                 // 0 — ждём, 1 — попал, -1 — мимо
    int count = 0;
    float a[64][3]{}, b[64][3]{};   // кость и её родитель
};
[[nodiscard]] ShotView shotView() noexcept;

// события игры для подсчёта промахов
void onPlayerHurt(int victimIndex, int attackerIndex, int localIndex, int hitgroup, int damage, int healthLeft) noexcept;
// свои bullet_impact — для логов промахов (вызывать каждый кадр; forceRemove — при выгрузке)
void updateEventListener(const EngineInterfaces& engineInterfaces, const Memory& memory, bool forceRemove = false) noexcept;
void onRoundStart() noexcept;
void onPlayerDeath(int victimIndex, int localIndex) noexcept;
// строка только в ~/csgo-hvh/logs/ДАТА.log, без консоли
void logToFile(const char* text) noexcept;
// в нас выстрелил враг (импакт рядом с нами): если урона от него не будет — уведомление «evaded»
void onShotAtUs(int shooterIndex, const char* name, const char* state) noexcept;
// ---- уведомления (худ «Notifications» рисует HvhGui.cpp) ----
enum class NoticeKind { Hit, Kill, Miss, Hurt, Evaded };
struct NoticePart {
    std::string text;
    bool value = false;     // белым (имя, куда, цифра); иначе — серая связка
    bool warn = false;      // оранжевый «!»: попали не туда / меньше урона, чем ждали
};
struct NoticeView {
    NoticeKind kind = NoticeKind::Hit;
    std::vector<NoticePart> parts;   // первое слово уже с большой буквы
    float age = 0.0f;                // секунд с появления
};
[[nodiscard]] std::array<std::uint8_t, 4> noticeColor(NoticeKind kind) noexcept;
// живые уведомления, новые первыми; заодно решает «evaded» по выстрелам в нас (звать раз в кадр)
[[nodiscard]] std::vector<NoticeView> recentNotices(float lifetime) noexcept;
// NET_UPDATE_POSTDATAUPDATE_START: запомнить присланные сервером углы взгляда, пока их не перезаписала интерполяция
void captureNetworkAngles(const EngineInterfaces& engineInterfaces, const ClientInterfaces& clientInterfaces) noexcept;

}

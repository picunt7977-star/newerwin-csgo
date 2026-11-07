#pragma once

#include <array>
#include <cstddef>

#include <CSGO/AnimState.h>
#include <CSGO/AnimationLayer.h>

class ClientInterfaces;
class EngineInterfaces;
struct Memory;

namespace csgo
{
struct Entity;
}

// Резольвер десинка по сравнению слоёв анимации.
//
// Сервер шлёт нам слои анимации игрока (m_AnimOverlay — единственное, что не вырезано из DT_CSPlayer:
// m_flPoseParameter как раз исключён). Эти слои сервер посчитал из НАСТОЯЩЕГО угла ног врага.
// Берём animstate врага, подставляем в него угол ног-кандидат, зовём ту же функцию движка
// (CCSGOPlayerAnimState::Update) и смотрим, какой кандидат даёт слои, совпадающие с сетевыми.
// Ошибку по каждому полю слоя нормируем на разброс этого поля между кандидатами — так в счёт идут
// только те слои, которые вообще зависят от угла ног (доворот ног, стрейф, lean), а не шум.
namespace hvh::resolver
{

// Снимок анимации врага: состояние animstate после прогона CCSGOPlayerAnimState::Update с
// заданным углом ног, и то, что этот прогон записал — слои и позы. Из снимка рейджбот собирает
// скелет (Ragebot.cpp: rebuildFromSnapshot): кости выходят ровно те, по которым считал сервер,
// а не «кости текущего кадра с подменённым body_yaw».
struct AnimSnapshot {
    bool ok = false;
    float simulationTime = -1.0f;   // обновление, к которому снимок относится
    float footYaw = 0.0f;           // угол ног после прогона (движок уже обрезал его по десинку)
    float eyeYaw = 0.0f;
    alignas(16) std::byte state[sizeof(csgo::AnimState)]{};
    std::array<csgo::AnimationLayer, csgo::ANIMATION_LAYER_COUNT> layers{};
    int layerCount = 0;
    std::array<float, 24> poses{};
};

// Прогнать анимацию врага с заданным углом ног — от состояния, с которого в этом обновлении
// начинал сервер, — и снять результат. Это главный поставщик скелетов для анимфикса: работает,
// только пока идёт то же обновление, на котором резольвер этого игрока разбирал
// (дальше база устарела, и прогон был бы от чужой фазы анимации).
[[nodiscard]] bool simulateAt(int index, const csgo::Entity& entity, const Memory& memory, float footYaw, AnimSnapshot& out) noexcept;
// Есть ли свежая база для simulateAt по этому игроку на это обновление
[[nodiscard]] bool canSimulate(int index, float simulationTime) noexcept;

// FRAME_NET_UPDATE_POSTDATAUPDATE_START: сетевые слои ещё не перезаписаны клиентской анимацией
void onNetUpdate(const EngineInterfaces& engineInterfaces, const ClientInterfaces& clientInterfaces, const Memory& memory) noexcept;
// FRAME_RENDER_START: слои после клиентской анимации — по ним проверяем, шлёт ли сервер свои
void afterClientAnimation(const EngineInterfaces& engineInterfaces, const ClientInterfaces& clientInterfaces) noexcept;

[[nodiscard]] bool available() noexcept;          // функция и офсеты найдены, сервер шлёт слои
[[nodiscard]] bool valid(int index) noexcept;     // по этому игроку есть угол ног
[[nodiscard]] float footYaw(int index) noexcept;  // угол ног на сервере, с коррекцией по промахам
[[nodiscard]] float delta(int index) noexcept;    // footYaw - взгляд, с учётом режима
[[nodiscard]] float maxDesync(int index) noexcept;
[[nodiscard]] float confidence(int index) noexcept;
// последний угол, найденный по слоям (не доведённый): simtime находки и дельта в долях максимума
[[nodiscard]] bool lastFound(int index, float& simulationTime, float& ratio) noexcept;
[[nodiscard]] const char* statusLine(int index) noexcept;

void onHit(int index) noexcept;                   // попали — режим коррекции хорош
// промах по вине резольвера; proven — импакт доказал, что сторона была неверной (весит вдвое)
void onMiss(int index, bool proven = false) noexcept;
// промах, где импакт доказал, что сторона была ВЕРНОЙ, а модель стояла дальше: ноги на полном десинке.
// Зеркалить тут нечего — резольвер для этого игрока начинает доводить угол до максимума по найденной стороне.
void onMagnitudeMiss(int index) noexcept;
[[nodiscard]] bool extremeActive(int index) noexcept;   // угол доводится до полного десинка (выучено промахами)
// По этому игроку идёт перебор вариантов коррекции: прошлый выстрел промахнулся по вине резольвера,
// и угол сейчас заведомо под вопросом — рейджботу стоит брать точки понадёжнее
[[nodiscard]] bool bruteforcing(int index) noexcept;
void onRoundStart() noexcept;
void clear() noexcept;

}

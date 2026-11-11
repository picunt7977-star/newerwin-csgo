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
// ticks > 0 — сдвинуть только столько (остаток заряда остаётся), 0 — весь заряд
// byDefensive — сдвиг просит дефенсив: длину пачки он потом забирает через takeDefensiveShift()
void requestShift(int ticks = 0, bool byDefensive = false) noexcept;
[[nodiscard]] bool isShifting() noexcept;
// Сколько команд пачки ещё впереди (0 — сдвига нет). Пока пачка идёт, сервер запишет лагкомп
// только по её ПОСЛЕДНЕЙ команде — остальные скрыты на любом сервере, без всякого отката
[[nodiscard]] int shiftLeft() noexcept;
// Длина последней ушедшей пачки сдвига — чья бы она ни была (рывок дефенсива, вторая пуля DT,
// телепорт автопика) и его же чоковая пачка. Забирается один раз (дальше 0)
[[nodiscard]] int takeShiftPack() noexcept;
// Хайдшоты (ручные или авто) сейчас действуют: выстрел только в чокнутой команде
[[nodiscard]] bool hideShotsActive() noexcept;
// авто хайдшоты подменили быстрый DT (для индикатора «HS»)
[[nodiscard]] bool autoHideShotsEngaged() noexcept;
// из CreateMove каждый тик: стоим/шаг/присед и оружие подходит — DT уступает хайдшотам
void setAutoHideShots(bool value) noexcept;
// следующая команда уйдёт в чок (хайдшоты: в ней будет выстрел)
void chokeNextPacket() noexcept;
[[nodiscard]] int chargedTicks() noexcept;
[[nodiscard]] bool isCharged() noexcept;
// нас сейчас видит хотя бы один враг
[[nodiscard]] bool threatened() noexcept;
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

// Сырой откат тикбейса: на сколько наш m_nTickBase ниже максимума, который мы уже видели.
// Пока он > 0, сервер не кладёт по нам новых записей лагкомпа (запись с тем же или меньшим simtime
// он отбрасывает). Значение приходит из снапшота, то есть устарело на пинг, и само по себе
// как «окно прямо сейчас» не годится — им пользуется только замер в Defensive.cpp
void updateLagComp(int tickBase) noexcept;      // из CreateMove, каждый тик
[[nodiscard]] int rawDeficit() noexcept;

// Дефенсив ставит цель заряда (0 — заряд не нужен) и то, как размазывать пропуски тиков:
// spread = 1 — пропускать подряд, N — 1 тик из N. Пропущенный тик — это несозданная команда,
// то есть замерший персонаж, поэтому подряд их много быть не должно
void setDefensiveCharge(int target, int spread) noexcept;
// Дефенсив «чоком»: держать в чоке столько команд и отправлять пачкой (0 — не нужно). В отличие
// от заряда пропуском, команды создаются — движение и мышь не теряются, а серверная модель просто
// отстаёт от нашей на длину чока. Пачка и есть окно: сервер обработает её за один свой тик
void setDefensiveChoke(int ticks) noexcept;
[[nodiscard]] int defensiveChokeTicks() noexcept;
// Лаг-пик: держать в чоке до `ticks` команд (0 — выключить). Выстрел отпускает пачку сам (onShot).
// Главнее фейклага и чока дефенсива, предел — только sv_maxusrcmdprocessticks
void setLagPeekChoke(int ticks) noexcept;
// Сколько реально держим сейчас (0 — лаг-пик не действует: DT быстрая стрельба, фейкдак, выключен)
[[nodiscard]] int lagPeekChokeTicks() noexcept;
// Последняя пачка из takeShiftPack() была чоковой (тикбейс не двигала — отката от неё не бывает)
[[nodiscard]] bool lastPackWasChoke() noexcept;
// sv_maxusrcmdprocessticks, если сервер его реплицирует: больше этого в пачке держать нельзя —
// хвост сервер выкинет (0 — неизвестно, тогда предел 14)
void setProcessLimit(int ticks) noexcept;
[[nodiscard]] int processLimitTicks() noexcept;
// Из CreateMove: заряд копим на земле и на взлёте; на спуске и при приземлении — нет: зарядка пропускает
// тики, и пропущенный тик приземления съедает прыжок распрыжки
// horizontalSpeed — для плавной перезарядки дефенсива в движении (заряд через тик, а не замирание на 13)
void setMovement(bool onGround, bool jumpHeld, float verticalSpeed, float horizontalSpeed, float interval) noexcept;
// Дефенсив может тратить заряд: детур стоит, мы живы, не фейкдак и заряд не занят второй пулей DT
// (быстрая стрельба). Сам DT для дефенсива не нужен — у него свой заряд
[[nodiscard]] bool defensiveAllowed() noexcept;
// DT в режиме быстрой стрельбы (сдвиг на выстреле) / в дефенсивном режиме (заряд — на рывки при пике)
[[nodiscard]] bool fastFireEnabled() noexcept;
[[nodiscard]] bool defensiveDoubleTap() noexcept;
// Рейдж каждый тик: в руках оружие с перезарядкой > 0.4 с (скаут, AWP) — у него DT всегда дефенсивный
void setSlowWeapon(bool slow) noexcept;
// В руках нож (из CreateMove каждый тик)
void setKnife(bool knife) noexcept;
// Сервер наш (loopback): sv_maxusrcmdprocessticks из cvar верный; иначе — лимит из настроек
void setServerLocal(bool local) noexcept;
// Нож + прыжок + DT и включена «Нож в прыжке — без лагов»: чока, лаг-пика и рывков нет
[[nodiscard]] bool knifeAirQuiet() noexcept;
// Нас сейчас видит враг (из movement::exploits): дефенсив DT в это время не дозаряжается
void setThreat(bool visible) noexcept;

}

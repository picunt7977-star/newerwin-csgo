#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <array>
#include <chrono>
#include <dlfcn.h>

#include <sys/mman.h>
#include <unistd.h>

#include "HvhConfig.h"
#include "Ragebot.h"
#include "Tickbase.h"

// CL_Move(float accumulated_extra_samples, bool bFinalTick) в engine_client.so (Linux, legacy-билд).
// bSendPacket компилятор держит в r13, значение по умолчанию задаёт `mov r13d, 1` внутри CL_Move —
// меняя этот immediate перед вызовом оригинала, решаем, уйдёт ли пакет на этом тике.

namespace hvh::tickbase
{

namespace
{

// push rbp; mov rbp,rsp; push r15; push r14; mov r14d,edi; push r13; push r12; push rbx; sub rsp,0x178; movss [rbp-0x194],xmm0
constexpr std::uint8_t clMovePrologue[]{ 0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x89, 0xFE, 0x41, 0x55, 0x41, 0x54, 0x53,
                                         0x48, 0x81, 0xEC, 0x78, 0x01, 0x00, 0x00, 0xF3, 0x0F, 0x11, 0x85, 0x6C, 0xFE, 0xFF, 0xFF };
// mov r13d, 1; jmp rel32
constexpr std::uint8_t sendPacketDefault[]{ 0x41, 0xBD, 0x01, 0x00, 0x00, 0x00, 0xE9 };
// первые 16 байт пролога — целые инструкции без RIP-относительной адресации, их можно перенести в трамплин
constexpr std::size_t stolenBytes = 16;

using ClMove = void (*)(float, bool);

std::uint8_t* clMove = nullptr;
std::uint8_t* sendPacketImm = nullptr;
std::uint8_t* trampoline = nullptr;
std::uint8_t originalBytes[stolenBytes];

bool active = false;
bool send = true;
int choked = 0;
bool fakeDuck = false;
int charged = 0;
int rechargeDelay = 0;
bool shotPending = false;
bool shiftRequested = false;
int requestedTicks = 0;          // сколько тиков сдвига просили (0 — весь заряд)
bool shifting = false;
bool holdShot = false;
bool sendNext = false;
bool unsentShot = false;     // выстрел лежит в неотправленной пачке
int shiftDeferred = 0;       // сколько тиков подряд сдвиг по запросу ждёт пачку с выстрелом
int maxTickBase = 0;
int deficit = 0;                // сырой откат тикбейса: максимум − текущий − 1
int defensiveTarget = 0;        // цель заряда под дефенсив ПРОПУСКОМ тиков (0 — не нужен)
// Цель заряда под дефенсив ЧОКОМ (0 — не нужен). Разница принципиальная: при чоке команда
// создаётся — движение предсказывается, мышь опрашивается, — и на сервер она просто уходит позже.
// Серверная модель отстаёт от нашей на длину чока, а мы едем ровно. Пропуск же вызова CL_Move
// команду не создаёт вовсе: на этом тике персонаж никуда не идёт (при «1 из 3» это треть скорости),
// а потом пачка прогоняется за один тик, предсказание улетает вперёд и коррекция тянет назад.
int defensiveChoke = 0;
int lagPeekChoke = 0;           // лаг-пик: держать в чоке до стольких команд (0 — выключен)
bool lastPackChoke = false;     // последняя пачка в shiftPackDone — чоковая, а не сдвиг тикбейса
int processLimit = 0;           // сколько команд сервер обработает за тик (0 — неизвестно)
int cvarProcessLimit = 0;       // sv_maxusrcmdprocessticks у клиента (на удалённом сервере — не его значение)
bool serverLocal = false;
bool knifeHeld = false;
int defensiveSpread = 3;        // пропускать 1 тик из N при зарядке под дефенсив
int groundTicks = 0;            // сколько тиков подряд стоим на земле без прыжка
bool rising = false;            // летим вверх — до приземления далеко, пропуск тика не съест прыжок
bool airborne = false;
bool movingFast = false;        // бежим/идём — перезарядку дефенсива растягиваем
float horizontalSpeed = 0.0f;   // своя скорость по горизонтали, юнитов/с
bool moveInput = false;         // в прошлой команде был зажат WASD
float tickInterval = 1.0f / 64.0f;
int randomChoke = 0;            // режим фейклага «Случайно»: лимит, выбранный на текущий чок
// Чок фейкдака. Сервер обрабатывает за раз не больше sv_maxusrcmdprocessticks команд (по умолчанию 16,
// на части HvH-серверов 8): лишние команды пачки он выкидывает — а это как раз тики приседа в её конце,
// и враг видит нас стоящими через раз. Movement снижает чок, если на отправке оказались не в приседе.
int fakeDuckChokeLimit = 14;
bool threat = false;             // нас сейчас видит враг
bool slowWeapon = false;         // в руках скаут/AWP и прочее с перезарядкой > 0.4 с — второй пули DT нет
bool lcShift = false;            // этот сдвиг просил дефенсив (не выстрел и не телепорт автопика)
int shiftRemaining = 0;          // сколько команд пачки ещё впереди (пока идёт сдвиг)
int shiftPackDone = 0;           // тиков в последней ушедшей пачке сдвига — забирает Defensive.cpp
bool defensiveRecharge = false; // заряд потрачен дефенсивом и сейчас набирается заново — DT при этом «включён»
bool autoHs = false;             // авто хайдшоты: сейчас вместо быстрой стрельбы DT
bool forceChoke = false;         // хайдшоты: следующую команду — в чок (в ней будет выстрел)
std::uint8_t* find(std::span<const std::byte> code, const std::uint8_t* pattern, std::size_t size) noexcept
{
    if (code.size() < size)
        return nullptr;
    const auto begin = reinterpret_cast<const std::uint8_t*>(code.data());
    for (std::size_t i = 0; i + size <= code.size(); ++i) {
        if (std::memcmp(begin + i, pattern, size) == 0)
            return const_cast<std::uint8_t*>(begin + i);
    }
    return nullptr;
}

bool makeWritable(void* address, std::size_t size) noexcept
{
    const auto pageSize = static_cast<std::uintptr_t>(sysconf(_SC_PAGESIZE));
    const auto start = reinterpret_cast<std::uintptr_t>(address) & ~(pageSize - 1);
    const auto end = (reinterpret_cast<std::uintptr_t>(address) + size + pageSize - 1) & ~(pageSize - 1);
    return mprotect(reinterpret_cast<void*>(start), end - start, PROT_READ | PROT_WRITE | PROT_EXEC) == 0;
}

void writeAbsoluteJump(std::uint8_t* at, const void* target) noexcept
{
    // jmp qword ptr [rip+0]; dq target
    at[0] = 0xFF;
    at[1] = 0x25;
    std::memset(at + 2, 0, 4);
    const auto address = reinterpret_cast<std::uintptr_t>(target);
    std::memcpy(at + 6, &address, sizeof(address));
}

void setSendPacket(bool value) noexcept
{
    send = value;
    if (sendPacketImm)
        *sendPacketImm = value ? 1 : 0;
}

[[nodiscard]] bool wantDoubleTap() noexcept
{
    const auto& ex = config.exploits;
    return active && !fakeDuck && ex.doubleTap && (!ex.doubleTapKey.isSet() || ex.doubleTapKey.isToggled());
}

// заряд нужен и для DT, и для телепорта/дефенсива. Автопик своего заряда не копит: телепорт на
// возврате берёт заряд DT, только если DT включён (иначе назначенная клавиша автопика держала DT всегда)
// DT стреляет второй пулей (сдвиг на выстреле). Иначе DT — дефенсивный: заряд для рывков, выстрел обычный
[[nodiscard]] bool fastFire() noexcept
{
    return wantDoubleTap() && config.exploits.doubleTapMode == 0 && !slowWeapon && !autoHs;
}

// Хайдшоты работают, когда DT не стреляет второй пулей: быстрая стрельба главнее (как в Neverlose)
[[nodiscard]] bool hideShotsOn() noexcept
{
    const auto& ex = config.exploits;
    const bool manual = ex.hideShots && (!ex.hideShotsKey.isSet() || ex.hideShotsKey.isToggled());
    return active && !fakeDuck && !fastFire() && (manual || autoHs);
}

// Сколько тиков копим и сдвигаем. Вторая пуля DT — по своему ползунку (ей нужен полный сдвиг),
// дефенсиву хватает меньшего: сервер откатывает тикбейс, как только промах больше
// sv_clockcorrection_msecs (~60 мс ≈ 4 тика), и чем меньше заряд, тем чаще приходит окно.
[[nodiscard]] int shiftSize() noexcept
{
    // сдвиг уходит одним пакетом — не больше, чем сервер обработает за тик
    const int ceiling = processLimit > 0 ? std::clamp(processLimit - 1, 1, 14) : 14;
    if (fastFire())
        return std::clamp(config.exploits.shiftTicks, 1, ceiling);
    return defensiveTarget > 0 ? std::clamp(defensiveTarget, std::min(4, ceiling), ceiling) : std::clamp(config.exploits.shiftTicks, 1, ceiling);
}

// Зарядка — это пропуск вызова CL_Move: команда не создаётся, а вместе с ней не опрашивается мышь
// и не идёт предсказание. 13 пропусков подряд — это 200 мс, на которые замирает и персонаж, и КАМЕРА,
// и весь ввод: именно это чувствуется как «дёргает и не даёт двигаться».
// Поэтому пропуски размазываем: 1 тик из N. Один пропущенный тик — это 15 мс, не заметно.
// Платим частотой окон, поэтому N вынесен в меню.
int chargeTick = 0;

[[nodiscard]] bool skipThisTick() noexcept
{
    // Стоим — пропущенная команда не видна вовсе: идти всё равно некуда. Рвём подряд, заряд
    // набирается за свои 13 тиков.
    // ...но только если WASD не зажат. Стоим и жмём — значит трогаемся: раньше тут пропускались
    // все тики подряд, команды не было, скорость оставалась 0 — и персонаж стоял у стены, пока не
    // наберётся весь заряд (после телепорта автопика — 14 тиков, на каждом пике)
    if (horizontalSpeed < 5.0f && !moveInput)
        return true;
    // В движении пропущенный тик — это потерянный шаг. Подряд они дают ровно то самое «иду вперёд,
    // останавливаюсь, и чуть-чуть назад»: персонаж стоит всю зарядку, а потом предсказание догоняет
    // сервер рывком. Размазываем: 1 тик из N. Для второй пули DT берём не меньше 3 — заряд
    // наберётся втрое дольше, зато бежать при этом можно.
    int every = std::clamp(defensiveSpread, 1, 6);
    if (wantDoubleTap())
        every = std::max(every, 3);
    if (every <= 1)
        return true;
    chargeTick = (chargeTick + 1) % every;
    return chargeTick == 0;
}

// Нужен ли заряд ПРОПУСКАМИ тиков. Раньше тут стояло «включён DT — значит копим», и пропуски
// шли всегда, когда стоит галка DT, даже если тратить заряд было некому: в дефенсивном режиме DT
// (не быстрая стрельба) заряд нужен только рывкам дефенсива и телепортам. Из-за этого дефенсив,
// переведённый на чок, всё равно получал пропуски от DT — персонаж замирал, а потом его тянуло
// назад, хотя чок к этому отношения не имел.
[[nodiscard]] bool wantCharge() noexcept
{
    if (!active || fakeDuck)
        return false;
    const auto& ex = config.exploits;
    // DT включён (любой режим) — заряд нужен всегда: переключил DT — он должен перезарядиться,
    // как в NL. Раньше в режиме «Дефенсив» без назначенной клавиши автопика заряд не копился вовсе
    if (fastFire() || wantDoubleTap())
        return true;
    // телепорт по клавише и телепорт автопика на возврате берут заряд DT. Стоя заряд копится
    // бесплатно, на ходу chargeAllowed() потребует, чтобы клавиша была нажата
    if (ex.teleportKey.isSet())
        return true;
    if (wantDoubleTap() && config.movement.autoPeekKey.isSet())
        return true;
    // дефенсив в стиле «пропуском тиков»; в стиле «чоком» заряд ему не нужен
    return defensiveTarget > 0;
}

// Можно ли пропустить этот тик под заряд. Было одно выражение на пять строк с тремя отрицаниями —
// разложено по причинам, потому что от каждой зависит, работает ли дефенсив вообще.
[[nodiscard]] bool chargeAllowed(int chargeTarget, int maxShift, bool lcRecharge) noexcept
{
    if (charged >= chargeTarget || shotPending || shiftRequested)
        return false;
    // Лаг-пик копит чок: пропуск тика тут — это лишний тик, на который пачка с выстрелом опоздает
    if (lagPeekChoke > 0)
        return false;
    // Чок дефенсива в воздухе: бюджет сервера занят пачками, заряд копить некуда
    if (defensiveChoke > 0 && airborne)
        return false;
    // Ждёт отправки пакет с выстрелом (хайдшоты кладут выстрел в чок, фейклаг тоже может) или рейдж
    // попросил отправить следующий тик. Пропуск тика под заряд тут — это не «один тик»: стоя пропуски
    // идут подряд до полного заряда, и выстрел висел в чоке до ~14 тиков (200 мс) — враг успевал
    // убить первым («нас убили раньше, чем сервер принял выстрел»). Сначала отправка, заряд потом.
    if (sendNext)
        return false;
    // Заряжаемся на земле (не сразу после приземления) и на взлёте. На спуске и на тике приземления —
    // нет: пропущенный тик там съедает прыжок распрыжки. На взлёте теряется лишь чуть стрейфа, зато
    // дефенсив работает и при распрыжке.
    if (groundTicks < 3 && !rising)
        return false;
    // Под прицелом обычно не заряжаемся: пропуск тика виден врагу как замирание. Исключение —
    // дефенсив: его окна и есть сдвиги, а сдвигать нечего, если под прицелом заряд не набирается
    // (раньше так и было: один рывок — и пусто, пока враг нас видит). Пропуски размазаны (skipThisTick).
    // DT включён и заряда нет — копим и под прицелом (размазано skipThisTick), иначе после
    // переключения DT на глазах у врага он не заряжался, пока враг не отвернётся
    if (threat && !lcRecharge && !wantDoubleTap() && (!fastFire() || charged >= maxShift))
        return false;
    // Автопик выглядывает: заряд = пропуск команд, персонаж бы замер прямо под прицелом
    if (holdShot && threat)
        return false;
    // Автопик зажат и мы идём (или жмём WASD) — это выглядывание: пропуск тика = шаг, которого нет,
    // и выходили мы медленно, рывками, прямо к врагу (13.11: треть попаданий по нам на бегу —
    // с неполным зарядом). Заряжаемся только стоя за укрытием, куда автопик и возвращает.
    if (holdShot && (horizontalSpeed > 5.0f || moveInput))
        return false;
    // Рейдж держит цель и оружие готово: пропущенный тик — это несозданная команда, то есть выстрел
    // на тик позже. Дефенсивному DT заряд не срочен — докопим после выстрела (пока оружие перезаряжается)
    if (!fastFire() && ragebot::aimingNow())
        return false;
    // На ходу пропущенный тик — это потерянный шаг, и так весь бег. Поэтому на ходу копим, только
    // если заряд нужен прямо сейчас: есть в кого стрелять второй пулей, нажата клавиша, которой
    // заряд тратят, или дефенсив копит под своё окно. Стоя копим всегда — там пропуск не виден.
    // Раньше хватало НАЗНАЧЕННОЙ клавиши телепорта или автопика, чтобы заряд копился постоянно,
    // и бег по пустой карте с включённым DT шёл с потерей каждого третьего тика.
    if (horizontalSpeed > 5.0f) {
        const auto& ex = config.exploits;
        // DT включён, а заряд не полный (полный отсекается в самом начале) — копим и на бегу
        const bool needNow = defensiveTarget > 0
            || wantDoubleTap()
            || (ex.teleportKey.isSet() && ex.teleportKey.isDown())
            || (config.movement.autoPeekKey.isSet() && config.movement.autoPeekKey.isDown())
            || (fastFire() && (threat || ragebot::currentTarget() != 0));
        if (!needNow)
            return false;
    }
    return true;
}

// Сколько тиков чока нужно, чтобы пройти заданное расстояние на текущей скорости.
// Сервер считает шаг больше 64 юнитов по горизонтали телепортом (CLagCompensationManager,
// m_flTeleportDistanceSqr) и запись лагкомпа по нам в этом случае не интерполирует: «адаптивный»
// держится под порогом (максимум чока, который сервер ещё отрабатывает честно), «ломать лагкомп»
// старается порог перешагнуть.
[[nodiscard]] int ticksForDistance(float distance) noexcept
{
    const float perTick = horizontalSpeed * tickInterval;
    if (perTick < 0.01f)
        return 14;      // стоим: шаг не наберётся никаким чоком
    return std::clamp(static_cast<int>(distance / perTick), 1, 14);
}

[[nodiscard]] bool knifeQuiet() noexcept
{
    return config.exploits.knifeAirNoLag && knifeHeld && airborne && wantDoubleTap();
}

[[nodiscard]] int chokeLimit() noexcept
{
    if (fakeDuck)
        return processLimit > 0 ? std::min(fakeDuckChokeLimit, std::max(processLimit - 1, 1)) : fakeDuckChokeLimit;
    // Нож в прыжке с DT: без лагов — только чок под десинк
    if (knifeQuiet())
        return config.antiAim.enabled && config.antiAim.desync ? 1 : 0;
    // Лаг-пик главнее и фейклага, и дефенсива: пока выходим из-за угла, сервер должен получить
    // нас как можно позже. Без ограничений дефенсива «по шагу» и «4 тика, пока есть цель» —
    // как раз они и делали из пика пачки по 4 тика (60 мс), которых враг не замечает.
    // Предел — только сколько сервер обработает за раз (иначе хвост выкинет и потянет назад).
    // С DT тоже: заряд под это место уступает (clMoveHook урезает его по бюджету сервера)
    if (lagPeekChoke > 0) {
        int ceiling = processLimit > 0 ? std::clamp(processLimit - 1, 1, 14) : 14;
        // Исключение — зажатый автопик с DT: там заряд нужен для телепорта назад, а пачка лаг-пика
        // съедала его целиком (чок и заряд — один бюджет сервера). Логи 13.11: 7 возвратов из 54 шли
        // ногами с «заряд 1–3» именно после лаг-пика. На автопике пачка — только то, что не трогает заряд.
        if (holdShot && charged >= 6)
            ceiling = std::clamp((processLimit > 0 ? processLimit : 16) - charged, 1, ceiling);
        return std::clamp(lagPeekChoke, 1, ceiling);
    }
    // Дефенсив в режиме «чоком»: его длина пачки главнее фейклага билдера — пачка и есть окно.
    // Выше того, что сервер обрабатывает за раз, не лезем: хвост он выкинет, и окна не будет.
    if (defensiveChoke > 0 && !fastFire()) {
        // Сервер обрабатывает за свой тик не больше sv_maxusrcmdprocessticks команд, а хвост пачки
        // выкидывает: клиент эти команды предсказал, сервер их не применил, и приход снапшота тянет
        // игрока назад. Cvar серверный и на удалённом сервере обычно не реплицируется — тогда
        // держимся консервативно: пачка в 8 команд проходит и там, где лимит урезан до 8.
        const int ceiling = processLimit > 0 ? std::clamp(processLimit - 1, 1, 14) : 7;
        int limit = std::clamp(defensiveChoke, 1, ceiling);
        // Длинная пачка — это один большой шаг за один серверный тик. Порог телепорта у сервера
        // (m_flTeleportDistanceSqr) — 64 юнита, но держаться вплотную под ним нельзя: 62 юнита за
        // раз это почти корпус, и на выходе из-за угла ощущается как телепорт.
        //
        // Насколько это мешает, зависит от того, управляем ли мы собой. На земле шаг рвёт пик:
        // ты ведёшь персонажа мышью и клавишами, а он прыгает вперёд кусками. В воздухе траектория
        // задана прыжком, корректировать её нечем, и пика там не бывает — там можно позволить шаг
        // вчетверо больше, а значит и пачку длиннее. Это важно не только для комфорта: окно равно
        // длине пачки, и короткая пачка превращает спин в воздухе в редкие вспышки.
        limit = std::min(limit, ticksForDistance(airborne ? 40.0f : 16.0f));
        // Рейдж целится по тому, где враг сейчас, а стреляет последней командой пачки — то есть из
        // позиции, до которой мы ещё не дошли. На бегу по земле это прямые промахи, поэтому пока
        // цель есть, пачка короткая. Стоя расхождения нет вовсе (шага нет), в воздухе стрельба и
        // так идёт с упреждением — там режет только предел по шагу выше.
        if (!airborne && movingFast && ragebot::currentTarget() != 0)
            limit = std::min(limit, 4);
        return std::max(limit, 1);
    }
    if (!config.antiAim.enabled)
        return 0;

    const auto& aa = config.antiAim;
    // Десинк живёт на чокнутых тиках: без хотя бы одного чока его не существует
    const int floor = aa.desync ? 1 : 0;
    int limit = std::clamp(aa.fakeLag, floor, 14);
    // свои лимиты по состоянию: в воздухе чок почти не виден (позиция и так летит),
    // а стоя длинный чок — главный способ развести бектрек врага
    if (airborne && aa.fakeLagAir > 0)
        limit = std::clamp(aa.fakeLagAir, floor, 14);
    else if (!airborne && !movingFast && aa.fakeLagStanding > 0)
        limit = std::clamp(aa.fakeLagStanding, floor, 14);

    switch (static_cast<FakeLagMode>(std::clamp(aa.fakeLagMode, 0, static_cast<int>(FakeLagMode::Count) - 1))) {
    case FakeLagMode::Random:
        // новый лимит на каждый чок: ровный рисунок чока враги читают и подстраивают бектрек
        if (choked == 0 || randomChoke < floor || randomChoke > limit)
            randomChoke = floor + std::rand() % std::max(limit - floor + 1, 1);
        limit = randomChoke;
        break;
    case FakeLagMode::Adaptive:
        limit = std::clamp(std::min(limit, ticksForDistance(62.0f)), floor, 14);
        break;
    case FakeLagMode::Break: {
        const float perTick = horizontalSpeed * tickInterval;
        // Ползунок тут не ограничивает: режим выбран ради разрыва, а на земле (250 ед/с — около
        // 4 ед. за тик на 64 тиках) 64 юнита не набираются и за 14 тиков — разрыв получается
        // только на распрыжке и в полёте. Меню об этом пишет прямым текстом.
        limit = perTick > 0.01f ? std::clamp(static_cast<int>(std::ceil(65.0f / perTick)), floor, 14) : 14;
        break;
    }
    case FakeLagMode::Static:
    default:
        break;
    }

    // при DT серверу нужен запас под сдвиг — фейклаг только минимальный
    if (wantCharge())
        limit = std::min(limit, 1);
    // больше, чем сервер обработает за тик, — хвост выкинет
    if (processLimit > 0)
        limit = std::min(limit, std::max(processLimit - 1, floor));
    return limit;
}

void callOriginal(float extraSamples, bool finalTick) noexcept
{
    reinterpret_cast<ClMove>(trampoline)(extraSamples, finalTick);
    if (send)
        choked = 0;
    else
        ++choked;
}

void clMoveHook(float extraSamples, bool finalTick) noexcept
{
    if (!active) {
        charged = 0;
        choked = 0;
        shotPending = false;
        shiftRequested = false;
        setSendPacket(true);
        callOriginal(extraSamples, finalTick);
        return;
    }

    const int maxShift = shiftSize();
    // С автопиком (клавиша назначена, DT включён) копим больше — под телепорт назад: «Тиков сдвига» 6
    // дают ~27 ед., телепорта почти не видно. Выстрел DT тратит из этого только maxShift.
    const auto& peekKey = config.movement.autoPeekKey;
    const int chargeTarget = wantDoubleTap() && peekKey.isSet()
        ? std::max(maxShift, std::clamp(config.movement.peekTeleportTicks, 1, 14)) : maxShift;

    // заряд под дефенсив AA: AA просит ломать лагкомп или заряд как раз ушёл на рывок
    const bool lcRecharge = defensiveTarget > 0 || defensiveRecharge;

    if (!wantCharge()) {
        charged = 0;
    } else if (rechargeDelay > 0) {
        --rechargeDelay;
    } else if (chargeAllowed(chargeTarget, maxShift, lcRecharge)) {
        // зарядка: пропускаем тик, команда не создаётся — сервер потом примет пачку
        if (skipThisTick()) {
            ++charged;
            return;
        }
        // этот тик — обычная команда (мышь опрошена, ввод не потерян), следующие снова в заряд
    }

    // Выстрел (или тик сразу после него) в чоке не держим.
    //
    // Фейкдак раньше стоял тут полным исключением: `sendNext && !fakeDuck`. Рейдж просил отправить
    // пакет, чтобы выстрелить, а пакет ждал конца цикла — до 14 тиков, то есть больше 200 мс. За
    // это время враг уезжает, и стрельба с зажатым ФД превращалась в сплошные промахи.
    //
    // Цикл фейкдака устроен так: первую половину IN_DUCK не нажат, вторую — нажат (Movement.cpp).
    // На сервере мы в приседе как раз во второй половине, и отпустить пакет там не только можно,
    // но и нужно: именно эта поза — то, ради чего фейкдак и делается. Ждать дальше смысла нет.
    //
    // 09.11: так нельзя. Пакет, отпущенный в начале второй половины, несёт 1-2 тика приседа — сервер
    // видит нас почти стоя (враги — тоже), а Movement.cpp считал это «сервер выкинул присед» и урезал
    // чок навсегда. Теперь рейдж под ФД стреляет только на последнем тике стоя (Ragebot.cpp,
    // fakeDuckShotTick), выстрел едет в пачке до конца цикла — ~8 тиков, в окне лагкомпа, — а враги
    // на отправленном тике видят полный присед, как и должны.
    bool sendThis = (sendNext && !fakeDuck) || choked >= chokeLimit() || choked >= 14;
    const int packSize = choked + 1;    // сколько команд уйдёт этим пакетом, если он уйдёт
    // хайдшоты: тик выстрела держим в чоке, его углы уйдут на сервер вместе со следующей командой (там AA)
    if (forceChoke && !fakeDuck && choked < 13)
        sendThis = false;
    forceChoke = false;
    setSendPacket(sendThis);
    if (charged >= maxShift)
        defensiveRecharge = false;
    sendNext = false;
    callOriginal(extraSamples, finalTick);

    // Лаг-пик с включённым DT. Заряд и чок берут из ОДНОГО бюджета сервера: он обрабатывает за
    // тик не больше sv_maxusrcmdprocessticks (16) команд, пропущенные и чокнутые тики одинаково
    // копят этот запас до потолка. Полный заряд 14 + чок 14 = 28 — сервер выкидывал хвост: сдвиг
    // после пачки лаг-пика (телепорт автопика, вторая пуля, рывок) терялся и тянуло назад, а
    // рывок дефенсива досрочно сливал саму пачку — «сервер держит нас на 1 тик позади».
    // Пока лаг-пик держит пачку, заряд — только то, что останется после неё.
    // То же для чока дефенсива в воздухе (там он идёт постоянно, а DT свой заряд не отдаёт)
    if (lagPeekChoke > 0 || (defensiveChoke > 0 && airborne)) {
        const int budget = processLimit > 0 ? processLimit : 16;
        const int pack = sendThis ? packSize : choked + 1;   // ушла или минимум уйдёт
        charged = std::min(charged, std::max(budget - pack, 0));
    }
    // Пачка чока ушла — это и есть «рывок» дефенсива в режиме «чоком»: сервер обработает все её
    // команды за один свой тик и запишет лагкомп только по последней
    // Пачка от двух команд уже даёт окно: сервер пишет лагкомп только по последней. Порог в 3
    // отрезал короткие пачки, а при sv_maxusrcmdprocessticks 2-3 (дефолт CS:GO — 2) длиннее и не
    // бывает: chokeLimit() держится под лимитом, иначе сервер выкинет хвост. Окна не было вовсе.
    if (sendThis && defensiveChoke > 0 && lagPeekChoke == 0 && !fastFire() && packSize >= 2) {
        shiftPackDone = packSize;
        lastPackChoke = true;
    }

    // Сдвиг по запросу (телепорт автопика, рывок) при полном чоке ждёт тик: пачка ушла бы как
    // «чок + сдвиг», 14 + 14 команд, а сервер обрабатывает за раз не больше sv_maxusrcmdprocessticks
    // и хвост выкидывает — телепорт автопика после выстрела с лаг-пика тянуло назад. Следующим тиком
    // пачка с выстрелом уходит (sendNext), чок обнуляется, и сдвиг идёт отдельной пачкой.
    //
    // 13.11: и при выстреле сдвиг по запросу ждёт, пока пачка с выстрелом уйдёт. Сервер (AdjustPlayerTimeBase)
    // на пачку длиннее 3 команд откатывает тикбейс на её длину — выстрел, за которым в той же пачке идёт
    // телепорт на 14 тиков, выполняется «0.2 с назад». Лагкомп считает наш tick_count слишком свежим для такого
    // времени (|target − curtime + correct| > 0.2, server_client.so) и откатывает врага по пингу — туда, где
    // он был за ~0.2 с до записи, по которой мы стреляли. Запрос не теряется: уходит следующим тиком.
    if (shotPending && !sendThis)
        unsentShot = true;
    const bool shotInPack = shotPending || unsentShot;
    if (sendThis)
        unsentShot = false;
    const bool shiftWaitsForShot = shiftRequested && !(shotPending && fastFire()) && shotInPack && charged > 0;
    const bool shiftWaitsForPack = shiftWaitsForShot || (shiftRequested && !(shotPending && fastFire())
        && choked > 0 && choked + std::min(requestedTicks > 0 ? requestedTicks : charged, charged) > 14);
    if (shiftWaitsForPack)
        sendNext = true;
    // DT сдвигает сразу на выстреле — второй выстрел не ждёт (с автопиком возврат идёт в этих же командах)
    if (((shotPending && fastFire()) || shiftRequested) && charged > 0 && !shiftWaitsForPack) {
        // сдвиг: прогоняем заряженные тики подряд, уходят одним пакетом с последним. Всегда весь заряд:
        // на 2 тиках сервер не откатывал тикбейс — окна дефенсива не было (проверено в игре 01.11).
        const bool small = false;
        // выстрел DT — только его тики (остаток копили под телепорт автопика); рывок по запросу — весь заряд
        int left = shiftRequested ? (requestedTicks > 0 ? std::min(requestedTicks, charged) : charged) : std::min(charged, maxShift);
        const int shiftedCount = left;
        shifting = true;
        while (left > 0) {
            --left;
            --charged;
            // сколько команд пачки ещё впереди: по этому числу дефенсив знает, сколько тиков
            // окна гарантированы без всякого отката (сервер пишет лагкомп по последней команде пачки)
            shiftRemaining = left;
            setSendPacket(left == 0);
            callOriginal(extraSamples, left == 0 && finalTick);
        }
        shifting = false;
        shiftRemaining = 0;
        // Пачка прошла — отдаём её длину дефенсиву: окно он откроет по факту, а не по догадке о том,
        // сколько заряда будет на момент сдвига. Чей сдвиг — не важно: откат от второй пули DT и от
        // телепорта автопика такой же настоящий, как от нашего рывка, и окна в нём такие же.
        shiftPackDone = shiftedCount;
        lastPackChoke = false;
        if (shiftRequested && !lcShift) {
            char text[96];
            std::snprintf(text, sizeof(text), "[тикбейс] сдвиг по запросу: %d тиков%s", shiftedCount, shotPending ? " (с выстрелом)" : "");
            ragebot::logToFile(text);
        }
        // После выстрела DT ждём полный цикл. После дефенсива заряжаемся сразу (на земле): докачать 2 тика.
        rechargeDelay = shotPending && fastFire() ? 16 : (groundTicks >= 3 || small ? 0 : 6);
        defensiveRecharge = !(shotPending && fastFire()) && wantDoubleTap();
        // Окно ведёт Defensive.cpp: он знает, что рывок его, и меряет настоящий откат по rawDeficit()
        (void)shiftedCount;
    } else if (shotPending && rechargeDelay > 2) {
        // Второй выстрел DT уже ушёл (сдвиг был недавно, заряд пуст) — ждать остаток 16 тиков незачем:
        // следующий выстрел всё равно через перезарядку оружия. Заряд снова готов через ~16 тиков, а не ~30.
        rechargeDelay = 2;
    }
    shotPending = false;
    // Сдвиг ждал, пока уйдёт пачка (с выстрелом или просто полный чок), — запрос остаётся на следующий тик.
    // Раньше при ожидании из-за чока запрос сбрасывался сразу: автопик просит телепорт один раз, и тот
    // просто не случался (логи 11.11: 19 из 94 телепортов без сдвига, 13.11: 6 из 47) — возврат шёл ногами
    // под огнём. Телепорту ждём до 4 тиков, рывку дефенсива — до 2 (позже его окно уже не к месту).
    if (shiftWaitsForPack && shiftRequested && ++shiftDeferred <= (lcShift ? 2 : 4))
        return;
    shiftDeferred = 0;
    shiftRequested = false;
    requestedTicks = 0;
    lcShift = false;
}

}

bool install(std::span<const std::byte> engineCode) noexcept
{
    if (clMove)
        return true;

    const auto function = find(engineCode, clMovePrologue, sizeof(clMovePrologue));
    if (!function) {
        std::fputs("[newerwin] CL_Move не найден — DT и десинк выключены\n", stderr);
        return false;
    }

    // ищем `mov r13d, 1; jmp` в теле CL_Move (в текущем билде на +0x298)
    std::uint8_t* imm = nullptr;
    for (std::size_t offset = 0; offset < 0x600; ++offset) {
        if (std::memcmp(function + offset, sendPacketDefault, sizeof(sendPacketDefault)) == 0) {
            imm = function + offset + 2;
            break;
        }
    }
    if (!imm) {
        std::fputs("[newerwin] bSendPacket не найден — DT и десинк выключены\n", stderr);
        return false;
    }

    auto tramp = static_cast<std::uint8_t*>(mmap(nullptr, 64, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    if (tramp == MAP_FAILED)
        return false;

    if (!makeWritable(function, stolenBytes) || !makeWritable(imm, 1)) {
        munmap(tramp, 64);
        return false;
    }

    std::memcpy(originalBytes, function, stolenBytes);
    std::memcpy(tramp, function, stolenBytes);
    writeAbsoluteJump(tramp + stolenBytes, function + stolenBytes);

    trampoline = tramp;
    sendPacketImm = imm;
    clMove = function;

    std::uint8_t patch[stolenBytes];
    writeAbsoluteJump(patch, reinterpret_cast<const void*>(&clMoveHook));
    patch[14] = 0x90;
    patch[15] = 0x90;
    std::memcpy(function, patch, stolenBytes);
    __builtin___clear_cache(reinterpret_cast<char*>(function), reinterpret_cast<char*>(function + stolenBytes));
    return true;
}

namespace
{
void unhookChannel() noexcept;
}

void uninstall() noexcept
{
    unhookChannel();
    if (!clMove)
        return;
    std::memcpy(clMove, originalBytes, stolenBytes);
    *sendPacketImm = 1;
    __builtin___clear_cache(reinterpret_cast<char*>(clMove), reinterpret_cast<char*>(clMove + stolenBytes));
    clMove = nullptr;
    sendPacketImm = nullptr;
    // трамплин не освобождаем: CL_Move может прямо сейчас быть на стеке
}

// ---------- latency spoof ----------
// Заголовок каждого нашего пакета несёт номер последнего полученного от сервера пакета (m_nInSequenceNr) и его
// reliable-состояние. Сервер считает наш пинг по тому, как быстро мы подтверждаем его пакеты. Подставляем
// в SendDatagram номер, полученный fakeLatencyMs назад, — сервер видит пинг больше и откатывает лагкомп дальше.
// CNetChan на Linux (engine_client.so, legacy, сверено по SendDatagram 0x4aa000 — запись заголовка):
//   +0x2c m_nOutSequenceNr, +0x30 m_nInSequenceNr, +0x3c m_nInReliableState, +0x40 m_nChokedPackets.
// SendDatagram — слот 47 таблицы CNetChan (таблица 0xd68978, слот 0xd68af0). Хук — своя копия таблицы на
// объекте: netchan пересоздаётся на каждом подключении, ставим заново, оригинал таблицы не трогаем.
namespace
{
constexpr std::size_t sendDatagramIndex = 47;
constexpr std::uintptr_t sendDatagramOffset = 0x4aa000;   // от начала engine_client.so — проверка, что сборка та же
constexpr std::ptrdiff_t inSequenceOffset = 0x30;
constexpr std::ptrdiff_t inReliableOffset = 0x3c;

using SendDatagramFn = int (*)(void* netchan, void* datagram);
SendDatagramFn originalSendDatagram = nullptr;
void* hookedChannel = nullptr;
std::uintptr_t* originalVmt = nullptr;
std::array<std::uintptr_t, 2 + 256> vmtCopy{};    // [-2] offset-to-top, [-1] typeinfo, дальше слоты
bool spoofBroken = false;                         // таблица не та — спуф выключен до перезапуска
float appliedSpoof = 0.0f;                        // какую прибавку сервер видит сейчас, с

struct SequenceMark {
    int inSequence = 0;
    int inReliable = 0;
    double time = 0.0;
};
std::array<SequenceMark, 256> marks{};
int markHead = 0, markCount = 0;

[[nodiscard]] double clockSeconds() noexcept
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

[[nodiscard]] int& field(void* channel, std::ptrdiff_t offset) noexcept
{
    return *reinterpret_cast<int*>(static_cast<char*>(channel) + offset);
}

int hookedSendDatagram(void* channel, void* datagram) noexcept
{
    const auto& ex = config.exploits;
    if (channel != hookedChannel || !ex.fakeLatency || ex.fakeLatencyMs <= 0 || markCount == 0) {
        if (channel == hookedChannel)
            appliedSpoof = 0.0f;
        return originalSendDatagram(channel, datagram);
    }
    // самая свежая отметка, которой уже не меньше fakeLatencyMs
    const double wanted = ex.fakeLatencyMs * 0.001;
    const double now = clockSeconds();
    const SequenceMark* pick = nullptr;
    for (int k = 0; k < markCount; ++k) {
        const auto& mark = marks[(markHead - 1 - k + static_cast<int>(marks.size())) % static_cast<int>(marks.size())];
        if (now - mark.time >= wanted) {
            // свежих отметок давно нет (обновления встали) — не держим подтверждение на древнем номере
            if (now - mark.time <= wanted + 0.1)
                pick = &mark;
            break;
        }
    }
    if (!pick) {
        appliedSpoof = 0.0f;
        return originalSendDatagram(channel, datagram);
    }
    int& inSequence = field(channel, inSequenceOffset);
    int& inReliable = field(channel, inReliableOffset);
    const int savedSequence = inSequence, savedReliable = inReliable;
    // назад во времени — только к меньшему номеру (на всякий случай: история могла остаться от прошлого сервера)
    if (pick->inSequence <= savedSequence && savedSequence - pick->inSequence < 256) {
        inSequence = pick->inSequence;
        inReliable = pick->inReliable;
        appliedSpoof = static_cast<float>(now - pick->time);
    }
    const int result = originalSendDatagram(channel, datagram);
    inSequence = savedSequence;
    inReliable = savedReliable;
    return result;
}

void unhookChannel() noexcept
{
    if (hookedChannel && originalVmt && *reinterpret_cast<std::uintptr_t**>(hookedChannel) == vmtCopy.data() + 2)
        *reinterpret_cast<std::uintptr_t**>(hookedChannel) = originalVmt;
    hookedChannel = nullptr;
    originalVmt = nullptr;
    appliedSpoof = 0.0f;
    markCount = markHead = 0;
}

[[nodiscard]] bool hookChannel(void* channel) noexcept
{
    auto* vmt = *reinterpret_cast<std::uintptr_t**>(channel);
    Dl_info info{};
    if (!dladdr(reinterpret_cast<void*>(vmt[sendDatagramIndex]), &info) || !info.dli_fname
        || !std::strstr(info.dli_fname, "engine_client")
        || vmt[sendDatagramIndex] - reinterpret_cast<std::uintptr_t>(info.dli_fbase) != sendDatagramOffset) {
        ragebot::logToFile("[фейк-пинг] таблица CNetChan не та (игра обновилась?) — latency spoof выключен");
        spoofBroken = true;
        return false;
    }
    // длина таблицы — пока слоты указывают в тот же модуль
    std::size_t length = 0;
    while (length < vmtCopy.size() - 2) {
        Dl_info slot{};
        if (!dladdr(reinterpret_cast<void*>(vmt[length]), &slot) || slot.dli_fbase != info.dli_fbase)
            break;
        ++length;
    }
    if (length <= sendDatagramIndex) {
        spoofBroken = true;
        return false;
    }
    vmtCopy.fill(0);
    std::memcpy(vmtCopy.data(), vmt - 2, (length + 2) * sizeof(std::uintptr_t));
    originalSendDatagram = reinterpret_cast<SendDatagramFn>(vmt[sendDatagramIndex]);
    vmtCopy[2 + sendDatagramIndex] = reinterpret_cast<std::uintptr_t>(&hookedSendDatagram);
    originalVmt = vmt;
    hookedChannel = channel;
    *reinterpret_cast<std::uintptr_t**>(channel) = vmtCopy.data() + 2;
    markCount = markHead = 0;
    char text[96];
    std::snprintf(text, sizeof(text), "[фейк-пинг] хук SendDatagram поставлен (слотов %zu)", length);
    ragebot::logToFile(text);
    return true;
}
}

void updateFakeLatency(void* networkChannel, bool loopback) noexcept
{
    // хук держим, пока включено и есть netchan; иначе снимаем (netchan мог смениться — reconnect)
    const bool want = config.exploits.fakeLatency && networkChannel && !loopback && !spoofBroken;
    if (!want || networkChannel != hookedChannel) {
        // старый netchan мог быть уже удалён движком — трогаем вптр, только если это всё ещё он
        if (hookedChannel && hookedChannel == networkChannel)
            unhookChannel();
        else {
            hookedChannel = nullptr;
            originalVmt = nullptr;
            appliedSpoof = 0.0f;
            markCount = markHead = 0;
        }
        if (!want || !hookChannel(networkChannel))
            return;
    }
    // история входящих номеров: каждый новый — с моментом получения (точнее, первого тика, когда увидели)
    const int inSequence = field(networkChannel, inSequenceOffset);
    const int inReliable = field(networkChannel, inReliableOffset);
    const auto& last = marks[(markHead - 1 + static_cast<int>(marks.size())) % static_cast<int>(marks.size())];
    if (markCount == 0 || inSequence > last.inSequence || inSequence < last.inSequence - 1000) {
        if (markCount > 0 && inSequence < last.inSequence)
            markCount = 0;      // номера пошли заново (новый сервер)
        marks[markHead] = { inSequence, inReliable, clockSeconds() };
        markHead = (markHead + 1) % static_cast<int>(marks.size());
        markCount = std::min(markCount + 1, static_cast<int>(marks.size()));
    }
}

float fakeLatencySeconds() noexcept
{
    return hookedChannel && config.exploits.fakeLatency ? appliedSpoof : 0.0f;
}

bool isInstalled() noexcept
{
    return clMove != nullptr;
}

bool sendPacket() noexcept
{
    return send;
}

void setFakeDuck(bool value) noexcept
{
    fakeDuck = value;
}

int chokedCommands() noexcept
{
    return choked;
}

int shotRollbackTicks() noexcept
{
    // быстрый DT: сдвиг уходит сразу за выстрелом, той же пачкой; пачку до 3 команд сервер не откатывает
    if (!clMove || shifting || !fastFire() || charged <= 0)
        return 0;
    const int shift = std::min(charged, shiftSize());
    return choked + 1 + shift > 3 ? shift : 0;
}

void onShot() noexcept
{
    shotPending = true;
    // если тик выстрела уже был в чоке — отправляем его со следующим, не ждём конца фейклага
    sendNext = true;
}

void sendNextPacket() noexcept
{
    sendNext = true;
}

void requestShift(int ticks, bool byDefensive) noexcept
{
    if (shifting)
        return;
    // два запроса за тик — берём больший («весь заряд» главнее)
    if (!shiftRequested)
        requestedTicks = ticks;
    else if (requestedTicks > 0)
        requestedTicks = ticks > 0 ? std::max(requestedTicks, ticks) : 0;
    shiftRequested = true;
    if (byDefensive)
        lcShift = true;
}

int shiftLeft() noexcept
{
    return shifting ? shiftRemaining : 0;
}

int takeShiftPack() noexcept
{
    const int value = shiftPackDone;
    shiftPackDone = 0;
    return value;
}

bool hideShotsActive() noexcept
{
    return hideShotsOn();
}

bool autoHideShotsEngaged() noexcept
{
    return autoHs && wantDoubleTap();
}

void setAutoHideShots(bool value) noexcept
{
    autoHs = value;
}

void chokeNextPacket() noexcept
{
    forceChoke = true;
}

bool isShifting() noexcept
{
    return shifting;
}

int chargedTicks() noexcept
{
    return charged;
}

bool doubleTapEnabled() noexcept
{
    return wantDoubleTap();
}

bool isCharged() noexcept
{
    return charged >= shiftSize();
}

bool threatened() noexcept
{
    return threat;
}

bool isDefensiveRecharging() noexcept
{
    return defensiveRecharge && wantDoubleTap();
}

int fakeDuckChoke() noexcept
{
    return fakeDuckChokeLimit;
}

void setFakeDuckChoke(int value) noexcept
{
    fakeDuckChokeLimit = std::clamp(value, 4, 14);
}

void holdShotShift(bool hold) noexcept
{
    holdShot = hold;
}

void setActive(bool value) noexcept
{
    active = value;
    if (!value) {
        maxTickBase = 0;
        deficit = 0;
        defensiveTarget = 0;
        defensiveChoke = 0;
        shiftPackDone = 0;
    }
}

void updateLagComp(int tickBase) noexcept
{
    // большой скачок — респаун/смена карты/реконнект, а не дефенсив
    if (std::abs(tickBase - maxTickBase) > 64)
        maxTickBase = 0;
    maxTickBase = std::max(maxTickBase, tickBase);
    // тики внутри рывка — не отдельные тики времени
    if (!shifting)
        deficit = std::clamp(maxTickBase - tickBase - 1, 0, 14);
}

int rawDeficit() noexcept
{
    return deficit;
}

void setDefensiveCharge(int target, int spread) noexcept
{
    defensiveTarget = std::clamp(target, 0, 14);
    defensiveSpread = std::clamp(spread, 1, 6);
}

void setDefensiveChoke(int ticks) noexcept
{
    defensiveChoke = std::clamp(ticks, 0, 14);
}

void setLagPeekChoke(int ticks) noexcept
{
    lagPeekChoke = std::clamp(ticks, 0, 14);
}

int lagPeekChokeTicks() noexcept
{
    return lagPeekChoke > 0 && !fakeDuck ? chokeLimit() : 0;
}

bool lastPackWasChoke() noexcept
{
    return lastPackChoke;
}

int defensiveChokeTicks() noexcept
{
    return defensiveChoke;
}

void setProcessLimit(int ticks) noexcept
{
    cvarProcessLimit = ticks > 0 ? std::clamp(ticks, 2, 32) : 0;
    // cvar у клиента верен только на своём сервере. На удалённом клиент видит своё 16, а сервер
    // режет раньше — берём меньшее из него и настройки «Команд за тик»
    const int configured = config.exploits.serverPackLimit > 0 ? std::clamp(config.exploits.serverPackLimit, 2, 16) : 0;
    if (serverLocal || configured == 0)
        processLimit = cvarProcessLimit;
    else
        processLimit = cvarProcessLimit > 0 ? std::min(cvarProcessLimit, configured) : configured;
}

int processLimitTicks() noexcept
{
    return processLimit;
}

void setMoveInput(bool held) noexcept
{
    moveInput = held;
}

void setMovement(bool onGround, bool jumpHeld, float verticalSpeed, float speed2D, float interval) noexcept
{
    horizontalSpeed = speed2D;
    if (interval > 0.0f)
        tickInterval = interval;
    movingFast = speed2D > 20.0f;
    groundTicks = onGround && !jumpHeld ? std::min(groundTicks + 1, 64) : 0;
    // «далеко от земли»: взлёт и первая половина спуска (у прыжка вверх ~300 ед/с, к приземлению −300).
    // Тут пропуск тика на зарядку прыжок не съест — LC в воздухе дозаряжается и работает весь прыжок.
    rising = !onGround && verticalSpeed > -150.0f;
    airborne = !onGround;
}

bool defensiveAllowed() noexcept
{
    // Дефенсив живёт на своём чоке — галка DT ему не нужна. Быстрая стрельба тут больше не запрет:
    // свои рывки при ней действительно невозможны (заряд принадлежит второй пуле, а пачка
    // «чок + сдвиг» была бы длиннее, чем сервер обрабатывает за раз), но окна от её сдвигов —
    // настоящие, и дефенсив их ловит и показывает в них скрытые углы. Раньше тут стояло !fastFire(),
    // и у всех, кто играет со второй пулей, дефенсив был выключен целиком и показывал 0%.
    return active && !fakeDuck && isInstalled();
}

bool fastFireEnabled() noexcept
{
    return fastFire();
}

bool defensiveDoubleTap() noexcept
{
    return wantDoubleTap() && !fastFire();
}

void setSlowWeapon(bool value) noexcept
{
    slowWeapon = value;
}

void setKnife(bool knife) noexcept
{
    knifeHeld = knife;
}

void setServerLocal(bool local) noexcept
{
    serverLocal = local;
}

bool knifeAirQuiet() noexcept
{
    return active && knifeQuiet();
}

void setThreat(bool value) noexcept
{
    threat = value;
}

}

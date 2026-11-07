#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>

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
int maxTickBase = 0;
int deficit = 0;                // сырой откат тикбейса: максимум − текущий − 1
int defensiveTarget = 0;        // цель заряда под дефенсив ПРОПУСКОМ тиков (0 — не нужен)
// Цель заряда под дефенсив ЧОКОМ (0 — не нужен). Разница принципиальная: при чоке команда
// создаётся — движение предсказывается, мышь опрашивается, — и на сервер она просто уходит позже.
// Серверная модель отстаёт от нашей на длину чока, а мы едем ровно. Пропуск же вызова CL_Move
// команду не создаёт вовсе: на этом тике персонаж никуда не идёт (при «1 из 3» это треть скорости),
// а потом пачка прогоняется за один тик, предсказание улетает вперёд и коррекция тянет назад.
int defensiveChoke = 0;
int processLimit = 0;           // sv_maxusrcmdprocessticks, если сервер его реплицирует (0 — неизвестно)
int defensiveSpread = 3;        // пропускать 1 тик из N при зарядке под дефенсив
int groundTicks = 0;            // сколько тиков подряд стоим на земле без прыжка
bool rising = false;            // летим вверх — до приземления далеко, пропуск тика не съест прыжок
bool airborne = false;
bool movingFast = false;        // бежим/идём — перезарядку дефенсива растягиваем
float horizontalSpeed = 0.0f;   // своя скорость по горизонтали, юнитов/с
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
    if (fastFire())
        return std::clamp(config.exploits.shiftTicks, 1, 14);
    return defensiveTarget > 0 ? std::clamp(defensiveTarget, 4, 14) : std::clamp(config.exploits.shiftTicks, 1, 14);
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
    if (horizontalSpeed < 5.0f)
        return true;
    // В движении пропущенный тик — это потерянный шаг. Подряд они дают ровно то самое «иду вперёд,
    // останавливаюсь, и чуть-чуть назад»: персонаж стоит всю зарядку, а потом предсказание догоняет
    // сервер рывком. Размазываем: 1 тик из N. Для второй пули DT берём не меньше 3 — заряд
    // наберётся втрое дольше, зато бежать при этом можно.
    int every = std::clamp(defensiveSpread, 1, 6);
    if (fastFire())
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
    // вторая пуля DT: без заряда её не будет
    if (fastFire())
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
    // Заряжаемся на земле (не сразу после приземления) и на взлёте. На спуске и на тике приземления —
    // нет: пропущенный тик там съедает прыжок распрыжки. На взлёте теряется лишь чуть стрейфа, зато
    // дефенсив работает и при распрыжке.
    if (groundTicks < 3 && !rising)
        return false;
    // Под прицелом обычно не заряжаемся: пропуск тика виден врагу как замирание. Исключение —
    // дефенсив: его окна и есть сдвиги, а сдвигать нечего, если под прицелом заряд не набирается
    // (раньше так и было: один рывок — и пусто, пока враг нас видит). Пропуски размазаны (skipThisTick).
    if (threat && !lcRecharge && (!fastFire() || charged >= maxShift))
        return false;
    // Автопик выглядывает: заряд = пропуск команд, персонаж бы замер прямо под прицелом
    if (holdShot && threat)
        return false;
    // На ходу пропущенный тик — это потерянный шаг, и так весь бег. Поэтому на ходу копим, только
    // если заряд нужен прямо сейчас: есть в кого стрелять второй пулей, нажата клавиша, которой
    // заряд тратят, или дефенсив копит под своё окно. Стоя копим всегда — там пропуск не виден.
    // Раньше хватало НАЗНАЧЕННОЙ клавиши телепорта или автопика, чтобы заряд копился постоянно,
    // и бег по пустой карте с включённым DT шёл с потерей каждого третьего тика.
    if (horizontalSpeed > 5.0f) {
        const auto& ex = config.exploits;
        const bool needNow = defensiveTarget > 0
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

[[nodiscard]] int chokeLimit() noexcept
{
    if (fakeDuck)
        return fakeDuckChokeLimit;
    // Дефенсив в режиме «чоком»: его длина пачки главнее фейклага билдера — пачка и есть окно.
    // Выше того, что сервер обрабатывает за раз, не лезем: хвост он выкинет, и окна не будет.
    if (defensiveChoke > 0 && !fastFire()) {
        // Сервер обрабатывает за свой тик не больше sv_maxusrcmdprocessticks команд, а хвост пачки
        // выкидывает: клиент эти команды предсказал, сервер их не применил, и приход снапшота тянет
        // игрока назад. Cvar серверный и на удалённом сервере обычно не реплицируется — тогда
        // держимся консервативно: пачка в 8 команд проходит и там, где лимит урезан до 8.
        const int ceiling = processLimit > 0 ? std::clamp(processLimit - 1, 1, 14) : 7;
        int limit = std::clamp(defensiveChoke, 1, ceiling);
        // В движении длинная пачка — это один большой шаг за один серверный тик. Больше 64 юнитов
        // сервер считает телепортом (m_flTeleportDistanceSqr) и запись лагкомпа по нам уже не
        // интерполирует, а своя модель на экране расходится с серверной тем сильнее, чем длиннее
        // чок. Держимся под порогом: на распрыжке и в полёте чок сам становится короче.
        limit = std::min(limit, ticksForDistance(62.0f));
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

    // выстрел (или тик сразу после него) в чоке не держим; фейкдаку чок нужен целиком
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
    // Пачка чока ушла — это и есть «рывок» дефенсива в режиме «чоком»: сервер обработает все её
    // команды за один свой тик и запишет лагкомп только по последней
    if (sendThis && defensiveChoke > 0 && !fastFire() && packSize >= 3)
        shiftPackDone = packSize;

    // DT сдвигает сразу на выстреле — второй выстрел не ждёт (с автопиком возврат идёт в этих же командах)
    if (((shotPending && fastFire()) || shiftRequested) && charged > 0) {
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

void uninstall() noexcept
{
    if (!clMove)
        return;
    std::memcpy(clMove, originalBytes, stolenBytes);
    *sendPacketImm = 1;
    __builtin___clear_cache(reinterpret_cast<char*>(clMove), reinterpret_cast<char*>(clMove + stolenBytes));
    clMove = nullptr;
    sendPacketImm = nullptr;
    // трамплин не освобождаем: CL_Move может прямо сейчас быть на стеке
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

int defensiveChokeTicks() noexcept
{
    return defensiveChoke;
}

void setProcessLimit(int ticks) noexcept
{
    processLimit = ticks > 0 ? std::clamp(ticks, 2, 32) : 0;
}

int processLimitTicks() noexcept
{
    return processLimit;
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

void setThreat(bool value) noexcept
{
    threat = value;
}

}

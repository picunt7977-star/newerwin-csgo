#include <algorithm>
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
int lagCompLeft = 0;
int shiftWindow = 0;            // тиков после нашего сдвига, пока сервер ещё не прислал откат тикбейса
int breakLcFrames = 0;          // AA просил ломать лагкомп недавно — заряд нужен
int groundTicks = 0;            // сколько тиков подряд стоим на земле без прыжка
bool rising = false;            // летим вверх — до приземления далеко, пропуск тика не съест прыжок
bool airborne = false;
bool movingFast = false;        // бежим/идём — перезарядку дефенсива растягиваем
bool chargeParity = false;      // плавная зарядка: пропускаем через тик
// Чок фейкдака. Сервер обрабатывает за раз не больше sv_maxusrcmdprocessticks команд (по умолчанию 16,
// на части HvH-серверов 8): лишние команды пачки он выкидывает — а это как раз тики приседа в её конце,
// и враг видит нас стоящими через раз. Movement снижает чок, если на отправке оказались не в приседе.
int fakeDuckChokeLimit = 14;
bool threat = false;             // нас сейчас видит враг
bool slowWeapon = false;         // в руках скаут/AWP и прочее с перезарядкой > 0.4 с — второй пули DT нет
bool lcShift = false;            // сдвиг просили LC-триггеры (дефенсив без выстрела)
bool defensiveRecharge = false; // заряд потрачен дефенсивом и сейчас набирается заново — DT при этом «включён»

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
    return wantDoubleTap() && config.exploits.doubleTapMode == 0 && !slowWeapon;
}

// Плавная зарядка: заряд под дефенсив (не под вторую пулю DT) в движении и в воздухе копится через тик.
// Иначе 13 пропущенных тиков подряд = замер на ~200 мс (в воздухе — зависание), а потом сдвиг рывком
// догоняет — со стороны и у себя на экране это «кидает туда-сюда». Через тик видно лишь лёгкое замедление.
[[nodiscard]] bool smoothCharge() noexcept
{
    return !fastFire() && (airborne || movingFast);
}

[[nodiscard]] bool wantCharge() noexcept
{
    const auto& ex = config.exploits;
    return wantDoubleTap() || (active && !fakeDuck && (ex.teleportKey.isSet() || ex.defensive || breakLcFrames > 0));
}

[[nodiscard]] int chokeLimit() noexcept
{
    if (fakeDuck)
        return fakeDuckChokeLimit;
    int limit = 0;
    if (config.antiAim.enabled)
        limit = std::clamp(config.antiAim.fakeLag, config.antiAim.desync ? 1 : 0, 14);
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

    const int maxShift = std::clamp(config.exploits.shiftTicks, 1, 14);
    // С автопиком (клавиша назначена, DT включён) копим больше — под телепорт назад: «Тиков сдвига» 6
    // дают ~27 ед., телепорта почти не видно. Выстрел DT тратит из этого только maxShift.
    const auto& peekKey = config.movement.autoPeekKey;
    const int chargeTarget = wantDoubleTap() && peekKey.isSet()
        ? std::max(maxShift, std::clamp(config.movement.peekTeleportTicks, 1, 14)) : maxShift;

    if (!wantCharge()) {
        charged = 0;
    } else if (rechargeDelay > 0) {
        --rechargeDelay;
    } else if (charged < chargeTarget && !shotPending && !shiftRequested && (groundTicks >= 3 || rising)
        && !(threat && (!fastFire() || charged >= maxShift)) && !(holdShot && threat)) {
        // Дефенсив DT не дозаряжается, пока нас видит враг: зарядка — это пропуск тиков, и посреди
        // перестрелки после рывка стояли 6 тиков без выстрела («фоткает»). Зарядимся за укрытием.
        // Заряжаемся на земле (не сразу после приземления) и на взлёте. На спуске и на тике приземления —
        // нет: пропущенный тик там съедает прыжок распрыжки. На взлёте теряется лишь чуть стрейфа, зато
        // дефенсив в воздухе работает и при распрыжке (иначе заряд не набирался бы вовсе).
        // зарядка: пропускаем тик, команда не создаётся — сервер потом примет пачку
        chargeParity = !chargeParity;
        if (!smoothCharge() || chargeParity) {
            ++charged;
            return;
        }
        // плавная зарядка: этот тик — обычная команда, следующий снова в заряд
    }

    // выстрел (или тик сразу после него) в чоке не держим; фейкдаку чок нужен целиком
    setSendPacket((sendNext && !fakeDuck) || choked >= chokeLimit() || choked >= 14);
    if (charged >= maxShift)
        defensiveRecharge = false;
    sendNext = false;
    callOriginal(extraSamples, finalTick);

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
            setSendPacket(left == 0);
            callOriginal(extraSamples, left == 0 && finalTick);
        }
        shifting = false;
        if (shiftRequested && !lcShift) {
            char text[96];
            std::snprintf(text, sizeof(text), "[тикбейс] сдвиг по запросу: %d тиков%s", shiftedCount, shotPending ? " (с выстрелом)" : "");
            ragebot::logToFile(text);
        }
        // После выстрела DT ждём полный цикл. После дефенсива заряжаемся сразу (на земле): докачать 2 тика.
        rechargeDelay = shotPending && fastFire() ? 16 : (groundTicks >= 3 || small ? 0 : 6);
        defensiveRecharge = !(shotPending && fastFire()) && wantDoubleTap();
        shiftWindow = small ? 2 : 3;
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

void requestShift(int ticks) noexcept
{
    if (shifting)
        return;
    // два запроса за тик — берём больший («весь заряд» главнее)
    if (!shiftRequested)
        requestedTicks = ticks;
    else if (requestedTicks > 0)
        requestedTicks = ticks > 0 ? std::max(requestedTicks, ticks) : 0;
    shiftRequested = true;
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
    return charged >= std::clamp(config.exploits.shiftTicks, 1, 14);
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
        lagCompLeft = 0;
        shiftWindow = 0;
        breakLcFrames = 0;
    }
}

void updateLagComp(int tickBase) noexcept
{
    // большой скачок — респаун/смена карты/реконнект, а не дефенсив
    if (std::abs(tickBase - maxTickBase) > 64)
        maxTickBase = 0;
    maxTickBase = std::max(maxTickBase, tickBase);
    lagCompLeft = std::clamp(maxTickBase - tickBase - 1, 0, 14);
    if (shiftWindow > 0)
        --shiftWindow;
    if (breakLcFrames > 0)
        --breakLcFrames;
}

int defensiveTicksLeft() noexcept
{
    return std::max(lagCompLeft, shiftWindow);
}

void setMovement(bool onGround, bool jumpHeld, float verticalSpeed, float horizontalSpeed) noexcept
{
    movingFast = horizontalSpeed > 20.0f;
    groundTicks = onGround && !jumpHeld ? std::min(groundTicks + 1, 64) : 0;
    // «далеко от земли»: взлёт и первая половина спуска (у прыжка вверх ~300 ед/с, к приземлению −300).
    // Тут пропуск тика на зарядку прыжок не съест — LC в воздухе дозаряжается и работает весь прыжок.
    rising = !onGround && verticalSpeed > -150.0f;
    airborne = !onGround;
}

bool defensiveAllowed() noexcept
{
    // с DT рывков дефенсива нет вообще: они телепортируют и гасят DT (дефенсив — скрытые углы AA)
    return !fastFire();
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

void requestBreakLc() noexcept
{
    breakLcFrames = 8;
    // С включённым DT LC-триггер не сдвигает: каждый сдвиг — замереть на заряд и рывком догнать, то есть
    // телепорт, и DT гас-включался. Дефенсив при этом — скрытые углы AA (держатся постоянно), заряд — DT.
    if (fastFire())
        return;
    // зажат автопик — заряд держим под телепорт назад (иначе триггер тратил его на выглядывании,
    // и после выстрела возвращаться рывком было нечем)
    if (holdShot)
        return;
    // Нас никто не видит — ломать лагкомп не для кого. Раньше триггер «Воздух»/Force LC гонял по кругу
    // «сдвиг → 13 тиков зарядки → сдвиг» прямо на пустой карте, и в полёте/на бегу кидало туда-сюда.
    // Скрытые углы AA при этом держатся (синтетическое окно в AntiAim.cpp), заряд копится и ждёт врага:
    // как только кто-то увидел — сдвиг сразу (а пока он видит, перезарядки и так нет).
    if (!threat)
        return;
    // уже в окне — второй сдвиг ничего не добавит, только потратит заряд
    if (defensiveTicksLeft() == 0 && isCharged() && !shifting) {
        shiftRequested = true;
        requestedTicks = 0;
        lcShift = true;
    }
}

}

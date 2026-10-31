#include <algorithm>
#include <cstdlib>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>

#include <sys/mman.h>
#include <unistd.h>

#include "HvhConfig.h"
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
bool shifting = false;
bool holdShot = false;
bool sendNext = false;
int maxTickBase = 0;
int lagCompLeft = 0;
int shiftWindow = 0;            // тиков после нашего сдвига, пока сервер ещё не прислал откат тикбейса
int breakLcFrames = 0;          // AA просил ломать лагкомп недавно — заряд нужен

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
[[nodiscard]] bool wantCharge() noexcept
{
    const auto& ex = config.exploits;
    return wantDoubleTap() || (active && !fakeDuck && (ex.teleportKey.isSet() || ex.defensive || breakLcFrames > 0));
}

[[nodiscard]] int chokeLimit() noexcept
{
    if (fakeDuck)
        return 14;
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

    if (!wantCharge()) {
        charged = 0;
    } else if (rechargeDelay > 0) {
        --rechargeDelay;
    } else if (charged < maxShift && !shotPending && !shiftRequested && !(holdShot && charged >= 4)) {
        // (на выглядывании с автопиком, если на телепорт уже хватает, не замираем ради полного заряда)
        // зарядка: пропускаем тик, команда не создаётся — сервер потом примет пачку
        ++charged;
        return;
    }

    // выстрел (или тик сразу после него) в чоке не держим; фейкдаку чок нужен целиком
    setSendPacket((sendNext && !fakeDuck) || choked >= chokeLimit() || choked >= 14);
    sendNext = false;
    callOriginal(extraSamples, finalTick);

    // DT сдвигает сразу на выстреле — второй выстрел не ждёт (с автопиком возврат идёт в этих же командах)
    if (((shotPending && wantDoubleTap()) || shiftRequested) && charged > 0) {
        // сдвиг: прогоняем заряженные тики подряд, уходят одним пакетом с последним
        shifting = true;
        while (charged > 0) {
            --charged;
            setSendPacket(charged == 0);
            callOriginal(extraSamples, charged == 0 && finalTick);
        }
        shifting = false;
        rechargeDelay = 16;
        shiftWindow = 3;
    } else if (shotPending && rechargeDelay > 2) {
        // Второй выстрел DT уже ушёл (сдвиг был недавно, заряд пуст) — ждать остаток 16 тиков незачем:
        // следующий выстрел всё равно через перезарядку оружия. Заряд снова готов через ~16 тиков, а не ~30.
        rechargeDelay = 2;
    }
    shotPending = false;
    shiftRequested = false;
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

void requestShift() noexcept
{
    if (!shifting)
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

void requestBreakLc() noexcept
{
    breakLcFrames = 8;
    // уже в окне — второй сдвиг ничего не добавит, только потратит заряд
    if (defensiveTicksLeft() == 0 && isCharged() && !shifting)
        shiftRequested = true;
}

}

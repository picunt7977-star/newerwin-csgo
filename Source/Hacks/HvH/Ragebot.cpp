#include <algorithm>
#include <array>
#include <bit>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <optional>
#include <string>
#include <string_view>

#include <sys/stat.h>

#include <imgui/imgui.h>

#include "Autowall.h"
#include "HvhConfig.h"
#include "Ragebot.h"
#include "Resolver.h"
#include "Tickbase.h"

#include <Helpers.h>
#include <Interfaces/ClientInterfaces.h>
#include <Interfaces/EngineInterfaces.h>
#include <Interfaces/OtherInterfaces.h>
#include <Memory.h>
#include <CSGO/Constants/GameEventNames.h>
#include <CSGO/ConVar.h>
#include <CSGO/Cvar.h>
#include <CSGO/Engine.h>
#include <CSGO/GameEvent.h>
#include <CSGO/Entity.h>
#include <CSGO/EngineTrace.h>
#include <CSGO/EntityList.h>
#include <CSGO/GlobalVars.h>
#include <CSGO/LocalPlayer.h>
#include <CSGO/matrix3x4.h>
#include <CSGO/ModelInfo.h>
#include <CSGO/NetworkChannel.h>
#include <CSGO/UserCmd.h>
#include <CSGO/Vector.h>
#include <CSGO/WeaponData.h>
#include <CSGO/WeaponId.h>

namespace hvh::ragebot
{

namespace
{

constexpr int FL_ONGROUND = 1 << 0;
// сид разброса на сервере — (random_seed & 0xFF) + 1, то есть всего 256 вариантов: перебираем все
constexpr int HITCHANCE_SEEDS = 256;
// кости модели игрока: хитбоксы висят на первых ~80, больше 128 у моделей CS:GO не бывает
constexpr int RECORD_BONES = 128;
// записей на игрока: 0.2 с (sv_maxunlag) на 128 тиках = 26, с запасом
constexpr int MAX_RECORDS = 32;

bool minDamageOverride = false;
bool forceBody = false;
int target = 0;
const char* statusText = "";

// промахи по игрокам: выстрел без player_hurt за 0.7 с считается промахом
std::array<int, 65> misses{};
std::array<float, 65> resolverOffset{};
std::array<float, 65> pendingShot{};
float localDiedAt = -1.0f;      // realtime нашей смерти — промах без импакта после неё = «death»

// ---------- резольвер: состояние на игрока ----------
// Режимы: угаданная сторона, обратная, они же с половинным десинком (low delta), без поворота.
// У каждого режима счёт: попадание +2, промах по вине резольвера -1. Стреляем режимом с лучшим счётом —
// так резольвер «учится» на каждом игроке и не сбивается от промахов по разбросу.
enum ResolveMode { Guess, Opposite, NoResolve, GuessLow, OppositeLow, ModeCount };
constexpr const char* modeNames[]{ "угадано", "обратно", "без поворота", "угадано low", "обратно low" };
// режим «слои» живёт в резольвере по анимации, у перебора стороны для него номера нет
constexpr int LayerMode = -2;
// слои сейчас молчат, сторону берём из прошлой находки по слоям (см. rememberedRatio)
constexpr int MemoryMode = -3;

[[nodiscard]] const char* modeText(int mode) noexcept
{
    if (mode == LayerMode)
        return "слои";
    if (mode == MemoryMode)
        return "память слоёв";
    return mode >= 0 && mode < ModeCount ? modeNames[mode] : "не десинкает";
}

struct ResolverState {
    int userId = -1;
    bool tracked = false;
    float lastSimulationTime = 0.0f;
    float lastEyeYaw = 0.0f;
    float lastYawDelta = 0.0f;
    std::uint8_t chokeHistory = 0xFF;   // бит на обновление: игрок чокал (иначе десинка нет)
    std::uint8_t jitterHistory = 0;     // бит на обновление: yaw скачет туда-обратно
    float jitterPhase = 1.0f;           // в какую сторону сейчас дёрнулся yaw
    float guessedSide = 0.0f;           // фристенд-догадка (обновляется, когда нет пули в полёте)
    std::array<int, ModeCount> score{};
    int shotMode = -1;                  // режим, которым стреляли последним
    // эксплойты врага
    csgo::Vector lastOrigin{};
    float maxSimulationTime = 0.0f;
    bool breakingLagComp = false;       // за одно обновление сдвинулся > 64 юнитов — сервер его не откатывает
    float defensiveUntil = 0.0f;        // время симуляции ушло назад (дефенсив/сдвиг тикбейса) — записи врут
    // Память стороны по слоям. Слои видят угол ног в основном на ходу, а стреляем мы чаще по молчащим
    // обновлениям — там сторону гадал фристенд, и по логам 29.10 «вперёд» с догадкой попадало 8 из 31,
    // а с углом от слоёв 10 из 15. Статичный десинк держит одну сторону секундами — её и помним.
    // Для джиттера учим, совпадает ли сторона с фазой yaw (у одних читов совпадает, у других наоборот).
    float learnedFoundAt = -1.0f;       // simtime последней учтённой находки
    float learnedAt = -100.0f;          // realtime её же
    float memoryRatio = 0.0f;           // дельта находки в долях максимума, знак — сторона
    std::uint8_t sameSideBits = 0;      // бит на находку: сторона та же, что у прошлой
    std::uint8_t sameSideCount = 0;     // сколько бит выше настоящих (до 8)
    std::uint8_t jitterSideBits = 0;    // бит на находку при джиттере: сторона = фаза yaw
    std::uint8_t jitterSideCount = 0;
    std::uint8_t memoryMisses = 0;      // промахи по памяти подряд
    // После промаха по вине резольвера следующий выстрел бьёт зеркальной стороной, пока не попадём:
    // перебор стороны получается быстрее, чем ждать, пока счёт режимов перевесит
    float bruteSide = 0.0f;
    float bruteUntil = 0.0f;
};
std::array<ResolverState, 65> resolverStates{};

// m_angEyeAngles у чужих игроков интерполируется: к CreateMove там уже сглаженный угол на lerp позади,
// у джиттера — где-то между фазами. Настоящий, присланный сервером, живёт только до RENDER_START —
// снимаем его в NET_UPDATE_POSTDATAUPDATE_START вместе с simtime, к которому он относится.
struct NetworkAngles {
    float simulationTime = -1.0f;
    csgo::Vector eye{};
};
std::array<NetworkAngles, 65> networkAngles{};

[[nodiscard]] csgo::Vector serverEyeAngles(int index, const csgo::Entity& entity) noexcept
{
    if (index > 0 && index < static_cast<int>(networkAngles.size()) && networkAngles[index].simulationTime == entity.simulationTime())
        return networkAngles[index].eye;
    return entity.eyeAngles();
}
std::array<int, 65> resolverMode{};  // режим на текущем тике, -1 — резольвер не применялся

[[nodiscard]] bool isDesyncing(const ResolverState& state) noexcept
{
    // хотя бы 2 чока за последние 8 обновлений; без чока десинка не бывает
    return !state.tracked || std::popcount(state.chokeHistory) >= 2;
}

[[nodiscard]] bool isJittering(const ResolverState& state) noexcept
{
    return std::popcount(static_cast<std::uint8_t>(state.jitterHistory & 0x0F)) >= 2;
}

void trackPlayer(ResolverState& state, const csgo::Entity& entity, int index, int userId, float interval, float realtime) noexcept
{
    if (state.userId != userId) {
        // на этом слоте другой игрок — всё выученное не про него
        state = {};
        state.userId = userId;
    }
    const float simulationTime = entity.simulationTime();
    if (state.tracked && simulationTime == state.lastSimulationTime)
        return;

    const float eyeYaw = serverEyeAngles(index, entity).y;
    const auto origin = entity.origin();
    if (state.tracked) {
        // > 64 юнитов между обновлениями: сервер выключает для него лагкомпенсацию
        state.breakingLagComp = (origin - state.lastOrigin).squareLength() > 64.0f * 64.0f;
        // время пошло назад — враг сдвинул тикбейс (дефенсив/DT): его записи сейчас не совпадают с сервером
        if (simulationTime + 0.001f < state.maxSimulationTime)
            state.defensiveUntil = realtime + 0.2f;
    }
    state.lastOrigin = origin;
    state.maxSimulationTime = std::max(state.maxSimulationTime, simulationTime);
    if (state.tracked && interval > 0.0f) {
        const int ticks = static_cast<int>(std::lround((simulationTime - state.lastSimulationTime) / interval));
        // большие разрывы — это мы не следили (дормант, не та пушка), а не чок
        if (ticks >= 1 && ticks <= 17)
            state.chokeHistory = static_cast<std::uint8_t>((state.chokeHistory << 1) | (ticks > 1 ? 1 : 0));

        const float delta = std::remainder(eyeYaw - state.lastEyeYaw, 360.0f);
        const bool jitter = std::abs(delta) > 25.0f && std::abs(state.lastYawDelta) > 25.0f && (delta > 0.0f) != (state.lastYawDelta > 0.0f);
        state.jitterHistory = static_cast<std::uint8_t>((state.jitterHistory << 1) | (jitter ? 1 : 0));
        if (std::abs(delta) > 25.0f)
            state.jitterPhase = delta > 0.0f ? 1.0f : -1.0f;
        state.lastYawDelta = delta;
    }

    // резольвер разбирает обновление раньше (NET_UPDATE), так что находка на этом simtime уже есть
    float foundAt = 0.0f, ratio = 0.0f;
    if (hvh::resolver::lastFound(index, foundAt, ratio) && foundAt == simulationTime && foundAt != state.learnedFoundAt
        && std::abs(ratio) > 0.3f) {
        const bool positive = ratio > 0.0f;
        // соседние обновления одного эпизода — не новые свидетельства, иначе «совпадений» будет 100%
        if (realtime - state.learnedAt > 0.25f) {
            if (state.learnedAt > 0.0f) {
                state.sameSideBits = static_cast<std::uint8_t>((state.sameSideBits << 1) | (positive == (state.memoryRatio > 0.0f) ? 1 : 0));
                state.sameSideCount = static_cast<std::uint8_t>(std::min(state.sameSideCount + 1, 8));
            }
            if (isJittering(state)) {
                state.jitterSideBits = static_cast<std::uint8_t>((state.jitterSideBits << 1) | (positive == (state.jitterPhase > 0.0f) ? 1 : 0));
                state.jitterSideCount = static_cast<std::uint8_t>(std::min(state.jitterSideCount + 1, 8));
            }
        }
        state.learnedFoundAt = foundAt;
        state.learnedAt = realtime;
        state.memoryRatio = ratio;
        state.memoryMisses = 0;
    }
    state.lastSimulationTime = simulationTime;
    state.lastEyeYaw = eyeYaw;
    state.tracked = true;
}

[[nodiscard]] float agreement(std::uint8_t bits, std::uint8_t count) noexcept
{
    const unsigned mask = count >= 8 ? 0xFFu : (1u << count) - 1u;
    return count ? static_cast<float>(std::popcount(static_cast<unsigned>(bits) & mask)) / static_cast<float>(count) : 0.0f;
}

// Статичный десинк: дельта из памяти (доли максимума) или 0. Свежая единичная находка лучше фристенда,
// старше — только если сторона у врага не скачет (≥ 70% находок совпали с предыдущей).
[[nodiscard]] float rememberedRatio(const ResolverState& state, float realtime) noexcept
{
    if (state.learnedAt <= 0.0f || state.memoryMisses >= 2 || isJittering(state))
        return 0.0f;
    const float age = realtime - state.learnedAt;
    const bool steady = state.sameSideCount >= 2 && agreement(state.sameSideBits, state.sameSideCount) >= 0.7f;
    const bool flips = state.sameSideCount >= 2 && agreement(state.sameSideBits, state.sameSideCount) < 0.5f;
    if (flips || age > (steady ? 10.0f : 3.0f))
        return 0.0f;
    return state.memoryRatio;
}

// Джиттер: +1 — сторона десинка совпадает с фазой yaw, -1 — противоположна, 0 — не выучено (берём фазу как есть)
[[nodiscard]] float jitterRelation(const ResolverState& state) noexcept
{
    if (state.jitterSideCount < 2)
        return 0.0f;
    const float agree = agreement(state.jitterSideBits, state.jitterSideCount);
    return agree >= 0.7f ? 1.0f : agree <= 0.3f ? -1.0f : 0.0f;
}

[[nodiscard]] int bestMode(const ResolverState& state) noexcept
{
    // «без поворота» не перебираем: по логам 29.10 — 1 попадание из 12, на HvH десинкают все,
    // а он выигрывал счёт просто потому, что остальные режимы набрали промахов
    int best = 0;
    for (int mode = 1; mode < ModeCount; ++mode) {
        if (mode == NoResolve)
            continue;
        if (state.score[mode] > state.score[best])
            best = mode;
    }
    return best;
}

// ---------- свой лагкомп: записи игрока на каждое новое simulationTime ----------
// Сервер при выстреле откатывает цель ровно в состояние на cmd->tickcount - lerp. Если стрелять по записи
// с её simtime и ставить tickcount = simtime + lerp, сервер проверит попадание по той же модели, что и мы, —
// без интерполяции между обновлениями, которая и давала «пуля прошла через точку, а модели там не было».

// откуда взята модель, по которой стреляем (приоритет: выстрел врага > стоит на месте > свежая > старая)
// «вперёд» — свежая запись старше окна отката: сервер возьмёт своё текущее состояние врага, оно новее нашего
enum class RecordKind { Onshot, Standing, Newest, Oldest, Live, Extrapolated, Ahead };
constexpr const char* kindNames[]{ "onshot", "стоит", "свежая", "старая", "без бектрека", "экстраполяция", "вперёд" };

struct LagRecord {
    float simulationTime = 0.0f;
    csgo::Vector origin{};          // m_vecOrigin — где игрок стоит на сервере в этот simtime
    csgo::Vector eyeAngles{};
    float absYaw = 0.0f;            // куда клиент повернул ноги (от этого считает резольвер)
    csgo::Vector velocity{};
    int flags = 0;
    int choked = 0;                 // тиков между этим и прошлым обновлением (1 — не чокал)
    bool shot = false;              // в этом обновлении враг выстрелил: углы настоящие, не фейк
    bool breakingLagComp = false;   // сдвинулся > 64 юнитов — сервер не откатывает, только экстраполяция
    float receivedAt = 0.0f;        // realtime, когда запись пришла к нам
    bool rebuilt = false;           // plus/minus собраны анимацией (ноги = взгляд ± десинк), а не поворотом костей
    bool resolved = false;          // bones собраны под угол ног от резольвера по слоям анимации
    bool remembered = false;        // ...или под сторону из памяти прошлых находок (слои молчат)
    float resolvedYaw = 0.0f;
    csgo::matrix3x4 bones[RECORD_BONES];
    csgo::matrix3x4 plus[RECORD_BONES];
    csgo::matrix3x4 minus[RECORD_BONES];
};

struct PlayerRecords {
    std::array<LagRecord, MAX_RECORDS> ring;
    int newestIndex = -1;
    int count = 0;
    float skippedSimulationTime = -1.0f;    // запись после отката времени — её не берём

    // age 0 — самая свежая
    [[nodiscard]] const LagRecord& at(int age) const noexcept { return ring[(newestIndex - age + MAX_RECORDS) % MAX_RECORDS]; }
    LagRecord& push() noexcept
    {
        newestIndex = (newestIndex + 1) % MAX_RECORDS;
        count = std::min(count + 1, MAX_RECORDS);
        return ring[newestIndex];
    }
    void popNewest() noexcept
    {
        if (count <= 0)
            return;
        newestIndex = (newestIndex - 1 + MAX_RECORDS) % MAX_RECORDS;
        --count;
    }
    void clear() noexcept
    {
        count = 0;
        newestIndex = -1;
        skippedSimulationTime = -1.0f;
    }
};
std::array<PlayerRecords, 65> records;

// чем быстрее бежит, тем меньше возможный десинк (~58° стоя, ~29° на полном беге)
[[nodiscard]] float maxDesyncFor(const csgo::Vector& velocity) noexcept
{
    return 58.0f * (1.0f - 0.5f * std::clamp(velocity.length2D() / 250.0f, 0.0f, 1.0f));
}

// ---------- пересборка анимации ----------
// Вместо поворота готовых костей ставим модели то, что есть на сервере: ноги (abs yaw) = взгляд ± десинк,
// body_yaw = взгляд - ноги, move_yaw — от новых ног, сбрасываем кэш костей и зовём setupBones.
// Офсеты кэша берём из байтов C_BaseAnimating::InvalidateBoneCache (паттерн в LinuxClientPatternFinder):
//   mov rax,[rip+g_iModelBoneCounter]; mov dword [rdi+m_flLastBoneSetupTime],-FLT_MAX; sub rax,1; mov [rdi+m_iMostRecentModelBoneCounter],rax
// Саму функцию не зовём: она закрыта флагом s_bEnableInvalidateBoneCache и вне отрисовки ничего не делает.
constexpr int POSE_MOVE_YAW = 7;    // [-180, 180]
constexpr int POSE_BODY_YAW = 11;   // [-60, 60] (animset_t.mdl)

struct BoneCacheOffsets {
    bool ok = false;
    int lastBoneSetupTime = 0;
    int mostRecentModelBoneCounter = 0;
    const std::uint64_t* modelBoneCounter = nullptr;
};

const BoneCacheOffsets& boneCacheOffsets(const Memory& memory) noexcept
{
    static const auto offsets = [&memory] {
        BoneCacheOffsets result;
        const auto function = memory.invalidateBoneCache;
        if (!function)
            return result;
        std::int32_t counterDisplacement, lastBoneSetupTime, mostRecentModelBoneCounter;
        std::memcpy(&counterDisplacement, function + 16, 4);
        std::memcpy(&lastBoneSetupTime, function + 22, 4);
        std::memcpy(&mostRecentModelBoneCounter, function + 37, 4);
        // на этой сборке игры: 0x2f98 и 0x2c58 — всё, что сильно в стороне, считаем чужой функцией
        if (lastBoneSetupTime < 0x1000 || lastBoneSetupTime > 0x8000 || mostRecentModelBoneCounter < 0x1000 || mostRecentModelBoneCounter > 0x8000)
            return result;
        result.lastBoneSetupTime = lastBoneSetupTime;
        result.mostRecentModelBoneCounter = mostRecentModelBoneCounter;
        result.modelBoneCounter = reinterpret_cast<const std::uint64_t*>(function + 20 + counterDisplacement);
        result.ok = true;
        return result;
    }();
    return offsets;
}

void invalidateBoneCache(const csgo::Entity& entity, const BoneCacheOffsets& offsets) noexcept
{
    const auto base = reinterpret_cast<std::uintptr_t>(entity.getPOD());
    *reinterpret_cast<float*>(base + offsets.lastBoneSetupTime) = -FLT_MAX;
    *reinterpret_cast<std::uint64_t*>(base + offsets.mostRecentModelBoneCounter) = *offsets.modelBoneCounter - 1;
}

int rebuildFailures = 0;        // подряд скелетов, где пересборка ничего не поменяла
bool rebuildDisabled = false;   // кэш костей так и не сбрасывается — дальше только поворот по высоте

// Собирает скелет при заданном угле ног: ставит animstate-подобные позы (body_yaw/move_yaw),
// крутит ноги модели на feetYaw, сбрасывает кэш костей и просит у игры скелет.
bool rebuildAtYaw(const csgo::Entity& entity, const Memory& memory, const LagRecord& record, float feetYaw, csgo::matrix3x4* out) noexcept
{
    const auto& offsets = boneCacheOffsets(memory);
    if (!offsets.ok || rebuildDisabled)
        return false;

    auto& poses = entity.poseParameters();
    auto& absAngles = const_cast<csgo::Vector&>(entity.getAbsAngle());
    auto& rotation = entity.angRotation();
    const float savedBodyYaw = poses[POSE_BODY_YAW];
    const float savedMoveYaw = poses[POSE_MOVE_YAW];
    const auto savedAbsAngles = absAngles;
    const auto savedRotation = rotation;

    const float eyeYaw = record.eyeAngles.y;
    const bool moving = record.velocity.length2D() > 0.1f;
    const float velocityYaw = Helpers::rad2deg(std::atan2(record.velocity.y, record.velocity.x));
    const auto shift = record.origin - entity.getAbsOrigin();

    feetYaw = std::remainder(feetYaw, 360.0f);
    poses[POSE_BODY_YAW] = std::clamp((std::remainder(eyeYaw - feetYaw, 360.0f) + 60.0f) / 120.0f, 0.0f, 1.0f);
    if (moving)
        poses[POSE_MOVE_YAW] = (std::remainder(velocityYaw - feetYaw, 360.0f) + 180.0f) / 360.0f;
    absAngles.y = feetYaw;
    rotation.y = feetYaw;
    invalidateBoneCache(entity, offsets);

    static csgo::matrix3x4 setup[MAXSTUDIOBONES];
    const bool ok = entity.setupBones(setup, MAXSTUDIOBONES, BONE_USED_BY_HITBOX, memory.globalVars->currenttime);
    if (ok) {
        for (int bone = 0; bone < RECORD_BONES; ++bone) {
            out[bone] = setup[bone];
            out[bone].translate(shift);
        }
    }

    // вернуть как было и снова сбросить кэш — иначе этот кадр отрисует наш последний скелет
    poses[POSE_BODY_YAW] = savedBodyYaw;
    poses[POSE_MOVE_YAW] = savedMoveYaw;
    absAngles = savedAbsAngles;
    rotation = savedRotation;
    invalidateBoneCache(entity, offsets);
    return ok;
}

// plus/minus — крайние углы ног (для безопасных точек), а если резольвер по слоям дал угол,
// то и сам скелет записи собираем под него: стреляем ровно по той модели, что на сервере.
bool rebuildDesyncVariants(const csgo::Entity& entity, const Memory& memory, LagRecord& record, float maxDesync,
    float resolvedYaw, bool haveResolved, bool needExtremes) noexcept
{
    const auto& offsets = boneCacheOffsets(memory);
    if (!offsets.ok || rebuildDisabled)
        return false;

    // Резольвер дал угол, а безопасные точки выключены — крайние скелеты не нужны,
    // а каждый из них это ещё один setupBones (самое дорогое, что чит делает за тик).
    if (haveResolved && !needExtremes) {
        if (!rebuildAtYaw(entity, memory, record, resolvedYaw, record.plus))
            return false;
        constexpr int headBone = 8;
        if ((record.plus[headBone].origin() - record.bones[headBone].origin()).length() >= 40.0f)
            return false;   // скелет уехал — пересборка сломалась, лучше стрелять по клиентскому
        std::copy(record.plus, record.plus + RECORD_BONES, record.bones);
        record.resolved = true;
        record.resolvedYaw = std::remainder(resolvedYaw, 360.0f);
        return false;       // plus/minus не построены — геометрических вариантов нет
    }

    const float eyeYaw = record.eyeAngles.y;
    if (!rebuildAtYaw(entity, memory, record, eyeYaw + maxDesync, record.plus)
        || !rebuildAtYaw(entity, memory, record, eyeYaw - maxDesync, record.minus))
        return false;

    // самопроверка: при десинке ~30°+ голова обязана сдвинуться, но не улететь за пределы модели
    constexpr int headBone = 8;
    const float plusMove = (record.plus[headBone].origin() - record.bones[headBone].origin()).length();
    const float minusMove = (record.minus[headBone].origin() - record.bones[headBone].origin()).length();
    const bool sane = plusMove < 40.0f && minusMove < 40.0f;
    const bool changed = std::max(plusMove, minusMove) > 0.5f;
    if (!sane || (!changed && maxDesync > 25.0f)) {
        if (++rebuildFailures >= 8) {
            rebuildDisabled = true;
            static constexpr std::uint8_t yellow[4]{ 255, 210, 90, 255 };
            if (memory.conColorMsg)
                memory.conColorMsg(yellow, "%s\n", "[ECLIPSE] пересборка анимации не меняет скелет — резольвер вернулся к повороту костей по высоте");
        }
        return false;
    }
    rebuildFailures = 0;

    if (haveResolved && rebuildAtYaw(entity, memory, record, resolvedYaw, record.bones)) {
        record.resolved = true;
        record.resolvedYaw = std::remainder(resolvedYaw, 360.0f);
    } else if (std::abs(std::remainder(entity.eyeAngles().y - eyeYaw, 360.0f)) > 5.0f) {
        // клиент собрал скелет со сглаженным взглядом (джиттер) — верх тела смотрит не туда, куда на сервере;
        // пересобираем «без поворота» с теми же ногами, но настоящим взглядом
        rebuildAtYaw(entity, memory, record, record.absYaw, record.bones);
    }
    return true;
}

void updateRecords(PlayerRecords& list, const csgo::Entity& entity, const Memory& memory, bool rebuild, int index) noexcept
{
    const float simulationTime = entity.simulationTime();
    if (list.count && simulationTime == list.at(0).simulationTime)
        return;
    if (simulationTime == list.skippedSimulationTime)
        return;

    if (list.count && simulationTime < list.at(0).simulationTime) {
        // время ушло назад — дефенсив/сдвиг тикбейса. Записи новее этого момента сервер теперь не найдёт:
        // при откате он берёт первую запись с simtime <= цели, начиная с новых, а новые теперь «из прошлого».
        // Саму эту запись тоже пропускаем, ждать её «починки» не будем — стреляем по остальным.
        while (list.count && list.at(0).simulationTime >= simulationTime)
            list.popNewest();
        list.skippedSimulationTime = simulationTime;
        return;
    }

    static csgo::matrix3x4 bones[MAXSTUDIOBONES];
    if (!entity.setupBones(bones, MAXSTUDIOBONES, BONE_USED_BY_HITBOX, memory.globalVars->currenttime))
        return;

    const float interval = memory.globalVars->intervalPerTick;
    const bool havePrevious = list.count > 0;
    const float previousSimulationTime = havePrevious ? list.at(0).simulationTime : 0.0f;
    const auto previousOrigin = havePrevious ? list.at(0).origin : csgo::Vector{};

    auto& record = list.push();
    record.simulationTime = simulationTime;
    record.receivedAt = memory.globalVars->realtime;
    record.origin = entity.origin();
    record.eyeAngles = serverEyeAngles(index, entity);
    record.absYaw = entity.getAbsAngle().y;
    record.velocity = entity.velocity();
    record.flags = entity.flags();
    record.choked = havePrevious && interval > 0.0f ? std::clamp(static_cast<int>(std::lround((simulationTime - previousSimulationTime) / interval)), 1, 17) : 1;
    record.breakingLagComp = havePrevious && (record.origin - previousOrigin).squareLength() > 64.0f * 64.0f;
    record.shot = false;
    if (const auto weapon = csgo::Entity::from(retSpoofGadgets->client, entity.getActiveWeapon()); weapon.getPOD())
        record.shot = std::abs(weapon.lastShotTime() - simulationTime) < interval * 0.5f;

    // setupBones отдаёт скелет в интерполированной позиции (getAbsOrigin); на сервере в этот simtime
    // игрок стоит в m_vecOrigin — переносим скелет туда целиком
    const auto shift = record.origin - entity.getAbsOrigin();
    for (int bone = 0; bone < RECORD_BONES; ++bone) {
        record.bones[bone] = bones[bone];
        record.bones[bone].translate(shift);
    }
    record.rebuilt = false;
    record.resolved = false;
    record.remembered = false;
    record.resolvedYaw = 0.0f;
    if (rebuild) {
        // максимальный десинк — по формуле самого клиента из animstate врага, а не на глаз
        const float animMax = entity.getMaxDesyncAngle();
        const float maxDesync = animMax > 1.0f ? std::clamp(animMax, 0.0f, 60.0f) : maxDesyncFor(record.velocity);
        const bool haveResolved = hvh::resolver::valid(index);
        // слои молчат — сторона из прошлой находки, если враг её держит (статичный десинк)
        const auto& state = resolverStates[index];
        const float remembered = haveResolved || !isDesyncing(state) ? 0.0f : rememberedRatio(state, memory.globalVars->realtime);
        const bool useMemory = remembered != 0.0f;
        const float yaw = haveResolved ? hvh::resolver::footYaw(index) : record.eyeAngles.y + remembered * maxDesync;
        record.rebuilt = rebuildDesyncVariants(entity, memory, record, maxDesync,
            haveResolved || useMemory ? yaw : 0.0f, haveResolved || useMemory, config.rage.safePoint != 0);
        record.remembered = useMemory && record.resolved;
    }
}

// Стандартная проверка лагкомпа: сервер откатит на cmd->tickcount, только если эта цель
// не дальше 0.2 с от «правильного» времени (пинг туда + обратно + lerp) и не старше sv_maxunlag
[[nodiscard]] bool recordValid(float simulationTime, float serverTime, float latencyOut, float latencyIn, float lerp, float maxUnlag) noexcept
{
    // как на сервере: его исходящая задержка (к нам — наша входящая) + lerp, без нашей исходящей
    (void)latencyOut;
    const float correct = std::clamp(latencyIn + lerp, 0.0f, maxUnlag);
    if (std::abs(correct - (serverTime - simulationTime)) >= 0.2f)
        return false;
    // serverTime — наш тикбейс, то есть уже время, когда сервер выполнит команду (пинг в нём учтён).
    // Раньше тут был ещё + latencyOut: пинг считался дважды, и при пинге от ~100 мс не проходила ни одна запись
    return simulationTime >= serverTime - maxUnlag;
}

// Сколько обновлений врага сервер успеет принять сверх нашей свежей записи к моменту, когда выполнит наш выстрел:
// запись уже лежит у нас (realtime - receivedAt), плюс пинг туда-обратно; враг шлёт пакет раз в choked тиков.
// Движение считаем не по m_vecVelocity (у чужих игроков она оценочная), а по самим записям: смещение между
// двумя последними, поворот скорости между тремя (распрыжка с автострейфом идёт дугой), гравитация и земля.
// Дальше прогоняем это по тикам до нужного обновления.
// Поправка к числу обновлений, которые успеет принять сервер: пинг из getLatency не знает, сколько наша
// команда ждёт на сервере и сколько стоит в чоке. Подстраивается по промахам «вперёд» (calibrateAhead).
float aheadBias = 0.0f;

// Сколько тиков враг обычно чокает: медиана последних 5 записей. По одной свежей нельзя — у фейклага
// с рандомом чок скачет 2/15/2, и «+7 обн.» по чоку 2 означает, что мы увели модель на 200 мс вперёд,
// а сервер всё это время держал её на месте до следующего пакета.
[[nodiscard]] int typicalChoke(const PlayerRecords& list) noexcept
{
    std::array<int, 5> chokes{};
    const int count = std::min(list.count, static_cast<int>(chokes.size()));
    if (count == 0)
        return 1;
    for (int age = 0; age < count; ++age)
        chokes[age] = std::max(list.at(age).choked, 1);
    std::sort(chokes.begin(), chokes.begin() + count);
    return chokes[count / 2];
}

// step — на сколько враг сдвинется за одно своё обновление сверх предсказанного (для точек, устойчивых к ±1 обн.)
int predictAhead(const PlayerRecords& list, LagRecord& out, float realtime, float latency, float interval, float gravity, csgo::Vector& step) noexcept
{
    const auto& newest = list.at(0);
    const int choke = typicalChoke(list);
    const float updateTime = static_cast<float>(choke) * interval;
    // Мы уже дольше обычного не получаем от него ничего — значит, он не шлёт (длинный чок, зарядка DT),
    // и у сервера данных не новее нашей записи: модель стоит там же, пока не придёт пачка. Раньше в такой
    // тишине насчитывали «+8 обн.» и уводили модель на 250 мс туда, где её не было.
    const float silence = realtime - newest.receivedAt;
    const float elapsed = std::clamp(silence + latency, 0.0f, 0.5f);
    const int updates = silence > updateTime * 1.5f
        ? 0 : std::max(static_cast<int>(elapsed / updateTime) + static_cast<int>(std::lround(aheadBias)), 0);
    const int ticks = std::min(updates * choke, static_cast<int>(0.3f / interval));

    auto velocity = newest.velocity;
    float turnRate = 0.0f;   // градусов в секунду
    if (list.count >= 2) {
        const auto& previous = list.at(1);
        const float dt = newest.simulationTime - previous.simulationTime;
        if (dt > 0.0f && dt <= 0.5f && (newest.origin - previous.origin).squareLength() < 64.0f * 64.0f) {
            velocity = (newest.origin - previous.origin) / dt;
            // средняя вертикальная скорость за отрезок -> скорость в момент свежей записи
            if (!(newest.flags & FL_ONGROUND))
                velocity.z -= 0.5f * gravity * dt;
            if (list.count >= 3) {
                const auto& older = list.at(2);
                const float dt2 = previous.simulationTime - older.simulationTime;
                if (dt2 > 0.0f && dt2 <= 0.5f) {
                    const auto velocityBefore = (previous.origin - older.origin) / dt2;
                    if (velocity.length2D() > 30.0f && velocityBefore.length2D() > 30.0f) {
                        const float yawNow = std::atan2(velocity.y, velocity.x);
                        const float yawBefore = std::atan2(velocityBefore.y, velocityBefore.x);
                        turnRate = std::clamp(Helpers::rad2deg(std::remainder(yawNow - yawBefore, 2.0f * 3.14159265f)) / (0.5f * (dt + dt2)), -300.0f, 300.0f);
                    }
                }
            }
        }
    }
    if (newest.flags & FL_ONGROUND)
        velocity.z = 0.0f;

    float groundZ = 1e9f;
    for (int age = 0; age < list.count; ++age) {
        if (list.at(age).flags & FL_ONGROUND)
            groundZ = std::min(groundZ, list.at(age).origin.z);
    }

    csgo::Vector position = newest.origin;
    bool onGround = newest.flags & FL_ONGROUND;
    const float turnPerTick = Helpers::deg2rad(turnRate * interval);
    const float c = std::cos(turnPerTick), s = std::sin(turnPerTick);
    for (int tick = 0; tick < ticks; ++tick) {
        const float x = velocity.x * c - velocity.y * s;
        velocity.y = velocity.x * s + velocity.y * c;
        velocity.x = x;
        position.x += velocity.x * interval;
        position.y += velocity.y * interval;
        if (!onGround) {
            velocity.z -= gravity * interval;
            position.z += velocity.z * interval;
            if (groundZ < 1e8f && position.z < groundZ) {
                position.z = groundZ;
                velocity.z = 0.0f;
            }
        }
    }

    step = { velocity.x * updateTime, velocity.y * updateTime, onGround ? 0.0f : velocity.z * updateTime };
    out = newest;
    out.choked = choke;
    const auto shift = position - newest.origin;
    for (int bone = 0; bone < RECORD_BONES; ++bone) {
        out.bones[bone].translate(shift);
        out.plus[bone].translate(shift);
        out.minus[bone].translate(shift);
    }
    out.origin = position;
    out.velocity = velocity;
    // время того обновления врага, в котором он, по нашему расчёту, будет на сервере
    out.simulationTime = newest.simulationTime + static_cast<float>(updates) * updateTime;
    return updates;
}

[[nodiscard]] int timeToTicks(float time, float interval) noexcept
{
    return static_cast<int>(0.5f + time / interval);
}

// ---------- логи выстрелов в консоль ----------

// хитбоксы-капсулы одной версии скелета: считаем один раз, дальше по ним сотни лучей
struct Capsule {
    csgo::Vector a, b;
    float radius;
    int group;
};
struct Capsules {
    std::array<Capsule, 32> items;
    int count = 0;
};

bool rayHitsPlayer(const csgo::Vector& start, const csgo::Vector& end, const Capsules& capsules) noexcept;
void buildCapsules(const csgo::StudioHitboxSet& set, const csgo::matrix3x4* bones, Capsules& out) noexcept;

struct ShotRecord {
    csgo::Vector eye;
    csgo::Vector aimPoint;
    bool isHead = false;
    bool edgePoint = false;  // мультипоинт (край), а не центр хитбокса
    bool safe = false;
    float side = 0.0f;       // резольвер: +1/-1, ±0.5 — low delta, 0 — без поправки
    int damage = 0;
    int mode = -1;
    RecordKind kind = RecordKind::Live;
    int backtrackTicks = 0;  // на сколько тиков запись старше самой свежей
    int hitchance = 0;       // % из 256 сидов
    float enemySpeed = 0.0f;
    int enemyChoke = 0;
    // почему без бектрека: сколько было записей, возраст свежей, пинг, sv_maxunlag
    int updatesAhead = 0;    // «вперёд»: на сколько обновлений врага продвинули запись
    int recordCount = 0;
    int newestAgeMs = 0;
    int pingMs = 0;
    int maxUnlagMs = 0;
    Capsules capsules;       // скелет, по которому стреляли: прошла ли пуля сквозь нашу модель
    // проверка предсказания «вперёд»: где он, по расчёту, будет и с каким взглядом
    bool predicted = false;
    float predictedSimulationTime = 0.0f;
    csgo::Vector predictedOrigin{};
    float usedEyeYaw = 0.0f;
    float updateTime = 0.0f;                    // как часто враг слал обновления на момент выстрела
    const csgo::StudioHitboxSet* set = nullptr; // хитбоксы цели — проверить пулю по пришедшим потом записям
    // часы: насколько наш тикбейс впереди часов клиента и насколько simtime врага позади
    int ourClockMs = 0;
    int enemyLagMs = 0;
    bool haveImpact = false;
    csgo::Vector impact;     // самый дальний импакт пули
};
std::array<ShotRecord, 65> shots{};
int lastShotIndex = 0;
float lastShotTime = 0.0f;

const Memory* logMemory = nullptr;
std::optional<csgo::Engine> logEngine;  // обёртка над указателем, храним копию

// Всё, что пишем в консоль, дублируем в ~/csgo-hvh/logs/ДАТА.log — чтобы не копировать из консоли игры
void fileLog(const char* text) noexcept
{
    static std::FILE* file = nullptr;
    static int day = -1;
    const std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_r(&now, &local);
    if (!file || local.tm_yday != day) {
        if (file)
            std::fclose(file);
        file = nullptr;
        day = local.tm_yday;
        const char* home = std::getenv("HOME");
        if (!home)
            return;
        const std::string dir = std::string{ home } + "/csgo-hvh/logs";
        mkdir(dir.c_str(), 0755);
        char name[32];
        std::strftime(name, sizeof(name), "/%Y-%m-%d.log", &local);
        file = std::fopen((dir + name).c_str(), "a");
        if (!file)
            return;
    }
    char stamp[16];
    std::strftime(stamp, sizeof(stamp), "%H:%M:%S ", &local);
    std::fputs(stamp, file);
    std::fputs(text, file);
    std::fputc('\n', file);
    std::fflush(file);
}

// короткие строки для экрана: «попал в X: голова -100», «промах по X: prediction error»
struct ScreenLine {
    std::string text;
    ImU32 color = 0;
    double time = 0.0;
};
std::array<ScreenLine, 8> screenLines;
int screenNext = 0;

[[nodiscard]] double steadySeconds() noexcept
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void screenLog(ImU32 color, std::string text) noexcept
{
    auto& line = screenLines[screenNext];
    line = { std::move(text), color, steadySeconds() };
    screenNext = (screenNext + 1) % static_cast<int>(screenLines.size());
}

void consoleLog(const std::uint8_t (&color)[4], const char* text) noexcept
{
    if (logMemory && logMemory->conColorMsg)
        logMemory->conColorMsg(color, "%s\n", text);
    fileLog(text);
}

[[nodiscard]] std::string playerName(int index) noexcept
{
    if (csgo::PlayerInfo info; logEngine && logEngine->getPlayerInfo(index, info))
        return info.name;
    return "#" + std::to_string(index);
}

[[nodiscard]] const char* sideText(float side) noexcept
{
    if (side == 0.0f)
        return "без резольвера";
    if (std::abs(side) < 1.0f)
        return side > 0.0f ? "+ low" : "- low";
    return side > 0.0f ? "+" : "-";
}

// «запись onshot, бт 3т, хш 74%, скорость 120, чок 2»
[[nodiscard]] std::string recordText(const ShotRecord& shot) noexcept
{
    char text[320];
    int length = std::snprintf(text, sizeof(text), "запись %s, бт %dт, хш %d%%, скорость %.0f, чок %d",
        kindNames[static_cast<int>(shot.kind)], shot.backtrackTicks, shot.hitchance, shot.enemySpeed, shot.enemyChoke);
    if ((shot.kind == RecordKind::Live || shot.kind == RecordKind::Ahead || shot.kind == RecordKind::Extrapolated) && length > 0 && length < static_cast<int>(sizeof(text)))
        std::snprintf(text + length, sizeof(text) - length, " (+%d обн., записей %d, свежей %d мс, пинг %d мс, maxunlag %d мс, наш тб %+d мс, simtime врага -%d мс)",
            shot.updatesAhead, shot.recordCount, shot.newestAgeMs, shot.pingMs, shot.maxUnlagMs, shot.ourClockMs, shot.enemyLagMs);
    return text;
}

struct LocalImpactListener : csgo::GameEventListener {
    void fireGameEvent(csgo::GameEventPOD* eventPointer) override
    {
        const auto event = csgo::GameEvent::from(retSpoofGadgets->client, eventPointer);
        if (!logEngine || !localPlayer || logEngine->getPlayerForUserID(event.getInt("userid")) != localPlayer.get().getNetworkable().index())
            return;
        if (lastShotIndex <= 0 || !logMemory || logMemory->globalVars->realtime - lastShotTime > 0.5f)
            return;
        auto& shot = shots[lastShotIndex];
        const csgo::Vector position{ event.getFloat("x"), event.getFloat("y"), event.getFloat("z") };
        // пуля даёт несколько импактов (пробития) — нужен последний, самый дальний
        if (!shot.haveImpact || (position - shot.eye).squareLength() > (shot.impact - shot.eye).squareLength()) {
            shot.impact = position;
            shot.haveImpact = true;
        }
    }
};
LocalImpactListener localImpactListener;
bool localImpactRegistered = false;

[[nodiscard]] float distanceToRay(const csgo::Vector& point, const csgo::Vector& origin, const csgo::Vector& through) noexcept
{
    auto direction = through - origin;
    const float length = direction.length();
    if (length <= 0.0f)
        return 1e9f;
    direction /= length;
    const auto offset = point - origin;
    const float along = offset.x * direction.x + offset.y * direction.y + offset.z * direction.z;
    return (offset - direction * along).length();
}

// true — промах по вине резольвера (пуля прошла через точку прицеливания)
// Предсказание «вперёд»: за 0.7 с записи врага за тот момент уже пришли — сравниваем с расчётом
struct PredictionCheck {
    const LagRecord* actual = nullptr;  // запись с предсказанным simtime
    bool matched = false;               // пуля попала бы в одну из пришедших записей
    int matchedOffset = 0;              // в какую: на сколько обновлений от предсказанной
};

PredictionCheck checkPrediction(const ShotRecord& shot, int index) noexcept
{
    PredictionCheck check;
    const auto& list = records[index];
    const float interval = logMemory->globalVars->intervalPerTick;
    for (int age = 0; age < list.count; ++age) {
        const auto& record = list.at(age);
        if (std::abs(record.simulationTime - shot.predictedSimulationTime) <= interval * 2.5f
            && (!check.actual || std::abs(record.simulationTime - shot.predictedSimulationTime) < std::abs(check.actual->simulationTime - shot.predictedSimulationTime)))
            check.actual = &record;
    }
    // В какую из пришедших потом записей пуля попала бы: если не в предсказанную, а на N обновлений
    // раньше/позже — сервер брал другой момент, и поправка aheadBias шагает в ту сторону
    if (shot.kind == RecordKind::Ahead && shot.haveImpact && shot.set && shot.updateTime > 0.0f) {
        static Capsules test;
        float closest = 1e9f;
        for (int age = 0; age < list.count; ++age) {
            const auto& record = list.at(age);
            const float offset = (record.simulationTime - shot.predictedSimulationTime) / shot.updateTime;
            if (std::abs(offset) > 3.5f || std::abs(offset) >= std::abs(closest))
                continue;
            const csgo::matrix3x4* variants[]{ record.bones, record.plus, record.minus };
            for (int v = 0; v < (record.rebuilt ? 3 : 1); ++v) {
                buildCapsules(*shot.set, variants[v], test);
                if (rayHitsPlayer(shot.eye, shot.impact, test)) {
                    closest = offset;
                    break;
                }
            }
        }
        if (closest < 1e8f) {
            check.matched = true;
            check.matchedOffset = static_cast<int>(std::lround(closest));
            if (check.matchedOffset != 0)
                aheadBias = std::clamp(aheadBias + (check.matchedOffset > 0 ? 0.34f : -0.34f), -2.0f, 3.0f);
        }
    }
    return check;
}

// true — промах по вине резольвера (пуля прошла через точку, а модель была повёрнута иначе)
bool logMiss(int index, float shotTime) noexcept
{
    const auto& shot = shots[index];
    PredictionCheck check;
    if (shot.predicted && logMemory)
        check = checkPrediction(shot, index);
    const float positionError = check.actual ? (check.actual->origin - shot.predictedOrigin).length() : 0.0f;

    const char* reason;
    bool resolverFault = false;
    if (!shot.haveImpact) {
        reason = localDiedAt >= shotTime - 0.05f
            ? "death (нас убили раньше, чем сервер принял выстрел)"
            : "нет импакта (сервер не засчитал выстрел — пинг/лаг)";
    } else if ((shot.impact - shot.eye).length() + 8.0f < (shot.aimPoint - shot.eye).length()) {
        reason = "пуля застряла раньше цели (автоволл ошибся)";
    } else if (shot.capsules.count ? !rayHitsPlayer(shot.eye, shot.impact, shot.capsules) : distanceToRay(shot.aimPoint, shot.eye, shot.impact) > 3.0f) {
        // при хитшансе 100% пуля почти всегда задевает модель и мимо точки — это не разброс, а модель не там
        reason = "разброс (пуля прошла мимо нашей модели)";
    } else if (shot.kind == RecordKind::Extrapolated) {
        reason = "prediction error (ломал лагкомп, экстраполяция не угадала, где он на сервере)";
    } else if (shot.kind == RecordKind::Ahead) {
        if (check.matched && check.matchedOffset != 0) {
            reason = "prediction error (сервер взял другое обновление врага)";
        } else if (check.actual && positionError > 4.0f) {
            reason = "prediction error (враг сменил движение)";
        } else if (check.matched) {
            // время и место угаданы, пуля прошла сквозь запись — модель была повёрнута не так
            resolverFault = true;
            reason = "резольвер (движение угадано, модель на сервере повёрнута иначе)";
        } else if (!check.actual && records[index].count == 0) {
            // записи сбрасываются, когда умираем мы или он (дормант) — сверять не с чем, это не ошибка предсказания
            reason = localDiedAt >= shotTime - 0.05f
                ? "не проверить (нас убили сразу после выстрела, записей нет)"
                : "не проверить (враг пропал/умер, записей нет)";
        } else {
            reason = "prediction error (модели на сервере не было ни в одном соседнем обновлении)";
        }
    } else if ((resolverFault = true), shot.side != 0.0f || shot.safe || shot.mode == LayerMode) {
        reason = "резольвер (пуля прошла через точку, а модель на сервере была не там)";
    } else {
        reason = "десинк/лагкомп (пуля прошла через точку, резольвер не применялся)";
    }
    char text[640];
    std::snprintf(text, sizeof(text), "[ECLIPSE] промах по %s: %s | %s%s, сторона %s (%s)%s, урон %d | %s",
        playerName(index).c_str(), reason, shot.isHead ? "голова" : "тело", shot.edgePoint ? " (край)" : " (центр)", sideText(shot.side),
        modeText(shot.mode), shot.safe ? ", safe" : "", shot.damage, recordText(shot).c_str());
    static constexpr std::uint8_t red[4]{ 255, 90, 90, 255 };
    consoleLog(red, text);
    {
        std::string brief = "промах по " + playerName(index) + ": ";
        const std::string_view full{ reason };
        brief += full.substr(0, full.find(" ("));
        brief += shot.isHead ? " (голова)" : " (тело)";
        screenLog(IM_COL32(255, 100, 100, 255), std::move(brief));
    }
    if (shot.mode == LayerMode && hvh::resolver::statusLine(index)[0]) {
        char layers[256];
        std::snprintf(layers, sizeof(layers), "[ECLIPSE]   резольвер по слоям: %s", hvh::resolver::statusLine(index));
        static constexpr std::uint8_t grey[4]{ 190, 190, 190, 255 };
        consoleLog(grey, layers);
    }

    if (shot.predicted && logMemory && shot.haveImpact) {
        char match[96] = "";
        if (shot.kind == RecordKind::Ahead) {
            if (check.matched)
                std::snprintf(match, sizeof(match), "; пуля попала бы в запись %+d обн., поправка %+d", check.matchedOffset, static_cast<int>(std::lround(aheadBias)));
            else
                std::snprintf(match, sizeof(match), "; ни в одну соседнюю запись");
        }
        char line[288];
        if (check.actual) {
            std::snprintf(line, sizeof(line), "[ECLIPSE]   проверка предсказания: ошибка %.1f ед. (по высоте %+.1f), взгляд повернулся на %+.0f°, чок %d%s",
                positionError, check.actual->origin.z - shot.predictedOrigin.z,
                std::remainder(check.actual->eyeAngles.y - shot.usedEyeYaw, 360.0f), check.actual->choked, match);
        } else {
            std::snprintf(line, sizeof(line), "[ECLIPSE]   проверка предсказания: записи с этим временем не пришло (сдвиг тикбейса/дормант, записей %d)%s", records[index].count, match);
        }
        static constexpr std::uint8_t orange[4]{ 255, 160, 90, 255 };
        consoleLog(orange, line);
    }
    return resolverFault;
}

struct AimPoint {
    csgo::Vector position{};
    float damage = 0.0f;        // с множителем хитгруппы и бронёй
    float baseDamage = 0.0f;    // долетает до точки без множителя — для хитшанса
    bool isHead = false;
    bool edge = false;          // точка мультипоинта, а не центр хитбокса
};

csgo::Vector cross(const csgo::Vector& a, const csgo::Vector& b) noexcept
{
    return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x };
}

csgo::Vector normalized(csgo::Vector v) noexcept
{
    const float length = v.length();
    return length > 0.0f ? v / length : v;
}

WeaponGroup groupOf(const csgo::Entity& weapon) noexcept
{
    switch (weapon.itemDefinitionIndex()) {
    case WeaponId::Deagle:
    case WeaponId::Revolver: return WeaponGroup::Heavy;
    case WeaponId::Ssg08: return WeaponGroup::Scout;
    case WeaponId::Awp: return WeaponGroup::Awp;
    case WeaponId::Scar20:
    case WeaponId::G3SG1: return WeaponGroup::Auto;
    default: break;
    }
    switch (weapon.getWeaponType()) {
    case WeaponType::Pistol: return WeaponGroup::Pistol;
    case WeaponType::Rifle: return WeaponGroup::Rifle;
    case WeaponType::SubMachinegun: return WeaponGroup::Smg;
    default: return WeaponGroup::General;
    }
}

// хитбоксы модели по группам из меню
constexpr std::array<std::pair<int, std::array<Hitbox, 4>>, 7> hitboxGroups{ {
    { HB_Head, { Hitbox::Head, Hitbox::Max, Hitbox::Max, Hitbox::Max } },
    { HB_Chest, { Hitbox::UpperChest, Hitbox::Thorax, Hitbox::LowerChest, Hitbox::Max } },
    { HB_Stomach, { Hitbox::Belly, Hitbox::Max, Hitbox::Max, Hitbox::Max } },
    { HB_Pelvis, { Hitbox::Pelvis, Hitbox::Max, Hitbox::Max, Hitbox::Max } },
    { HB_Arms, { Hitbox::RightUpperArm, Hitbox::LeftUpperArm, Hitbox::RightForearm, Hitbox::LeftForearm } },
    { HB_Legs, { Hitbox::RightThigh, Hitbox::LeftThigh, Hitbox::RightCalf, Hitbox::LeftCalf } },
    { HB_Feet, { Hitbox::RightFoot, Hitbox::LeftFoot, Hitbox::Max, Hitbox::Max } },
} };

// кратчайшее расстояние между отрезками p1-q1 и p2-q2; s — где на первом отрезке ближайшая точка (0..1)
float segmentDistance(const csgo::Vector& p1, const csgo::Vector& q1, const csgo::Vector& p2, const csgo::Vector& q2, float& s) noexcept
{
    const auto d1 = q1 - p1;
    const auto d2 = q2 - p2;
    const auto r = p1 - p2;
    const float a = d1.dotProduct(d1);
    const float e = d2.dotProduct(d2);
    const float f = d2.dotProduct(r);

    float t = 0.0f;
    s = 0.0f;
    if (a <= 1e-6f && e <= 1e-6f)
        return r.length();
    if (a <= 1e-6f) {
        t = std::clamp(f / e, 0.0f, 1.0f);
    } else {
        const float c = d1.dotProduct(r);
        if (e <= 1e-6f) {
            s = std::clamp(-c / a, 0.0f, 1.0f);
        } else {
            const float b = d1.dotProduct(d2);
            const float denom = a * e - b * b;
            s = denom != 0.0f ? std::clamp((b * f - c * e) / denom, 0.0f, 1.0f) : 0.0f;
            t = (b * s + f) / e;
            if (t < 0.0f) {
                t = 0.0f;
                s = std::clamp(-c / a, 0.0f, 1.0f);
            } else if (t > 1.0f) {
                t = 1.0f;
                s = std::clamp((b - c) / a, 0.0f, 1.0f);
            }
        }
    }
    return ((p1 + d1 * s) - (p2 + d2 * t)).length();
}

void buildCapsules(const csgo::StudioHitboxSet& set, const csgo::matrix3x4* bones, Capsules& out) noexcept
{
    out.count = 0;
    for (int i = 0; i < set.numHitboxes && out.count < static_cast<int>(out.items.size()); ++i) {
        const auto hitbox = const_cast<csgo::StudioHitboxSet&>(set).getHitbox(i);
        if (!hitbox || hitbox->capsuleRadius <= 0.0f || hitbox->bone < 0 || hitbox->bone >= RECORD_BONES)
            continue;
        out.items[out.count++] = { transform(hitbox->bbMin, bones[hitbox->bone]), transform(hitbox->bbMax, bones[hitbox->bone]), hitbox->capsuleRadius, hitbox->group };
    }
}

// первый хитбокс на луче start -> end (хитгруппа), -1 — луч мимо модели
int firstHitGroup(const Capsules& capsules, const csgo::Vector& start, const csgo::Vector& end) noexcept
{
    const float length = (end - start).length();
    float nearest = 1e9f;
    int group = -1;
    for (int i = 0; i < capsules.count; ++i) {
        const auto& capsule = capsules.items[i];
        float s;
        const float distance = segmentDistance(start, end, capsule.a, capsule.b, s);
        if (distance > capsule.radius)
            continue;
        // где луч входит в капсулу: от ближайшей точки назад на полхорды
        const float entry = s * length - std::sqrt(std::max(capsule.radius * capsule.radius - distance * distance, 0.0f));
        if (entry < nearest) {
            nearest = entry;
            group = capsule.group;
        }
    }
    return group;
}

bool rayHitsPlayer(const csgo::Vector& start, const csgo::Vector& end, const Capsules& capsules) noexcept
{
    for (int i = 0; i < capsules.count; ++i) {
        float s;
        if (segmentDistance(start, end, capsules.items[i].a, capsules.items[i].b, s) <= capsules.items[i].radius)
            return true;
    }
    return false;
}

// ---------- разброс: тот же генератор, что у игры ----------
// Копия CUniformRandomStream из vstdlib (RandomSeed/RandomFloat): проверено офлайн против
// libvstdlib_client.so — 1024 из 1024 чисел совпали. Своя копия, чтобы не сбивать глобальный генератор игры.
struct UniformRandomStream {
    int idum = 0;
    int iy = 0;
    int iv[32]{};

    void seed(int value) noexcept
    {
        idum = value < 0 ? value : -value;
        iy = 0;
    }

    int next() noexcept
    {
        constexpr int IA = 16807, IM = 2147483647, IQ = 127773, IR = 2836, NTAB = 32, NDIV = 1 + (IM - 1) / NTAB;
        int j, k;
        if (idum <= 0 || !iy) {
            idum = -idum < 1 ? 1 : -idum;
            for (j = NTAB + 7; j >= 0; --j) {
                k = idum / IQ;
                idum = IA * (idum - k * IQ) - IR * k;
                if (idum < 0)
                    idum += IM;
                if (j < NTAB)
                    iv[j] = idum;
            }
            iy = iv[0];
        }
        k = idum / IQ;
        idum = IA * (idum - k * IQ) - IR * k;
        if (idum < 0)
            idum += IM;
        j = iy / NDIV;
        if (j >= NTAB || j < 0)
            j = (j % NTAB) & 0x7fffffff;
        iy = iv[j];
        iv[j] = idum;
        return iy;
    }

    float randomFloat(float low, float high) noexcept
    {
        float value = static_cast<float>((1.0 / 2147483647) * next());
        if (value > 1.0f - 1.2e-7f)
            value = 1.0f - 1.2e-7f;
        return value * (high - low) + low;
    }
};

// FX_FireBullets: RandomSeed(seed); a = RandomFloat(0,1); b = RandomFloat(0,2π); c = RandomFloat(0,1); d = RandomFloat(0,2π)
// отклонение = a*inaccuracy по углу b + c*spread по углу d
struct SpreadSeed {
    float a, cosB, sinB;
    float c, cosD, sinD;
};

const std::array<SpreadSeed, HITCHANCE_SEEDS>& spreadSeeds() noexcept
{
    static const auto table = [] {
        std::array<SpreadSeed, HITCHANCE_SEEDS> result{};
        UniformRandomStream stream;
        for (int i = 0; i < HITCHANCE_SEEDS; ++i) {
            stream.seed(i + 1);
            const float a = stream.randomFloat(0.0f, 1.0f);
            const float b = stream.randomFloat(0.0f, 2.0f * 3.14159265f);
            const float c = stream.randomFloat(0.0f, 1.0f);
            const float d = stream.randomFloat(0.0f, 2.0f * 3.14159265f);
            result[i] = { a, std::cos(b), std::sin(b), c, std::cos(d), std::sin(d) };
        }
        return result;
    }();
    return table;
}

// Хитшанс: % сидов из 256, при которых пуля по скелету выбранной записи даст не меньше minDamage.
// Первый хитбокс на луче решает, куда попадём: «целился в голову, попал в грудь» тоже считается,
// если урона в грудь хватает, а в ногу при мин. уроне 50 — уже нет.
int hitchance(const csgo::Entity& weapon, const csgo::WeaponInfo* weaponData, const csgo::Entity& entity, const csgo::Vector& eye, const csgo::Vector& angle,
              const Capsules& capsules, float baseDamage, int minDamage) noexcept
{
    const float inaccuracy = weapon.getInaccuracy();
    float spread = weapon.weaponMode() == 1 ? weaponData->spreadAlt : weaponData->spread;
    // офсет spread взят из раскладки CCSWeaponInfo: если там мусор — считаем без него, но не врём в плюс
    if (!(spread >= 0.0f && spread < 0.2f))
        spread = 0.0f;
    const bool negev = weapon.itemDefinitionIndex() == WeaponId::Negev;
    const int recoilIndex = weapon.recoilIndex();

    const float sp = std::sin(Helpers::deg2rad(angle.x)), cp = std::cos(Helpers::deg2rad(angle.x));
    const float sy = std::sin(Helpers::deg2rad(angle.y)), cy = std::cos(Helpers::deg2rad(angle.y));
    const csgo::Vector forward{ cp * cy, cp * sy, -sp };
    const csgo::Vector right{ sy, -cy, 0.0f };
    const csgo::Vector up{ sp * cy, sp * sy, cp };

    // урон по каждой хитгруппе считаем один раз
    std::array<bool, 11> enough{};
    for (int group = 0; group < static_cast<int>(enough.size()); ++group)
        enough[group] = autowall::scaleDamage(entity, group, weaponData, baseDamage) >= static_cast<float>(minDamage);

    int hits = 0;
    for (const auto& seed : spreadSeeds()) {
        float a = seed.a, c = seed.c;
        // негев первые выстрелы кладёт кучнее (как в FX_FireBullets)
        if (negev && recoilIndex < 3) {
            for (int j = 3; j > recoilIndex; --j) {
                a *= a;
                c *= c;
            }
            a = 1.0f - a;
            c = 1.0f - c;
        }
        const float x = seed.cosB * a * inaccuracy + seed.cosD * c * spread;
        const float y = seed.sinB * a * inaccuracy + seed.sinD * c * spread;
        const auto direction = normalized(forward + right * x + up * y);
        const int group = firstHitGroup(capsules, eye, eye + direction * weaponData->range);
        if (group >= 0 && (group >= static_cast<int>(enough.size()) || enough[group]))
            ++hits;
    }
    return hits * 100 / HITCHANCE_SEEDS;
}

struct MovementVars {
    float friction = 5.2f;
    float stopSpeed = 80.0f;
    float accelerate = 5.5f;
};

MovementVars movementVars(const csgo::Cvar& cvar) noexcept
{
    static const auto frictionVar = cvar.findVar("sv_friction");
    static const auto stopSpeedVar = cvar.findVar("sv_stopspeed");
    static const auto accelerateVar = cvar.findVar("sv_accelerate");
    MovementVars vars;
    if (frictionVar)
        vars.friction = csgo::ConVar::from(retSpoofGadgets->client, frictionVar).getFloat();
    if (stopSpeedVar)
        vars.stopSpeed = csgo::ConVar::from(retSpoofGadgets->client, stopSpeedVar).getFloat();
    if (accelerateVar)
        vars.accelerate = csgo::ConVar::from(retSpoofGadgets->client, accelerateVar).getFloat();
    return vars;
}

// «Точность»: разброс в CS:GO не растёт, пока скорость ниже ~34% максимальной
[[nodiscard]] float autoStopTarget(AutoStop mode, float maxSpeed) noexcept
{
    return mode == AutoStop::Accuracy ? maxSpeed * 0.33f : 0.0f;
}

// сколько скорости съест трение за тик (CGameMovement::Friction)
[[nodiscard]] float afterFriction(float speed, const MovementVars& vars, float interval) noexcept
{
    return std::max(0.0f, speed - std::max(speed, vars.stopSpeed) * vars.friction * interval);
}

// За сколько тиков трение + полный встречный ввод погасят скорость до цели: столько тиков до готовности
// оружия и начинаем тормозить (с 250 ед/с до нуля — около 6 тиков, «за тик» не успеть)
[[nodiscard]] int ticksToStop(float speed, float targetSpeed, float maxSpeed, const MovementVars& vars, float interval) noexcept
{
    int ticks = 0;
    while (speed > targetSpeed + 1.0f && ticks < 16) {
        speed = std::max(targetSpeed, afterFriction(speed, vars, interval) - vars.accelerate * interval * maxSpeed);
        ++ticks;
    }
    return ticks;
}

// Автостоп с предсказанием: сколько скорости съест трение на следующем тике
// и какой встречный ввод нужен, чтобы прийти ровно к цели, а не улететь в обратную сторону
void autoStop(csgo::UserCmd* cmd, AutoStop mode, float maxSpeed, float interval, const MovementVars& vars) noexcept
{
    const auto velocity = localPlayer.get().velocity();
    const float speed = velocity.length2D();

    const float targetSpeed = autoStopTarget(mode, maxSpeed);
    const float nextSpeed = afterFriction(speed, vars, interval);

    if (nextSpeed <= targetSpeed + 1.0f) {
        if (mode == AutoStop::Full || speed < 15.0f) {
            cmd->forwardmove = 0.0f;
            cmd->sidemove = 0.0f;
            return;
        }
        // одного трения хватает — ограничиваем ввод так, чтобы не разогнаться обратно
        const float move = std::hypot(cmd->forwardmove, cmd->sidemove);
        if (move > targetSpeed && move > 0.0f) {
            cmd->forwardmove *= targetSpeed / move;
            cmd->sidemove *= targetSpeed / move;
        }
        return;
    }

    // Accelerate: прибавка за тик = sv_accelerate * interval * wishspeed — берём такой wishspeed,
    // чтобы погасить ровно остаток (больше — перелёт назад и снова разброс)
    const float wish = std::clamp((nextSpeed - targetSpeed) / std::max(vars.accelerate * interval, 0.001f), 0.0f, 450.0f);
    const float relative = Helpers::deg2rad(velocity.toAngle().y - cmd->viewangles.y);
    cmd->forwardmove = -std::cos(relative) * wish;
    cmd->sidemove = std::sin(relative) * wish;
}

}

namespace
{

// Резольвер десинка. Клиент рисует десинкера с ногами там, где их считает сам, а на сервере
// тело повёрнуто до maxDesync в сторону от взгляда. Строим две версии скелета — ноги под углом
// взгляд + maxDesync и взгляд - maxDesync (поворот костей вокруг оси игрока).
// Дальше: «безопасная точка» — та, что попадает во все три версии; если её нет — бьём по угаданной
// стороне, а сторону перебираем по промахам.
bool buildDesyncVariants(bool isBot, const LagRecord& record, const csgo::matrix3x4* client, csgo::matrix3x4* plus, csgo::matrix3x4* minus, float deltaScale) noexcept
{
    // боты не десинкают
    if (isBot)
        return false;

    // полный десинк — скелеты, собранные анимацией при записи; low delta и запасной путь — поворот костей
    if (record.rebuilt && deltaScale == 1.0f) {
        std::copy(record.plus, record.plus + RECORD_BONES, plus);
        std::copy(record.minus, record.minus + RECORD_BONES, minus);
        return true;
    }

    const float eyeYaw = record.eyeAngles.y;
    const float clientYaw = record.absYaw;
    const auto& pivot = record.origin;
    const float maxDesync = deltaScale * maxDesyncFor(record.velocity);

    // На сервере на полный угол повёрнуты ноги и таз (feet yaw), а позвоночник закручен обратно к взгляду
    // (body_yaw), поэтому чем выше кость, тем меньше её доворот: ступни — 100%, голова — ~35%.
    const float delta = std::remainder(eyeYaw - clientYaw, 360.0f);
    const auto rotated = [&](csgo::matrix3x4* out, float sign) {
        const float target = delta + sign * maxDesync;
        for (int bone = 0; bone < RECORD_BONES; ++bone) {
            out[bone] = client[bone];
            const float height = std::clamp((client[bone].origin().z - pivot.z) / 64.0f, 0.0f, 1.0f);
            // у клиента верх тела уже довёрнут к взгляду своим body_yaw — по высоте уменьшаем весь поворот ног
            const float radians = Helpers::deg2rad(target * (1.0f - 0.65f * height));
            out[bone].rotateYaw(pivot, std::cos(radians), std::sin(radians));
        }
    };
    rotated(plus, 1.0f);
    rotated(minus, -1.0f);
    return true;
}

// Фристенд-догадка: десинкеры прячут настоящую голову — за стену, а в открытую — подальше от стрелка.
// +1 — настоящее тело на стороне «взгляд + десинк», -1 — «взгляд - десинк».
float guessDesyncSide(const csgo::EngineTrace& engineTrace, const csgo::Vector& eye, const csgo::matrix3x4* plus, const csgo::matrix3x4* minus) noexcept
{
    constexpr int headBone = 8;
    const auto headPlus = plus[headBone].origin();
    const auto headMinus = minus[headBone].origin();
    const auto exposed = [&](const csgo::Vector& point) {
        // только стены (MASK_SOLID_BRUSHONLY): игроки луч не загораживают
        csgo::Trace trace;
        engineTrace.traceRay({ eye, point }, 0x1400B, { localPlayer.get().getPOD() }, trace);
        return trace.fraction > 0.97f;
    };
    const bool plusExposed = exposed(headPlus);
    const bool minusExposed = exposed(headMinus);
    if (plusExposed != minusExposed)
        return plusExposed ? -1.0f : 1.0f;
    return (headPlus - eye).squareLength() > (headMinus - eye).squareLength() ? 1.0f : -1.0f;
}

}

float resolverOffsetFor(int index) noexcept
{
    return index > 0 && index < static_cast<int>(resolverOffset.size()) ? resolverOffset[index] : 0.0f;
}

bool minDamageOverrideActive() noexcept { return minDamageOverride; }
const char* status() noexcept { return statusText; }

void onPlayerHurt(int victimIndex, int attackerIndex, int localIndex, int hitgroup, int damage, int healthLeft) noexcept
{
    if (attackerIndex != localIndex || victimIndex <= 0 || victimIndex >= static_cast<int>(misses.size()))
        return;
    if (pendingShot[victimIndex] > 0.0f) {
        static constexpr const char* hitgroups[]{ "тело", "голова", "грудь", "живот", "левая рука", "правая рука", "левая нога", "правая нога", "шея" };
        const auto& shot = shots[victimIndex];
        char text[448];
        std::snprintf(text, sizeof(text), "[ECLIPSE] попал в %s: %s -%d (осталось %d) | целился в %s, сторона %s (%s)%s | %s",
            playerName(victimIndex).c_str(), hitgroup >= 0 && hitgroup < 9 ? hitgroups[hitgroup] : "?", damage, healthLeft,
            shot.isHead ? "голову" : "тело", sideText(shot.side), modeText(shot.mode), shot.safe ? ", safe" : "", recordText(shot).c_str());
        static constexpr std::uint8_t green[4]{ 120, 230, 120, 255 };
        consoleLog(green, text);
        char brief[160];
        std::snprintf(brief, sizeof(brief), "попал в %s: %s -%d (осталось %d)", playerName(victimIndex).c_str(),
            hitgroup >= 0 && hitgroup < 9 ? hitgroups[hitgroup] : "?", damage, healthLeft);
        screenLog(IM_COL32(120, 230, 120, 255), brief);
    }
    misses[victimIndex] = 0;
    // попали — этот режим резольвера хорош для этого игрока
    if (pendingShot[victimIndex] > 0.0f) {
        const int mode = shots[victimIndex].mode;
        if (mode >= 0 && mode < ModeCount)
            resolverStates[victimIndex].score[mode] += 2;
        else if (mode == LayerMode)
            hvh::resolver::onHit(victimIndex);
        else if (mode == MemoryMode)
            resolverStates[victimIndex].memoryMisses = 0;

        auto& state = resolverStates[victimIndex];
        state.bruteSide = 0.0f;
        state.bruteUntil = 0.0f;
        // джиттер: учим, совпадает ли сторона попадания с фазой взгляда. Находок по слоям мало,
        // а попаданий больше — связь фазы и стороны так выучивается быстрее
        if (isJittering(state) && std::abs(shots[victimIndex].side) > 0.05f) {
            const bool samePhase = (shots[victimIndex].side > 0.0f) == (state.jitterPhase > 0.0f);
            state.jitterSideBits = static_cast<std::uint8_t>((state.jitterSideBits << 1) | (samePhase ? 1 : 0));
            state.jitterSideCount = static_cast<std::uint8_t>(std::min<int>(state.jitterSideCount + 1, 8));
        }
    }
    pendingShot[victimIndex] = 0.0f;
}

void updateEventListener(const EngineInterfaces& engineInterfaces, const Memory& memory, bool forceRemove) noexcept
{
    const bool wanted = !forceRemove && config.rage.enabled;
    if (wanted && !localImpactRegistered) {
        engineInterfaces.getGameEventManager(memory.getEventDescriptor).addListener(&localImpactListener, csgo::bullet_impact);
        localImpactRegistered = true;
    } else if (!wanted && localImpactRegistered) {
        engineInterfaces.getGameEventManager(memory.getEventDescriptor).removeListener(&localImpactListener);
        localImpactRegistered = false;
    }
}

void captureNetworkAngles(const EngineInterfaces& engineInterfaces, const ClientInterfaces& clientInterfaces) noexcept
{
    if (!config.rage.enabled)
        return;
    const int maxClients = std::min(engineInterfaces.getEngine().getMaxClients(), static_cast<int>(networkAngles.size()) - 1);
    for (int i = 1; i <= maxClients; ++i) {
        const auto entity = csgo::Entity::from(retSpoofGadgets->client, clientInterfaces.getEntityList().getEntity(i));
        if (entity.getPOD() == nullptr || entity.getNetworkable().isDormant()) {
            networkAngles[i].simulationTime = -1.0f;
            continue;
        }
        // simtime не сдвинулся — пакета на этого игрока не было, в поле лежит интерполированный угол
        if (entity.simulationTime() == networkAngles[i].simulationTime)
            continue;
        networkAngles[i] = { entity.simulationTime(), entity.eyeAngles() };
    }
}

void drawLogs(ImDrawList* drawList) noexcept
{
    if (!config.view.screenLogs || !drawList)
        return;
    constexpr double lifetime = 6.0, fade = 1.0;
    const double now = steadySeconds();
    ImVec2 position{ 8.0f, 6.0f };
    // от старых к новым: новая строка снизу
    for (int k = 0; k < static_cast<int>(screenLines.size()); ++k) {
        const auto& line = screenLines[(screenNext + k) % screenLines.size()];
        const double age = now - line.time;
        if (line.text.empty() || age > lifetime)
            continue;
        const float alpha = static_cast<float>(std::clamp((lifetime - age) / fade, 0.0, 1.0));
        const auto withAlpha = [alpha](ImU32 color) {
            return (color & 0x00FFFFFF) | (static_cast<ImU32>(((color >> 24) & 0xFF) * alpha) << 24);
        };
        drawList->AddText({ position.x + 1.0f, position.y + 1.0f }, withAlpha(IM_COL32(0, 0, 0, 200)), line.text.c_str());
        drawList->AddText(position, withAlpha(line.color), line.text.c_str());
        position.y += ImGui::GetTextLineHeight() + 2.0f;
    }
}

void logToFile(const char* text) noexcept
{
    fileLog(text);
}

void onPlayerDeath(int victimIndex, int localIndex) noexcept
{
    if (victimIndex == localIndex && logMemory)
        localDiedAt = logMemory->globalVars->realtime;
}

void onRoundStart() noexcept
{
    misses.fill(0);
    pendingShot.fill(0.0f);
    hvh::resolver::onRoundStart();
}
bool forceBodyActive() noexcept { return forceBody; }
int currentTarget() noexcept { return target; }

namespace
{
bool runImpl(csgo::UserCmd* cmd, const EngineInterfaces& engineInterfaces, const ClientInterfaces& clientInterfaces, const OtherInterfaces& interfaces, const Memory& memory, float lerp) noexcept;
}

bool run(csgo::UserCmd* cmd, const EngineInterfaces& engineInterfaces, const ClientInterfaces& clientInterfaces, const OtherInterfaces& interfaces, const Memory& memory, float lerp) noexcept
{
    const bool shot = runImpl(cmd, engineInterfaces, clientInterfaces, interfaces, memory, lerp);

    // «Фотка»: оружие готово, цель есть, а выстрела нет дольше секунды — пишем причину в файл (в консоль нет)
    static float stuckSince = -1.0f;
    static float lastWritten = -10.0f;
    const std::string_view status{ statusText };
    const bool waiting = status.starts_with("хитшанс") || status == "мало урона" || status == "нет безопасных точек";
    const float now = memory.globalVars->realtime;
    if (shot || !waiting) {
        stuckSince = -1.0f;
    } else if (stuckSince < 0.0f) {
        stuckSince = now;
    } else if (now - stuckSince > 1.0f && now - lastWritten > 2.0f) {
        lastWritten = now;
        char text[192];
        std::snprintf(text, sizeof(text), "[ECLIPSE] не стреляю %.1f с: %s%s%s", now - stuckSince, statusText,
            target ? ", цель " : "", target ? playerName(target).c_str() : "");
        fileLog(text);
    }
    return shot;
}

namespace
{

bool runImpl(csgo::UserCmd* cmd, const EngineInterfaces& engineInterfaces, const ClientInterfaces& clientInterfaces, const OtherInterfaces& interfaces, const Memory& memory, float lerp) noexcept
{
    const auto& rage = config.rage;
    logMemory = &memory;
    logEngine.emplace(engineInterfaces.getEngine());
    minDamageOverride = rage.minDamageOverrideKey.isSet() && rage.minDamageOverrideKey.isDown();
    forceBody = rage.forceBodyKey.isSet() && rage.forceBodyKey.isDown();
    target = 0;
    statusText = "";

    for (std::size_t i = 0; i < pendingShot.size(); ++i) {
        if (pendingShot[i] > 0.0f && memory.globalVars->realtime - pendingShot[i] > 0.7f) {
            const bool resolverFault = logMiss(static_cast<int>(i), pendingShot[i]);
            ++misses[i];
            // штрафуем режим только за промах по его вине (разброс/стена/лаг — не он)
            const int mode = shots[i].mode;
            if (resolverFault && mode >= 0 && mode < ModeCount) {
                --resolverStates[i].score[mode];
                // и навязываем зеркальную сторону на следующие ~1.5 с, пока не попадём
                const float used = shots[i].side;
                if (std::abs(used) > 0.05f) {
                    auto& state = resolverStates[i];
                    state.bruteSide = used > 0.0f ? -1.0f : 1.0f;
                    state.bruteUntil = memory.globalVars->realtime + 1.5f;
                }
            }
            else if (resolverFault && mode == LayerMode)
                hvh::resolver::onMiss(static_cast<int>(i));
            else if (resolverFault && mode == MemoryMode) {
                // враг переключил сторону (инвертер) — пробуем зеркальную, второй промах — память выключаем до новой находки
                auto& state = resolverStates[i];
                state.memoryRatio = -state.memoryRatio;
                ++state.memoryMisses;
            }
            else if (resolverFault && mode == -1)
                resolverStates[i].chokeHistory = 0xFF;  // считали, что не десинкает, а пуля прошла сквозь — значит, десинкает
            pendingShot[i] = 0.0f;
        }
    }

    if (!rage.enabled || !localPlayer || !localPlayer.get().isAlive()) {
        for (auto& list : records)
            list.clear();
        hvh::resolver::clear();
        return false;
    }

    const float interval = memory.globalVars->intervalPerTick;
    const int maxClients = std::min(engineInterfaces.getEngine().getMaxClients(), 64);

    // записи лагкомпа и резольвер обновляем каждый тик, даже когда стрелять нечем
    std::array<bool, 65> isBot{};
    for (int i = 1; i <= maxClients; ++i) {
        const auto entity = csgo::Entity::from(retSpoofGadgets->client, clientInterfaces.getEntityList().getEntity(i));
        if (entity.getPOD() == nullptr || entity.getPOD() == localPlayer.get().getPOD() || entity.getNetworkable().isDormant()
            || !entity.isAlive() || !entity.isOtherEnemy(memory, localPlayer.get())) {
            records[i].clear();
            continue;
        }
        csgo::PlayerInfo playerInfo{};
        const bool haveInfo = engineInterfaces.getEngine().getPlayerInfo(i, playerInfo);
        isBot[i] = haveInfo && playerInfo.fakeplayer;
        trackPlayer(resolverStates[i], entity, i, haveInfo ? playerInfo.userId : -1, interval, memory.globalVars->realtime);
        updateRecords(records[i], entity, memory, rage.resolver && !isBot[i], i);
    }

    if (localPlayer.get().isDefusing())
        return false;

    const auto weapon = csgo::Entity::from(retSpoofGadgets->client, localPlayer.get().getActiveWeapon());
    if (!weapon.getPOD() || weapon.clip() <= 0)
        return false;

    const auto weaponType = weapon.getWeaponType();
    if (weaponType == WeaponType::Knife || weaponType == WeaponType::Grenade || weaponType == WeaponType::C4 || weapon.itemDefinitionIndex() == WeaponId::Revolver)
        return false;

    const auto weaponData = weapon.getWeaponData();
    if (!weaponData)
        return false;

    if (!rage.autoFire && !(cmd->buttons & csgo::UserCmd::IN_ATTACK))
        return false;

    const auto& settings = weaponSettings(groupOf(weapon));
    const float serverTime = memory.globalVars->serverTime();
    // Во время сдвига DT команды идут пачкой без предсказания между ними: nextPrimaryAttack оружия ещё старый,
    // и без этой проверки рейдж «стрелял» в каждой из прогоняемых команд, затирая данные настоящего выстрела
    static float firedAt = -1.0f;
    static float firedCycle = 0.0f;
    // вне сдвига верим предсказанию игры: если сервер выстрел не принял, ждать 1.25 с скаута незачем
    const bool cycling = tickbase::isShifting() && firedAt >= 0.0f && serverTime >= firedAt && serverTime < firedAt + firedCycle;
    const bool canShoot = localPlayer.get().nextAttack() <= serverTime && weapon.nextPrimaryAttack() <= serverTime && !cycling;
    const bool needScope = settings.autoScope && weapon.isSniperRifle() && !localPlayer.get().isScoped();
    // оружие будет готово раньше, чем мы успеем затормозить, — тормозим уже сейчас, к выстрелу скорость погашена
    const auto stopMode = static_cast<AutoStop>(settings.autoStop);
    const auto moveVars = movementVars(interfaces.getCvar());
    const float stopMaxSpeed = localPlayer.get().isScoped() || needScope ? weaponData->maxSpeedAlt : weaponData->maxSpeed;
    const int stopTicks = stopMode == AutoStop::Off ? 0
        : ticksToStop(localPlayer.get().velocity().length2D(), autoStopTarget(stopMode, stopMaxSpeed), stopMaxSpeed, moveVars, interval);
    const float readyIn = std::max(localPlayer.get().nextAttack(), weapon.nextPrimaryAttack()) - serverTime;
    const bool readySoon = readyIn <= interval * static_cast<float>(std::max(stopTicks, 1));

    // Между выстрелами точки не ищем: автоволл по записям — самое дорогое в чите, отсюда и фризы.
    // Если цель нужна заранее (ранний автостоп/скоуп) — только дешёвый поиск: свежая запись, центры хитбоксов.
    if (!canShoot && !readySoon && !settings.autoStopEarly && !needScope) {
        statusText = "перезарядка";
        return false;
    }
    const bool cheapScan = !canShoot && !readySoon;

    statusText = "нет целей";

    float latencyOut = 0.0f, latencyIn = 0.0f;
    if (const auto channel = engineInterfaces.getEngine().getNetworkChannel()) {
        const auto network = csgo::NetworkChannel::from(retSpoofGadgets->client, channel);
        latencyOut = network.getLatency(0);
        latencyIn = network.getLatency(1);
    }
    static const auto maxUnlagVar = interfaces.getCvar().findVar("sv_maxunlag");
    const float maxUnlag = maxUnlagVar ? csgo::ConVar::from(retSpoofGadgets->client, maxUnlagVar).getFloat() : 0.2f;
    static const auto gravityVar = interfaces.getCvar().findVar("sv_gravity");
    const float gravity = gravityVar ? csgo::ConVar::from(retSpoofGadgets->client, gravityVar).getFloat() : 800.0f;

    const auto eye = localPlayer.get().getEyePosition();
    const auto worldUp = csgo::Vector{ 0.0f, 0.0f, 1.0f };

    struct Best {
        int index = 0;
        csgo::Vector origin{};
        float eyeYaw = 0.0f;
        AimPoint point;
        AimPoint fallback;      // вторая группа (тело вместо головы и наоборот) — если по первой не хватит хитшанса
        float fov = 0.0f;
        csgo::matrix3x4 bones[RECORD_BONES];
        csgo::StudioHitboxSet* set = nullptr;
        float simulationTime = 0.0f;
        bool safe = false;
        RecordKind kind = RecordKind::Live;
        int backtrackTicks = 0;
        float speed = 0.0f;
        int choke = 0;
        int minDamage = 0;
        int recordCount = 0;
        float newestAge = 0.0f;
        int updatesAhead = 0;
    };
    // Мультипоинт по всему телу — до ~70 точек на цель, каждая это автоволл. Центры хитбоксов проверяем всегда,
    // доп. точки — пока не кончился бюджет на тик (иначе фризы при нескольких целях за стенами)
    int pointBudget = 160;

    // Три лучшие цели по урону: если по первой не набирается хитшанс, стреляем во вторую/третью, а не стоим
    static std::array<Best, 3> top;
    int topCount = 0;

    // рабочие скелеты: выбранный резольвером и три версии для безопасных точек
    static csgo::matrix3x4 bones[RECORD_BONES], plusBones[RECORD_BONES], minusBones[RECORD_BONES];
    static LagRecord liveRecord, extrapolatedRecord;
    static csgo::Vector aheadStep;
    static Capsules earlierCapsules, laterCapsules;
    static Capsules clientCapsules, plusCapsules, minusCapsules, aimCapsules;

    for (int i = 1; i <= maxClients; ++i) {
        const auto entity = csgo::Entity::from(retSpoofGadgets->client, clientInterfaces.getEntityList().getEntity(i));
        if (entity.getPOD() == nullptr || entity.getPOD() == localPlayer.get().getPOD() || entity.getNetworkable().isDormant()
            || !entity.isAlive() || !entity.isOtherEnemy(memory, localPlayer.get()) || entity.gunGameImmunity())
            continue;

        auto& state = resolverStates[i];
        resolverOffset[i] = 0.0f;
        resolverMode[i] = -1;
        // дефенсив не ждём: у хвх-игроков он почти всегда включён, рейдж бы просто не стрелял

        const auto toTarget = (entity.getAbsOrigin() - eye).toAngle();
        const float fov = std::hypot(std::remainder(toTarget.x - cmd->viewangles.x, 360.0f), std::remainder(toTarget.y - cmd->viewangles.y, 360.0f));
        if (fov > rage.fov)
            continue;

        const auto model = entity.getRenderable().getModel();
        if (!model)
            continue;
        const auto studio = engineInterfaces.getModelInfo().getStudioModel(model);
        if (!studio)
            continue;
        const auto set = studio->getHitboxSet(entity.hitboxSet());
        if (!set)
            continue;

        // ---- кандидаты-записи в порядке приоритета ----
        struct Candidate {
            const LagRecord* record;
            RecordKind kind;
            int age;
            int updatesAhead = 0;
        };
        std::array<Candidate, 4> candidates{};
        int candidateCount = 0;
        const auto& list = records[i];

        if (list.count && list.at(0).breakingLagComp) {
            // ломает лагкомп: сервер стреляет по его текущей позиции, откат не поможет.
            // Сдвигаем свежую запись вперёд по скорости на время, пока данные шли к нам и пакет идёт обратно.
            const int updates = predictAhead(list, extrapolatedRecord, memory.globalVars->realtime, latencyOut + latencyIn, interval, gravity, aheadStep);
            candidates[candidateCount++] = { &extrapolatedRecord, RecordKind::Extrapolated, 0, updates };
        } else {
            int onshot = -1, standing = -1, newest = -1, oldest = -1;
            for (int age = 0; age < list.count; ++age) {
                const auto& record = list.at(age);
                if (!recordValid(record.simulationTime, serverTime, latencyOut, latencyIn, lerp, maxUnlag))
                    continue;
                if (newest < 0)
                    newest = age;
                if (onshot < 0 && record.shot)
                    onshot = age;
                if (standing < 0 && (record.flags & FL_ONGROUND) && record.velocity.length2D() < 5.0f)
                    standing = age;
                oldest = age;
            }
            const auto add = [&](int age, RecordKind kind) {
                if (age < 0 || candidateCount >= static_cast<int>(candidates.size()))
                    return;
                for (int c = 0; c < candidateCount; ++c) {
                    if (candidates[c].age == age)
                        return;
                }
                candidates[candidateCount++] = { &list.at(age), kind, age, 0 };
            };
            add(onshot, RecordKind::Onshot);
            add(standing, RecordKind::Standing);
            add(newest, RecordKind::Newest);
            // старая — только если она заметно в другом месте: выглядывающего так ловят до того, как он спрятался
            if (oldest >= 0 && newest >= 0 && (list.at(oldest).origin - list.at(newest).origin).squareLength() > 4.0f * 4.0f)
                add(oldest, RecordKind::Oldest);

            if (candidateCount == 0 && list.count) {
                // Все записи старше окна отката (у хвх-игроков simtime отстаёт на 200-450 мс: заряженный DT/дефенсив).
                // Сервер до них не дотянется и возьмёт своё текущее состояние врага — оно новее нашей свежей записи
                // на те обновления, что придут к нему, пока летит наш выстрел. Туда и стреляем, tickcount не трогаем.
                const int updates = predictAhead(list, extrapolatedRecord, memory.globalVars->realtime, latencyOut + latencyIn, interval, gravity, aheadStep);
                candidates[candidateCount++] = { &extrapolatedRecord, RecordKind::Ahead, 0, updates };
            }

            if (candidateCount == 0) {
                // записей ещё нет (только что появился, дефенсив съел всё) — старый способ:
                // интерполированная модель, tickcount не трогаем
                static csgo::matrix3x4 setup[MAXSTUDIOBONES];
                if (!entity.setupBones(setup, MAXSTUDIOBONES, BONE_USED_BY_HITBOX, memory.globalVars->currenttime))
                    continue;
                std::copy(setup, setup + RECORD_BONES, liveRecord.bones);
                liveRecord.simulationTime = entity.simulationTime();
                liveRecord.origin = entity.getAbsOrigin();
                liveRecord.eyeAngles = serverEyeAngles(i, entity);
                liveRecord.absYaw = entity.getAbsAngle().y;
                liveRecord.velocity = entity.velocity();
                liveRecord.flags = entity.flags();
                liveRecord.choked = list.count ? list.at(0).choked : 1;
                liveRecord.shot = false;
                liveRecord.breakingLagComp = false;
                liveRecord.rebuilt = false;
                liveRecord.resolved = false;
                candidates[candidateCount++] = { &liveRecord, RecordKind::Live, 0, 0 };
            }
        }
        if (cheapScan)
            candidateCount = 1;

        const int health = entity.health();
        const int minDamageSetting = minDamageOverride ? settings.minDamageOverride : settings.minDamage;
        // больше 100 — «HP+N»: убить с запасом N; иначе хп меньше мин. урона — достаточно добить
        const int minDamage = minDamageSetting > 100 ? health + (minDamageSetting - 100) : std::min(minDamageSetting, health);
        const bool bodyOnly = forceBody;
        // конечности — только когда можно убить: рука даёт x1, нога x0.75 от урона оружия
        const bool limbsLethal = static_cast<float>(weaponData->damage) >= static_cast<float>(health);

        // режим резольвера — лучший по счёту для этого игрока; не чокает — значит, не десинкает, не крутим
        const int mode = isDesyncing(state) ? bestMode(state) : -1;
        const float deltaScale = mode == GuessLow || mode == OppositeLow ? 0.5f : 1.0f;

        for (int c = 0; c < candidateCount; ++c) {
            const auto& record = *candidates[c].record;

            // Скелет, по которому стреляем. Если резольвер по слоям дал угол ног, скелет записи
            // уже собран под него (updateRecords), а plus/minus — крайние ноги для безопасных точек.
            std::copy(record.bones, record.bones + RECORD_BONES, bones);
            const bool layerResolved = rage.resolver && record.resolved;
            bool variants = false;
            float side = 0.0f;
            if (layerResolved) {
                // крайние скелеты собраны только при включённых безопасных точках
                if (record.rebuilt) {
                    std::copy(record.plus, record.plus + RECORD_BONES, plusBones);
                    std::copy(record.minus, record.minus + RECORD_BONES, minusBones);
                    variants = true;
                }
            } else if (rage.resolver && mode >= 0 && buildDesyncVariants(isBot[i], record, record.bones, plusBones, minusBones, deltaScale)) {
                variants = true;
                if (pendingShot[i] == 0.0f || state.guessedSide == 0.0f)
                    state.guessedSide = guessDesyncSide(engineInterfaces.engineTrace(), eye, plusBones, minusBones);
                // у джиттеров десинк обычно переключается вместе с yaw — берём фазу джиттера вместо фристенда
                // если сервер увидит нечётное число обновлений сверх нашей записи — фаза уже перещёлкнулась
                const bool flipped = candidates[c].updatesAhead % 2 == 1;
                const float relation = jitterRelation(state);
                const float phase = (flipped ? -state.jitterPhase : state.jitterPhase) * (relation < 0.0f ? -1.0f : 1.0f);
                const float base = isJittering(state) ? phase : state.guessedSide;
                if (mode == Guess || mode == GuessLow)
                    side = base;
                else if (mode == Opposite || mode == OppositeLow)
                    side = -base;
                // промах по вине резольвера — сразу зеркалим сторону, не дожидаясь счёта режимов
                if (state.bruteSide != 0.0f && memory.globalVars->realtime < state.bruteUntil)
                    side = state.bruteSide;
                if (side != 0.0f) {
                    const auto* chosen = side > 0.0f ? plusBones : minusBones;
                    std::copy(chosen, chosen + RECORD_BONES, bones);
                }
            }
            const bool desyncSafe = !cheapScan && variants && (rage.safePoint == 2 || (rage.safePoint == 1 && misses[i] > 0));
            const bool predictedRecord = candidates[c].kind == RecordKind::Ahead || candidates[c].kind == RecordKind::Extrapolated;
            // на предсказанной записи сторону десинка проверить нечем (взгляд за 300 мс уходит на 60°) —
            // точки, общие для обеих сторон, предпочитаем там всегда, без требования (см. steady ниже)
            const bool preferDesyncSafe = predictedRecord && !cheapScan && variants;
            if (desyncSafe || preferDesyncSafe) {
                buildCapsules(*set, record.bones, clientCapsules);
                buildCapsules(*set, plusBones, plusCapsules);
                buildCapsules(*set, minusBones, minusCapsules);
            }
            const auto isSafe = [&](const csgo::Vector& point) {
                auto direction = point - eye;
                direction /= std::max(direction.length(), 0.001f);
                const auto end = eye + direction * weaponData->range;
                return rayHitsPlayer(eye, end, clientCapsules) && rayHitsPlayer(eye, end, plusCapsules) && rayHitsPlayer(eye, end, minusCapsules);
            };
            buildCapsules(*set, bones, aimCapsules);

            // Край капсулы прощает только тогда, когда сторона десинка известна: при низкой уверенности
            // резольвера «край» — это промах по чужой стороне, а центр той же капсулы попадает при любой.
            // По логам 29.10 основная масса промахов — «голова (край)» / «тело (край)» после угаданной стороны.
            float sideScale = 1.0f;
            if (rage.resolver && !isBot[i]) {
                if (layerResolved)
                    sideScale = record.remembered ? 0.5f : std::clamp(hvh::resolver::confidence(i) * 1.4f, 0.35f, 1.0f);
                else if (mode >= 0 && side == 0.0f)
                    sideScale = 0.0f;    // десинкает, а сторона не выбрана — точки только по центру
            }

            // Предсказанная запись: сервер мог успеть принять на обновление больше или меньше, чем мы
            // насчитали (чок врага плавает, наша команда ждёт в очереди). Точки, которые попадают и в модель,
            // сдвинутую на шаг назад/вперёд, берём в первую очередь: на бегу поперёк это обычно тело,
            // при беге на нас/от нас — и голова. Нет таких — стреляем как раньше, не стоим.
            const bool checkSteps = predictedRecord && !cheapScan && aheadStep.squareLength() > 1.0f;
            if (checkSteps) {
                earlierCapsules = aimCapsules;
                laterCapsules = aimCapsules;
                for (int k = 0; k < aimCapsules.count; ++k) {
                    earlierCapsules.items[k].a -= aheadStep;
                    earlierCapsules.items[k].b -= aheadStep;
                    laterCapsules.items[k].a += aheadStep;
                    laterCapsules.items[k].b += aheadStep;
                }
            }
            const auto stepSafe = [&](const csgo::Vector& point) {
                if (!checkSteps)
                    return false;
                const auto end = eye + normalized(point - eye) * weaponData->range;
                return rayHitsPlayer(eye, end, earlierCapsules) && rayHitsPlayer(eye, end, laterCapsules);
            };

            // steady — устойчивые к ±1 обновлению; sure — ещё и общие для обеих сторон десинка
            AimPoint bestHead, bestBody, steadyHead, steadyBody, sureHead, sureBody;
            AimPoint weakHead, weakBody;   // лучший достижимый урон без порога — для форсшута
            // разброс в долях дистанции: радиус пятна пуль на d юнитах ≈ d * cone
            const float cone = std::max(weapon.getInaccuracy(), 0.0f)
                + std::clamp(weapon.weaponMode() == 1 ? weaponData->spreadAlt : weaponData->spread, 0.0f, 0.2f);
            const auto scanPoints = [&](bool requireDesyncSafe, bool noHead) {
                for (const auto& [flag, hitboxes] : hitboxGroups) {
                    if (!(settings.hitboxes & flag))
                        continue;
                    if ((bodyOnly || noHead) && flag == HB_Head)
                        continue;
                    const bool isLimb = flag == HB_Arms || flag == HB_Legs || flag == HB_Feet;
                    if (isLimb && (!limbsLethal || cheapScan))
                        continue;

                    const bool isHead = flag == HB_Head;
                    const bool multipoint = (settings.multipoint & flag) && !cheapScan;
                    // каждый промах по игроку прижимает точки к центру; предсказанная запись — тоже
                    const float shrink = (misses[i] >= 2 ? 0.4f : misses[i] == 1 ? 0.7f : 1.0f) * (predictedRecord ? 0.75f : 1.0f);
                    const float maxScale = (isHead ? settings.headScale : settings.bodyScale) * sideScale;

                    for (const auto hitboxId : hitboxes) {
                        if (hitboxId == Hitbox::Max)
                            break;
                        const auto hitbox = set->getHitbox(hitboxId);
                        if (!hitbox || hitbox->bone < 0 || hitbox->bone >= RECORD_BONES)
                            continue;

                        const auto min = transform(hitbox->bbMin, bones[hitbox->bone]);
                        const auto max = transform(hitbox->bbMax, bones[hitbox->bone]);
                        const auto center = (min + max) * 0.5f;

                        // Мультипоинт: центр, края капсулы поперёк взгляда (лево/право), верх у головы и точки
                        // по оси капсулы у вытянутых хитбоксов (конечности, таз). Размер: вручную — ползунок,
                        // авто — сколько влезает, чтобы пятно разброса на этой дистанции не вылезало за капсулу:
                        // смещение s·r плюс половина пятна не больше 0.95·r.
                        std::array<csgo::Vector, 6> points{ center };
                        std::size_t count = 1;
                        const float radius = hitbox->capsuleRadius;
                        if (multipoint && radius > 0.0f) {
                            float scale = maxScale;
                            if (settings.autoScale) {
                                const float blur = (center - eye).length() * cone * 0.5f;
                                // клиентская анимация бегущего расходится с серверной (покачивание головы, фаза шага)
                                // на 1-3 ед.: по ботам без десинка 29.10 — серии промахов в голову «сквозь точку»
                                // на бегу. Край капсулы держим дальше от этой погрешности.
                                const float animError = 0.5f + std::min(record.velocity.length2D(), 300.0f) * 0.006f;
                                scale = std::min(scale, (radius * 0.95f - blur - animError) / radius);
                            }
                            scale *= shrink;
                            if (scale >= 0.2f) {
                                const auto view = normalized(center - eye);
                                csgo::Vector side{ view.y, -view.x, 0.0f };   // горизонтальный перпендикуляр к лучу
                                side = side.squareLength() > 0.0001f ? normalized(side) : csgo::Vector{ 1.0f, 0.0f, 0.0f };
                                const float offset = radius * scale;
                                points[count++] = center + side * offset;
                                points[count++] = center - side * offset;
                                if (isHead)
                                    points[count++] = center + worldUp * offset;
                            }
                            // ось капсулы лежит внутри хитбокса при любом разбросе — концы тянем к центру тем же scale
                            const auto axis = max - min;
                            if (!isHead && flag != HB_Chest && axis.length() > radius && scale > 0.0f) {
                                points[count++] = center + axis * (0.5f * std::min(scale + 0.3f, 0.9f));
                                points[count++] = center - axis * (0.5f * std::min(scale + 0.3f, 0.9f));
                            }
                        }

                        for (std::size_t p = 0; p < count; ++p) {
                            if (p > 0 && pointBudget-- <= 0)
                                break;
                            if (requireDesyncSafe && !isSafe(points[p]))
                                continue;
                            // стены — по пути к точке записи: сервер откатит игрока именно туда
                            const float baseDamage = autowall::getDamageToPoint(engineInterfaces, interfaces, points[p], weaponData, rage.autowall);
                            if (baseDamage < 1.0f)
                                continue;
                            // куда пуля попадёт на самом деле: первый хитбокс на луче (рука перед животом и т.п.)
                            int group = firstHitGroup(aimCapsules, eye, points[p] + normalized(points[p] - eye) * 2.0f);
                            if (group < 0)
                                group = hitbox->group;
                            const float damage = autowall::scaleDamage(entity, group, weaponData, baseDamage);
                            // форсшут: лучший урон запоминаем до проверки порога — если настройку
                            // выбить нечем, стреляем сюда и опускаем порог до этого урона
                            auto& weak = isHead ? weakHead : weakBody;
                            if (damage > weak.damage)
                                weak = { points[p], damage, baseDamage, isHead, p > 0 };
                            if (damage < static_cast<float>(minDamage) || (isLimb && damage < static_cast<float>(health)))
                                continue;
                            auto& slot = isHead ? bestHead : bestBody;
                            if (damage > slot.damage)
                                slot = { points[p], damage, baseDamage, isHead, p > 0 };
                            if (predictedRecord) {
                                const bool steps = checkSteps && stepSafe(points[p]);
                                auto& steady = isHead ? steadyHead : steadyBody;
                                if (steps && damage > steady.damage)
                                    steady = { points[p], damage, baseDamage, isHead, p > 0 };
                                auto& sure = isHead ? sureHead : sureBody;
                                if ((steps || !checkSteps) && (!preferDesyncSafe || isSafe(points[p])) && damage > sure.damage)
                                    sure = { points[p], damage, baseDamage, isHead, p > 0 };
                            }
                        }
                    }
                }
            };
            scanPoints(desyncSafe, false);
            const bool safeOnly = desyncSafe;
            if (sureHead.damage > 0.0f || sureBody.damage > 0.0f) {
                bestHead = sureHead;
                bestBody = sureBody;
            } else if (steadyHead.damage > 0.0f || steadyBody.damage > 0.0f) {
                bestHead = steadyHead;
                bestBody = steadyBody;
            }

            bool headOk = bestHead.damage > 0.0f;
            bool bodyOk = bestBody.damage > 0.0f;
            int effectiveMinDamage = minDamage;
            bool forced = false;
            if (!headOk && !bodyOk) {
                if (!rage.forceShot || (weakHead.damage <= 0.0f && weakBody.damage <= 0.0f)) {
                    statusText = safeOnly ? "нет безопасных точек" : "мало урона";
                    continue;
                }
                // Форсшут: заданный мин. урон недостижим — берём лучшую точку и порог по её урону,
                // иначе цель просто не обстреливается. Хитшанс потом считается уже от этого порога.
                bestHead = weakHead;
                bestBody = weakBody;
                headOk = bestHead.damage > 0.0f;
                bodyOk = bestBody.damage > 0.0f;
                forced = true;
                statusText = "форсшут";
            }

            // Было ещё «вперёд + джиттер → тело»: по логам 29.10 тело попадало 36%, голова 42%, а скаут в тело
            // почти не убивает с выстрела — враг успевал ответить. Тело теперь только по настройкам/если убивает.
            AimPoint chosen;
            if (forced && bodyOk && bestBody.damage > bestHead.damage)
                chosen = bestBody;   // форсшут: бьём туда, где урона больше
            else if (bodyOk && (bodyOnly || settings.preferBody
                    || (settings.bodyIfLethal && bestBody.damage >= static_cast<float>(health))))
                chosen = bestBody;
            else
                chosen = headOk ? bestHead : bestBody;
            // порог хитшанса — по выбранной точке: иначе он окажется выше её урона и выстрела не будет
            if (forced)
                effectiveMinDamage = std::max(1, static_cast<int>(std::floor(chosen.damage)));
            const AimPoint& other = chosen.isHead ? bestBody : bestHead;

            // запись с целью нашлась — дальше по приоритету не ищем; между игроками — по урону
            int slot = topCount;
            while (slot > 0 && (chosen.damage > top[slot - 1].point.damage || (chosen.damage == top[slot - 1].point.damage && fov < top[slot - 1].fov)))
                --slot;
            if (slot < static_cast<int>(top.size())) {
                for (int k = std::min(topCount, static_cast<int>(top.size()) - 1); k > slot; --k)
                    top[k] = top[k - 1];
                topCount = std::min(topCount + 1, static_cast<int>(top.size()));
                auto& best = top[slot];
                best.index = i;
                best.point = chosen;
                best.fallback = other.damage > 0.0f ? other : AimPoint{};
                best.fov = fov;
                best.set = set;
                best.simulationTime = record.simulationTime;
                best.safe = safeOnly;
                best.kind = candidates[c].kind;
                best.backtrackTicks = list.count && candidates[c].age > 0
                    ? timeToTicks(list.at(0).simulationTime - record.simulationTime, interval) : 0;
                best.speed = record.velocity.length2D();
                best.origin = record.origin;
                best.eyeYaw = record.eyeAngles.y;
                best.choke = record.choked;
                best.minDamage = effectiveMinDamage;
                best.recordCount = list.count;
                best.updatesAhead = candidates[c].updatesAhead;
                best.newestAge = list.count ? serverTime - list.at(0).simulationTime : 0.0f;
                std::copy(bones, bones + RECORD_BONES, best.bones);
            }
            if (layerResolved) {
                // для индикатора: во сколько раз угол ног отличается от взгляда (в долях максимума)
                const float limit = std::max(hvh::resolver::maxDesync(i), 1.0f);
                resolverOffset[i] = std::clamp(std::remainder(record.resolvedYaw - record.eyeAngles.y, 360.0f) / limit, -1.0f, 1.0f);
                resolverMode[i] = record.remembered ? MemoryMode : LayerMode;
            } else {
                resolverOffset[i] = side * deltaScale;
                resolverMode[i] = variants ? mode : -1;
            }
            break;
        }
    }

    if (topCount == 0)
        return false;

    auto& best = top[0];
    target = best.index;
    // с нажатым прыжком тик уже воздушный: стоп и скоп в нём только ломают бхоп
    const bool onGround = (localPlayer.get().flags() & FL_ONGROUND) && !(cmd->buttons & csgo::UserCmd::IN_JUMP);

    // автоскоуп раньше стопа: зум и точность скоупа набираются несколько тиков
    const bool scoping = needScope && onGround;
    if (scoping)
        cmd->buttons |= csgo::UserCmd::IN_ATTACK2;

    // автостоп заранее: начинаем за столько тиков до готовности оружия, сколько нужно на торможение
    if (stopMode != AutoStop::Off && onGround && (canShoot || readySoon || settings.autoStopEarly))
        autoStop(cmd, stopMode, stopMaxSpeed, interval, moveVars);

    if (scoping) {
        statusText = "скоуп";
        return false;
    }

    if (!canShoot) {
        statusText = readySoon ? "автостоп" : "перезарядка";
        return false;
    }

    // по очереди: цель, её вторая группа (голова/тело), следующая цель — первая, где набрался хитшанс
    csgo::Vector angle{};
    int chance = 0, firstChance = -1, chosenSlot = -1;
    for (int k = 0; k < topCount && chosenSlot < 0; ++k) {
        auto& candidate = top[k];
        buildCapsules(*candidate.set, candidate.bones, aimCapsules);
        const auto entity = csgo::Entity::from(retSpoofGadgets->client, clientInterfaces.getEntityList().getEntity(candidate.index));
        angle = (candidate.point.position - eye).toAngle();
        chance = hitchance(weapon, weaponData, entity, eye, angle, aimCapsules, candidate.point.baseDamage, candidate.minDamage);
        if (firstChance < 0)
            firstChance = chance;
        if (chance < settings.hitchance && candidate.fallback.damage > 0.0f) {
            const auto fallbackAngle = (candidate.fallback.position - eye).toAngle();
            if (const int fallbackChance = hitchance(weapon, weaponData, entity, eye, fallbackAngle, aimCapsules, candidate.fallback.baseDamage, candidate.minDamage);
                fallbackChance >= settings.hitchance) {
                candidate.point = candidate.fallback;
                angle = fallbackAngle;
                chance = fallbackChance;
            }
        }
        if (chance >= settings.hitchance)
            chosenSlot = k;
    }
    if (chosenSlot < 0) {
        static char text[32];
        std::snprintf(text, sizeof(text), "хитшанс %d%%", firstChance);
        statusText = text;
        return false;
    }
    if (chosenSlot != 0) {
        std::swap(top[0], top[chosenSlot]);
        buildCapsules(*best.set, best.bones, aimCapsules);
        target = best.index;
    }

    auto aimAngle = angle - localPlayer.get().aimPunchAngle() * 2.0f;
    aimAngle.normalize();
    cmd->viewangles = aimAngle;
    cmd->buttons |= csgo::UserCmd::IN_ATTACK;
    // сервер откатит цель ровно в эту запись; без записи (интерполированная модель, экстраполяция) tickcount не трогаем
    if (best.kind != RecordKind::Live && best.kind != RecordKind::Extrapolated && best.kind != RecordKind::Ahead)
        cmd->tickCount = timeToTicks(best.simulationTime + lerp, interval);

    firedAt = serverTime;
    firedCycle = weaponData->cycletime;
    pendingShot[best.index] = memory.globalVars->realtime;
    auto& shot = shots[best.index];
    shot = {};
    shot.eye = eye;
    shot.aimPoint = best.point.position;
    shot.isHead = best.point.isHead;
    shot.edgePoint = best.point.edge;
    shot.safe = best.safe;
    shot.side = resolverOffset[best.index];
    shot.damage = static_cast<int>(best.point.damage);
    shot.mode = resolverMode[best.index];
    shot.kind = best.kind;
    shot.backtrackTicks = best.backtrackTicks;
    shot.hitchance = chance;
    shot.enemySpeed = best.speed;
    shot.enemyChoke = best.choke;
    shot.recordCount = best.recordCount;
    shot.updatesAhead = best.updatesAhead;
    shot.capsules = aimCapsules;
    shot.predicted = best.kind == RecordKind::Ahead || best.kind == RecordKind::Extrapolated;
    shot.predictedSimulationTime = best.simulationTime;
    shot.predictedOrigin = best.origin;
    shot.usedEyeYaw = best.eyeYaw;
    shot.updateTime = static_cast<float>(std::max(best.choke, 1)) * interval;
    shot.set = best.set;
    shot.ourClockMs = static_cast<int>((serverTime - static_cast<float>(memory.globalVars->tickCount) * interval) * 1000.0f);
    if (records[best.index].count)
        shot.enemyLagMs = static_cast<int>((static_cast<float>(memory.globalVars->tickCount) * interval - latencyIn - records[best.index].at(0).simulationTime) * 1000.0f);
    shot.newestAgeMs = static_cast<int>(best.newestAge * 1000.0f);
    shot.pingMs = static_cast<int>((latencyOut + latencyIn) * 1000.0f);
    shot.maxUnlagMs = static_cast<int>(maxUnlag * 1000.0f);
    lastShotIndex = best.index;
    lastShotTime = memory.globalVars->realtime;
    {
        static char text[96];
        const float offset = resolverOffset[best.index];
        std::snprintf(text, sizeof(text), "%s%s%s  [%s %dт]", best.point.isHead ? "выстрел: голова" : "выстрел: тело",
            offset > 0.0f ? "  [R +]" : offset < 0.0f ? "  [R -]" : "", offset != 0.0f && std::abs(offset) < 1.0f ? " low" : "",
            kindNames[static_cast<int>(best.kind)], best.backtrackTicks);
        statusText = text;
    }

    if (!rage.silent)
        engineInterfaces.getEngine().setViewAngles(cmd->viewangles);

    if (!tickbase::isShifting())
        tickbase::onShot();
    return true;
}

}

}

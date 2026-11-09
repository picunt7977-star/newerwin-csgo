#include <algorithm>
#include <array>
#include <bit>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <optional>
#include <string>

#include <sys/stat.h>

#include <imgui/imgui.h>

#include "Autowall.h"
#include "HvhConfig.h"
#include "Extras.h"
#include "Ragebot.h"
#include "AntiAim.h"
#include "Resolver.h"
#include "Defensive.h"
#include "Tickbase.h"
#include "Movement.h"
#include "RadarSpotted.h"

#include <Helpers.h>
#include <Netvars.h>
#include <Interfaces/ClientInterfaces.h>
#include <Interfaces/EngineInterfaces.h>
#include <Interfaces/OtherInterfaces.h>
#include <Memory.h>
#include <CSGO/Constants/GameEventNames.h>
#include <CSGO/ConVar.h>
#include <CSGO/Cvar.h>
#include <CSGO/Engine.h>
#include <CSGO/EngineTrace.h>
#include <CSGO/Entity.h>
#include <CSGO/EntityList.h>
#include <CSGO/GameEvent.h>
#include <CSGO/GlobalVars.h>
#include <CSGO/LocalPlayer.h>
#include <CSGO/matrix3x4.h>
#include <CSGO/ModelInfo.h>
#include <CSGO/NetworkChannel.h>
#include <CSGO/Sound.h>
#include <CSGO/UserCmd.h>
#include <CSGO/Vector.h>
#include <CSGO/WeaponData.h>
#include <CSGO/WeaponId.h>

// Рейджбот newerwin.
//
// Тик рейджа:
//  1. записи лагкомпа на каждого врага — как история на сервере (simtime назад не принимается, пока голова
//     истории не устарела): свой скелет на каждое новое simtime + два крайних по десинку;
//  2. сторона десинка: свежие слои анимации > перебор после промаха > фаза джиттера > свидетельства выстрелов
//     > фристенд-догадка (с учётом, как часто она у этого игрока права);
//  3. по каждой цели — записи-кандидаты (onshot, стоит, свежая, старая; вне окна отката — прогноз «вперёд»),
//     каждая оценивается целиком, берётся лучшая;
//  4. точки мультипоинта: урон автоволлом × надёжность (вероятность попасть, калибровка по логам 29+31.10);
//  5. хитшанс по всем 256 сидам разброса, выстрел сразу, DT сдвигает тем же пакетом.
// После выстрела: попадание/промах с причиной (по импакту пули) — это и есть обучение резольвера и прогноза.

namespace hvh::ragebot
{

namespace
{

constexpr int FL_ONGROUND = 1 << 0;
// сид разброса на сервере — (random_seed & 0xFF) + 1: всего 256 вариантов, перебираем все
constexpr int HITCHANCE_SEEDS = 256;
// кости модели: хитбоксы висят на первых ~80, больше 128 у моделей CS:GO не бывает
constexpr int RECORD_BONES = 128;
// записей на игрока: sv_maxunlag 0.2 с на 128 тиках = 26, с запасом
constexpr int MAX_RECORDS = 32;
constexpr int HEAD_BONE = 8;
constexpr int HITGROUP_HEAD = 1;

// ---------- общее состояние ----------

bool minDamageOverride = false;
bool forceBody = false;
bool forceShot = false;
int target = 0;
bool fireReadyNow = false;      // оружие готово к выстрелу сейчас или в ближайшие тики (последний тик рейджа)
int fireReadyTick = -1000;
const char* statusText = "";
float localDiedAt = -1.0f;
const Memory* memoryPtr = nullptr;
std::optional<csgo::Engine> engine;

std::mutex viewMutex;
std::array<TargetView, 65> views{};
ShotView lastShot;

// ---------- логи: консоль игры + файл ~/csgo-hvh/logs/ДАТА.log + коротко на экране ----------

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

struct ScreenLine {
    std::string text;
    ImU32 color = 0;
    double time = 0.0;
};
std::array<ScreenLine, 8> screenLines;
int screenNext = 0;
std::mutex screenMutex;   // пишет игровой поток (события), читает отрисовка

[[nodiscard]] double steadySeconds() noexcept
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// одна строка на событие: в консоль (цветом) и файл — полностью, на экран — до «|»
void report(const std::uint8_t (&color)[4], const char* text) noexcept
{
    if (memoryPtr && memoryPtr->conColorMsg)
        memoryPtr->conColorMsg(color, "%s\n", text);
    fileLog(text);
    std::string brief = text;
    if (brief.starts_with("[newerwin] "))
        brief.erase(0, 11);
    if (const auto bar = brief.find(" |"); bar != std::string::npos)
        brief.resize(bar);
    std::scoped_lock lock{ screenMutex };
    screenLines[screenNext] = { std::move(brief), IM_COL32(color[0], color[1], color[2], 255), steadySeconds() };
    screenNext = (screenNext + 1) % static_cast<int>(screenLines.size());
}

[[nodiscard]] std::string playerName(int index) noexcept
{
    if (csgo::PlayerInfo info; engine && engine->getPlayerInfo(index, info))
        return info.name;
    return "#" + std::to_string(index);
}

// ---------- углы взгляда с сервера ----------
// m_angEyeAngles у чужих игроков к CreateMove уже интерполирован (у джиттера — между фазами).
// Присланный сервером угол живёт до RENDER_START: снимаем его в NET_UPDATE_POSTDATAUPDATE_START.

struct NetworkState {
    float simulationTime = -1.0f;
    csgo::Vector eye{};
    float aliveCycle = -1.0f;   // слой ALIVELOOP как прислал сервер
    float aliveRate = 0.0f;
    int aliveSequence = -1;
};
std::array<NetworkState, 65> network{};

[[nodiscard]] csgo::Vector serverEyeAngles(int index, const csgo::Entity& entity) noexcept
{
    if (network[index].simulationTime == entity.simulationTime())
        return network[index].eye;
    return entity.eyeAngles();
}

// ---------- резольвер ----------
// Одна шкала свидетельств на игрока: + — тело на стороне «взгляд + десинк», − — «взгляд − десинк».
// Источники (вес): импакт пули доказал сторону геометрией (4), попадание по стороне (3 доказанное / 1.5),
// промах по вине резольвера без доказательства (−2 той стороне). Свежие слои анимации используются напрямую
// (скелет под найденный угол ног), в шкалу не пишутся.
// Шкала медленно гаснет: статичный десинк держится долго, инвертер перебьют промахи.
// У джиттерящих вместо стороны учим связь: совпадает ли сторона с фазой рывка взгляда.

struct Brain {
    int userId = -1;
    bool tracked = false;
    float lastSimulationTime = 0.0f;
    float lastEyeYaw = 0.0f;
    float lastYawDelta = 0.0f;
    std::uint8_t chokeBits = 0xFF;  // бит на обновление: чокал (без чока десинка нет)
    std::uint8_t jitterBits = 0;    // бит на обновление: взгляд дёрнулся туда-обратно > 25°
    float jitterPhase = 1.0f;       // знак последнего рывка
    bool desyncProven = false;      // пуля прошла сквозь «недесинкающую» модель
    float evidence = 0.0f;
    float evidenceAt = 0.0f;
    float jitterLink = 0.0f;        // + — сторона = фаза, − — сторона = −фаза
    float bruteSide = 0.0f;         // после промаха без доказательства — пробуем другую сторону
    float bruteUntil = 0.0f;
    bool bruteProven = false;       // сторону перебора доказал импакт, а не «просто другая»
    std::uint8_t guessRight = 0, guessWrong = 0;   // фристенд-догадка против доказанной стороны (его антиаим бывает наоборот)
    float layerFoundAt = -1.0f;     // simtime последней учтённой находки по слоям
    int misses = 0;                 // подряд, сбрасывается попаданием
    float aheadBias = -0.3f;        // своя поправка прогноза «вперёд», в долях обновления (у каждого чита свой рисунок чока)
    int aheadChecks = 0;
    std::uint8_t headShots = 0, headHits = 0, bodyShots = 0, bodyHits = 0;
    // Сторона ног по слоям на каждом обновлении (бит 1 — «+»). Джиттер десинка взгляд не трогает —
    // jitterBits его не видят, а ноги перекидываются: по логам 01.11 у таких врагов слои давали
    // сторону прошлого обновления, и 3 из 5 доказанных промахов в голову — выстрел в прогноз.
    std::uint8_t layerSides = 0;
    std::uint8_t layerSideCount = 0;
    // Промахи резольвера в голову с последнего попадания в голову. headUnlucky ждал 4 выстрела в тело,
    // а пока голова выигрывает по пользе, их не бывает — и по одному врагу мажем в голову раз за разом
    // (лог 01.11: «густав», «Gargamel», «мерзостb» — по 2–3 промаха резольвера в голову подряд).
    std::uint8_t headResolverMisses = 0;
};
std::array<Brain, 65> brains{};

[[nodiscard]] bool desyncing(const Brain& brain) noexcept
{
    return !brain.tracked || brain.desyncProven || std::popcount(brain.chokeBits) >= 2;
}

[[nodiscard]] bool jittering(const Brain& brain) noexcept
{
    return std::popcount(static_cast<std::uint8_t>(brain.jitterBits & 0x0F)) >= 2;
}

// Ноги по слоям перекидываются: из последних 4 находок сторона менялась хотя бы дважды
[[nodiscard]] bool desyncFlipping(const Brain& brain) noexcept
{
    if (brain.layerSideCount < 4)
        return false;
    const unsigned bits = brain.layerSides & 0x0Fu;
    return std::popcount((bits ^ (bits >> 1)) & 0x07u) >= 2;
}

[[nodiscard]] float sideEvidence(const Brain& brain, float realtime) noexcept
{
    // полураспад ~14 с
    return brain.evidence * std::exp(-(realtime - brain.evidenceAt) / 20.0f);
}

void addEvidence(Brain& brain, float side, float weight, float realtime) noexcept
{
    if (side == 0.0f)
        return;
    brain.evidence = std::clamp(sideEvidence(brain, realtime) + (side > 0.0f ? weight : -weight), -8.0f, 8.0f);
    brain.evidenceAt = realtime;
}

void addJitterLink(Brain& brain, float side, float phase, float weight) noexcept
{
    if (side == 0.0f || phase == 0.0f)
        return;
    brain.jitterLink = std::clamp(brain.jitterLink + ((side > 0.0f) == (phase > 0.0f) ? weight : -weight), -6.0f, 6.0f);
}

void track(Brain& brain, const csgo::Entity& entity, int index, int userId, float interval) noexcept
{
    if (brain.userId != userId) {
        brain = {};
        brain.userId = userId;
    }
    const float simulationTime = entity.simulationTime();
    if (brain.tracked && simulationTime == brain.lastSimulationTime)
        return;
    const float eyeYaw = serverEyeAngles(index, entity).y;
    if (brain.tracked && interval > 0.0f) {
        const int ticks = static_cast<int>(std::lround((simulationTime - brain.lastSimulationTime) / interval));
        if (ticks >= 1 && ticks <= 17)   // большие разрывы — дормант, а не чок
            brain.chokeBits = static_cast<std::uint8_t>((brain.chokeBits << 1) | (ticks > 1 ? 1 : 0));
        const float delta = std::remainder(eyeYaw - brain.lastEyeYaw, 360.0f);
        const bool jitter = std::abs(delta) > 25.0f && std::abs(brain.lastYawDelta) > 25.0f && (delta > 0.0f) != (brain.lastYawDelta > 0.0f);
        brain.jitterBits = static_cast<std::uint8_t>((brain.jitterBits << 1) | (jitter ? 1 : 0));
        if (std::abs(delta) > 25.0f)
            brain.jitterPhase = delta > 0.0f ? 1.0f : -1.0f;
        brain.lastYawDelta = delta;
    }
    brain.tracked = true;
    brain.lastSimulationTime = simulationTime;
    brain.lastEyeYaw = eyeYaw;

    // Находка по слоям у джиттерящих учит связь стороны с фазой рывка — она не устаревает.
    // В шкалу стороны слои НЕ идут: находки приходят почти каждое обновление и за полсекунды забивали шкалу,
    // заглушая импакты; а устаревшая сторона слоёв по логам хуже фристенд-догадки (45% против 52% в тело).
    float foundAt = 0.0f, ratio = 0.0f;
    if (jittering(brain) && hvh::resolver::lastFound(index, foundAt, ratio) && foundAt != brain.layerFoundAt && std::abs(ratio) > 0.3f) {
        brain.layerFoundAt = foundAt;
        addJitterLink(brain, ratio, brain.jitterPhase, 0.5f * std::clamp(hvh::resolver::confidence(index), 0.25f, 1.0f));
    }
}

// ---------- записи лагкомпа ----------
// Сервер при выстреле откатывает цель в состояние на cmd->tickcount − lerp. Стреляем по записи с её simtime
// и ставим tickcount = simtime + lerp — сервер проверит попадание по той же модели, что и мы.

enum class RecordKind { Onshot, Standing, Newest, Oldest, Live, Extrapolated, Ahead };
constexpr const char* kindNames[]{ "onshot", "стоит", "свежая", "старая", "без записей", "экстраполяция", "вперёд" };

struct LagRecord {
    float simulationTime = 0.0f;
    float receivedAt = 0.0f;
    csgo::Vector origin{};
    csgo::Vector velocity{};
    csgo::Vector eyeAngles{};
    float absYaw = 0.0f;
    int flags = 0;
    int choked = 1;                 // тиков с прошлого обновления (1 — не чокал)
    bool shot = false;              // выстрелил в этом обновлении: углы настоящие
    bool breakingLagComp = false;   // сдвинулся > 64 ед.: сервер не откатывает
    bool animStale = false;         // анимация прошла не столько, сколько simtime (дефенсив)
    float maxDesync = 0.0f;
    float jitterPhase = 1.0f;       // фаза джиттера на момент записи
    bool variants = false;          // plus/minus собраны анимацией
    bool animFixed = false;         // крайние скелеты собраны прогоном анимации, а не подменой поз
    bool resolvedByAnimation = false;   // и скелет под угол резольвера — тоже
    bool resolved = false;          // bones собраны под угол ног от резольвера по слоям
    float resolvedYaw = 0.0f;
    float resolvedConfidence = 0.0f;
    float aliveCycle = -1.0f;
    float aliveRate = 0.0f;
    int aliveSequence = -1;
    csgo::matrix3x4 bones[RECORD_BONES];
    csgo::matrix3x4 plus[RECORD_BONES];
    csgo::matrix3x4 minus[RECORD_BONES];
};

struct PlayerRecords {
    std::array<LagRecord, MAX_RECORDS> ring;
    int newestIndex = -1;
    int count = 0;
    float skippedSimulationTime = -1.0f;

    [[nodiscard]] const LagRecord& at(int age) const noexcept { return ring[(newestIndex - age + MAX_RECORDS) % MAX_RECORDS]; }
    LagRecord& push() noexcept
    {
        newestIndex = (newestIndex + 1) % MAX_RECORDS;
        count = std::min(count + 1, MAX_RECORDS);
        return ring[newestIndex];
    }
    void clear() noexcept
    {
        count = 0;
        newestIndex = -1;
        skippedSimulationTime = -1.0f;
    }
};
std::array<PlayerRecords, 65> records;

[[nodiscard]] float maxDesyncFor(const csgo::Vector& velocity) noexcept
{
    return 58.0f * (1.0f - 0.5f * std::clamp(velocity.length2D() / 250.0f, 0.0f, 1.0f));
}

// ---------- пересборка скелета под заданный угол ног ----------
// Ставим модели ноги (abs yaw), body_yaw = взгляд − ноги и move_yaw от новых ног, сбрасываем кэш костей
// и просим у игры скелет. Офсеты кэша — из байтов C_BaseAnimating::InvalidateBoneCache:
//   mov rax,[rip+g_iModelBoneCounter]; mov dword [rdi+m_flLastBoneSetupTime],-FLT_MAX; sub rax,1; mov [rdi+m_iMostRecentModelBoneCounter],rax
constexpr int POSE_MOVE_YAW = 7;
constexpr int POSE_BODY_YAW = 11;

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

int rebuildFailures = 0;
bool rebuildDisabled = false;   // кэш так и не сбрасывается — дальше крайние скелеты поворотом костей

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

    // вернуть как было и снова сбросить кэш — иначе кадр отрисует наш последний скелет
    poses[POSE_BODY_YAW] = savedBodyYaw;
    poses[POSE_MOVE_YAW] = savedMoveYaw;
    absAngles = savedAbsAngles;
    rotation = savedRotation;
    invalidateBoneCache(entity, offsets);
    return ok;
}

// ---------- анимфикс: скелет по снимку серверной анимации ----------
// rebuildAtYaw выше подменяет две позы формулой и просит кости у игры. Этого мало: при угле ног
// animstate крутит не только body_yaw, а ещё lean, move_yaw, доворот позвоночника и наклон корпуса,
// и считает их от своего состояния (скорость, присед, фаза шага). Поэтому правильный путь —
// прогнать движковый CCSGOPlayerAnimState::Update с нужным углом (это делает резольвер: Resolver.cpp
// simulateAt), снять всё, что прогон записал, и собрать кости уже по этому снимку.
// Так хитбоксы совпадают с серверными — включая присед, прыжок, lean и фазу бега.
bool rebuildFromSnapshot(const csgo::Entity& entity, const Memory& memory, const LagRecord& record,
    const hvh::resolver::AnimSnapshot& snapshot, csgo::matrix3x4* out) noexcept
{
    const auto& offsets = boneCacheOffsets(memory);
    if (!offsets.ok || rebuildDisabled || !snapshot.ok)
        return false;

    auto* animstate = entity.getAnimstate();
    auto* layers = entity.animOverlays();
    const int count = std::min(entity.animOverlayCount(), csgo::ANIMATION_LAYER_COUNT);
    if (!animstate || !layers || count < snapshot.layerCount || snapshot.layerCount <= 0)
        return false;

    alignas(16) std::byte savedState[sizeof(csgo::AnimState)];
    std::memcpy(savedState, animstate, sizeof(csgo::AnimState));
    csgo::AnimationLayer savedLayers[csgo::ANIMATION_LAYER_COUNT];
    std::copy(layers, layers + count, savedLayers);
    auto& poses = entity.poseParameters();
    std::array<float, 24> savedPoses{};
    std::copy(std::begin(poses), std::end(poses), savedPoses.begin());
    auto& absAngles = const_cast<csgo::Vector&>(entity.getAbsAngle());
    auto& rotation = entity.angRotation();
    const auto savedAbsAngles = absAngles;
    const auto savedRotation = rotation;

    std::memcpy(animstate, snapshot.state, sizeof(csgo::AnimState));
    std::copy(snapshot.layers.begin(), snapshot.layers.begin() + snapshot.layerCount, layers);
    std::copy(snapshot.poses.begin(), snapshot.poses.end(), std::begin(poses));
    absAngles.y = snapshot.footYaw;
    rotation.y = snapshot.footYaw;
    invalidateBoneCache(entity, offsets);

    static csgo::matrix3x4 setup[MAXSTUDIOBONES];
    const bool ok = entity.setupBones(setup, MAXSTUDIOBONES, BONE_USED_BY_HITBOX, memory.globalVars->currenttime);
    if (ok) {
        const auto shift = record.origin - entity.getAbsOrigin();
        for (int bone = 0; bone < RECORD_BONES; ++bone) {
            out[bone] = setup[bone];
            out[bone].translate(shift);
        }
    }

    std::memcpy(animstate, savedState, sizeof(csgo::AnimState));
    std::copy(savedLayers, savedLayers + count, layers);
    std::copy(savedPoses.begin(), savedPoses.end(), std::begin(poses));
    absAngles = savedAbsAngles;
    rotation = savedRotation;
    invalidateBoneCache(entity, offsets);
    return ok;
}

// Скелет под угол ног, честно прогнанный через анимацию. Возвращает угол, на котором движок
// остановился (он сам обрезает по максимальному десинку) — по нему рейджбот знает, где реально тело.
bool rebuildByAnimation(const csgo::Entity& entity, const Memory& memory, const LagRecord& record, int index,
    float feetYaw, csgo::matrix3x4* out, float* landedYaw = nullptr) noexcept
{
    if (!hvh::resolver::canSimulate(index, record.simulationTime))
        return false;
    static hvh::resolver::AnimSnapshot snapshot;
    if (!hvh::resolver::simulateAt(index, entity, memory, feetYaw, snapshot))
        return false;
    if (!rebuildFromSnapshot(entity, memory, record, snapshot, out))
        return false;
    if (landedYaw)
        *landedYaw = snapshot.footYaw;
    return true;
}

// Запасной путь, если пересборка недоступна: поворот готовых костей вокруг оси игрока.
// На сервере на полный угол повёрнуты ноги и таз, а позвоночник закручен обратно к взгляду:
// чем выше кость, тем меньше доворот (ступни 100%, голова ~35%).
void rotateVariant(const LagRecord& record, float sign, csgo::matrix3x4* out) noexcept
{
    const float maxDesync = record.maxDesync > 1.0f ? record.maxDesync : maxDesyncFor(record.velocity);
    const float turn = std::remainder(record.eyeAngles.y - record.absYaw, 360.0f) + sign * maxDesync;
    for (int bone = 0; bone < RECORD_BONES; ++bone) {
        out[bone] = record.bones[bone];
        const float height = std::clamp((record.bones[bone].origin().z - record.origin.z) / 64.0f, 0.0f, 1.0f);
        const float radians = Helpers::deg2rad(turn * (1.0f - 0.65f * height));
        out[bone].rotateYaw(record.origin, std::cos(radians), std::sin(radians));
    }
}

void buildVariants(const csgo::Entity& entity, const Memory& memory, LagRecord& record, int index) noexcept
{
    const float maxDesync = record.maxDesync > 1.0f ? record.maxDesync : maxDesyncFor(record.velocity);
    const float eyeYaw = record.eyeAngles.y;

    // самопроверка крайних скелетов: при десинке ~30°+ голова обязана сдвинуться, но не улететь
    const auto variantsSane = [&record, maxDesync] {
        const float plusMove = (record.plus[HEAD_BONE].origin() - record.bones[HEAD_BONE].origin()).length();
        const float minusMove = (record.minus[HEAD_BONE].origin() - record.bones[HEAD_BONE].origin()).length();
        return plusMove <= 40.0f && minusMove <= 40.0f && !(std::max(plusMove, minusMove) < 0.5f && maxDesync > 25.0f);
    };

    // Крайние скелеты. Сначала анимфикс — прогон анимации под каждый угол (lean, доворот корпуса и
    // фаза шага выходят настоящими). Нет свежей базы у резольвера — старый путь подменой поз,
    // не вышел и он — поворот готовых костей.
    record.animFixed = rebuildByAnimation(entity, memory, record, index, eyeYaw + maxDesync, record.plus)
        && rebuildByAnimation(entity, memory, record, index, eyeYaw - maxDesync, record.minus)
        && variantsSane();
    record.variants = record.animFixed;
    if (!record.variants) {
        record.variants = rebuildAtYaw(entity, memory, record, eyeYaw + maxDesync, record.plus)
            && rebuildAtYaw(entity, memory, record, eyeYaw - maxDesync, record.minus);
        if (record.variants) {
            if (!variantsSane()) {
                record.variants = false;
                if (++rebuildFailures >= 8)
                    rebuildDisabled = true;
            } else {
                rebuildFailures = 0;
            }
        }
    }
    if (!record.variants) {
        rotateVariant(record, 1.0f, record.plus);
        rotateVariant(record, -1.0f, record.minus);
        record.variants = true;
    }

    // угол ног от резольвера по слоям — стреляем ровно по той модели, что на сервере
    if (hvh::resolver::valid(index)) {
        static csgo::matrix3x4 resolved[RECORD_BONES];
        // Угол ног — от взгляда ЭТОЙ записи: у резольвера взгляд с его последнего разбора, и если враг
        // успел развернуться (выстрел, дефенсив), абсолютный угол уезжал за предел десинка
        // (лог 31.10: «ноги +159°/57°» — скелет с телом задом наперёд).
        const float yaw = eyeYaw + std::clamp(hvh::resolver::delta(index), -maxDesync, maxDesync);
        float landed = yaw;
        const bool byAnimation = rebuildByAnimation(entity, memory, record, index, yaw, resolved, &landed);
        const bool built = byAnimation || rebuildAtYaw(entity, memory, record, yaw, resolved);
        if (built && (resolved[HEAD_BONE].origin() - record.bones[HEAD_BONE].origin()).length() < 40.0f) {
            std::copy(resolved, resolved + RECORD_BONES, record.bones);
            record.resolved = true;
            record.resolvedByAnimation = byAnimation;
            // При прогоне берём угол, на котором остановился движок: он обрезает его по
            // максимальному десинку сам, и это та сторона, по которой потом учится резольвер
            record.resolvedYaw = std::remainder(byAnimation ? landed : yaw, 360.0f);
            record.resolvedConfidence = hvh::resolver::confidence(index);
            return;
        }
    }
    // клиент собрал скелет со сглаженным взглядом (джиттер) — пересобираем с теми же ногами и настоящим взглядом
    if (std::abs(std::remainder(entity.eyeAngles().y - eyeYaw, 360.0f)) > 5.0f) {
        if (!rebuildByAnimation(entity, memory, record, index, record.absYaw, record.bones))
            rebuildAtYaw(entity, memory, record, record.absYaw, record.bones);
    }
}

// «потерял след» у лагкомпа сервера: соседние записи дальше 64 ед. по горизонтали
[[nodiscard]] bool horizontalJump(const csgo::Vector& a, const csgo::Vector& b) noexcept
{
    const float dx = a.x - b.x;
    const float dy = a.y - b.y;
    return dx * dx + dy * dy > 64.0f * 64.0f;
}

void updateRecords(PlayerRecords& list, const csgo::Entity& entity, const Memory& memory, bool withVariants, int index, float maxUnlag) noexcept
{
    const float simulationTime = entity.simulationTime();
    if (list.count && simulationTime == list.at(0).simulationTime)
        return;
    if (simulationTime == list.skippedSimulationTime)
        return;
    if (list.count && simulationTime < list.at(0).simulationTime) {
        // Время ушло назад (дефенсив DT). Сервер в таком случае новую запись в историю лагкомпа не кладёт
        // и старые не трогает (CLagCompensationManager: "don't add new entry for same or older time"):
        // выстрел проверяется по голове истории — записи с наибольшим simtime. Повторяем ровно это:
        // наша свежая запись и есть та модель, по которой сервер будет судить, пока simtime её не перегонит.
        // Но каждый кадр сервер удаляет записи старше curtime − sv_maxunlag; удалил голову — история пуста,
        // и следующая запись принимается с любым simtime (иначе после отката тикбейса держали бы старьё секундами).
        // Записи старше остаются: скорость и чок считаются по max(Δsimtime, Δприёма), бектрек — по simtime каждой.
        if (list.at(0).simulationTime >= memory.globalVars->serverTime() - maxUnlag) {
            list.skippedSimulationTime = simulationTime;
            return;
        }
    }

    static csgo::matrix3x4 setup[MAXSTUDIOBONES];
    if (!entity.setupBones(setup, MAXSTUDIOBONES, BONE_USED_BY_HITBOX, memory.globalVars->currenttime))
        return;

    const float interval = memory.globalVars->intervalPerTick;
    const bool havePrevious = list.count > 0;
    const float previousSimulationTime = havePrevious ? list.at(0).simulationTime : 0.0f;
    const auto previousOrigin = havePrevious ? list.at(0).origin : csgo::Vector{};

    auto& record = list.push();
    record.simulationTime = simulationTime;
    record.receivedAt = memory.globalVars->realtime;
    record.origin = entity.origin();
    record.velocity = entity.velocity();
    record.eyeAngles = serverEyeAngles(index, entity);
    record.absYaw = entity.getAbsAngle().y;
    record.flags = entity.flags();
    if (havePrevious && interval > 0.0f) {
        const float gap = std::max(simulationTime - previousSimulationTime, record.receivedAt - list.at(1).receivedAt - interval);
        record.choked = std::clamp(static_cast<int>(std::lround(gap / interval)), 1, 17);
    } else {
        record.choked = 1;
    }
    // сервер сравнивает по горизонтали (Length2DSqr > m_flTeleportDistanceSqr): падение или прыжок — не телепорт
    record.breakingLagComp = havePrevious && horizontalJump(record.origin, previousOrigin);
    record.shot = false;
    if (const auto weapon = csgo::Entity::from(retSpoofGadgets->client, entity.getActiveWeapon()); weapon.getPOD())
        record.shot = std::abs(weapon.lastShotTime() - simulationTime) < interval * 0.5f;
    record.maxDesync = std::clamp(entity.getMaxDesyncAngle(), 0.0f, 60.0f);
    record.jitterPhase = brains[index].jitterPhase;
    record.variants = false;
    record.resolved = false;
    record.resolvedConfidence = 0.0f;

    // Дефенсив: сервер гоняет команды со сдвинутым тикбейсом, а цикл ALIVELOOP идёт по настоящему времени
    // анимации. Сдвинулся не на rate·Δsimtime — запись описывает не тот тик, откат её не найдёт.
    const auto& net = network[index];
    const bool haveAlive = net.simulationTime == simulationTime && net.aliveSequence >= 0;
    record.aliveCycle = haveAlive ? net.aliveCycle : -1.0f;
    record.aliveRate = haveAlive ? net.aliveRate : 0.0f;
    record.aliveSequence = haveAlive ? net.aliveSequence : -1;
    record.animStale = false;
    if (haveAlive && havePrevious && simulationTime > previousSimulationTime) {
        const auto& previous = list.at(1);
        const float expected = previous.aliveCycle + previous.aliveRate * (simulationTime - previousSimulationTime);
        if (previous.aliveSequence == record.aliveSequence && previous.aliveCycle >= 0.0f && previous.aliveRate > 0.0f
            && expected < 0.9f && record.aliveCycle >= previous.aliveCycle && record.aliveCycle < 0.9f)
            record.animStale = std::abs(record.aliveCycle - expected) > std::max(2.0f * interval * previous.aliveRate, 0.002f);
    }

    // setupBones отдаёт скелет в интерполированной позиции; на сервере в этот simtime игрок в m_vecOrigin
    const auto shift = record.origin - entity.getAbsOrigin();
    for (int bone = 0; bone < RECORD_BONES; ++bone) {
        record.bones[bone] = setup[bone];
        record.bones[bone].translate(shift);
    }
    if (withVariants)
        buildVariants(entity, memory, record, index);
    // история стороны по слоям — только заметные углы: у ног почти по взгляду знак случаен
    if (record.resolved && index > 0 && index < static_cast<int>(brains.size())) {
        const float delta = std::remainder(record.resolvedYaw - record.eyeAngles.y, 360.0f);
        if (std::abs(delta) >= 0.3f * std::max(record.maxDesync, 1.0f)) {
            auto& brain = brains[index];
            brain.layerSides = static_cast<std::uint8_t>((brain.layerSides << 1) | (delta > 0.0f ? 1 : 0));
            brain.layerSideCount = static_cast<std::uint8_t>(std::min(brain.layerSideCount + 1, 8));
        }
    }
}

// Окно отката сервера: цель не дальше 0.2 с от «правильного» времени и не старше sv_maxunlag
// (serverTime — наш тикбейс, то есть время выполнения команды на сервере)
// margin — запас у края sv_maxunlag: логи 04–09.11, бектрек 12+ тиков — 4/13 (30%) против 84–100% у 9–11:
// пока выстрел едет (чок + пинг), сервер успевает выкинуть запись с края окна
[[nodiscard]] bool recordValid(float simulationTime, float serverTime, float latencyIn, float lerp, float maxUnlag, float interval, float margin = 0.0f) noexcept
{
    // сервер берёт lerp в целых тиках
    const float lerpTicks = std::floor(0.5f + lerp / interval) * interval;
    const float correct = std::clamp(latencyIn + lerpTicks, 0.0f, maxUnlag);
    // запас 10 мс: задержка, которую меряет сервер, дрожит относительно нашей — запись на самом краю окна
    // он сочтёт невалидной и откатит врага в другое время
    if (std::abs(correct - (serverTime - simulationTime)) >= 0.19f)
        return false;
    return simulationTime >= serverTime - maxUnlag + margin;
}

// ---------- прогноз для врагов вне окна отката ----------
// У заряженных DT/дефенсива simtime отстаёт на 150-450 мс: окно отката до записей не дотягивается, и сервер
// стреляет по своему текущему состоянию врага — оно новее нашей свежей записи на обновления, которые придут
// к нему, пока летит наш выстрел. Движение — по самим записям (смещение, поворот дугой, гравитация, земля).
// Сервер чаще берёт на обновление меньше расчёта: ставим модель между N−1 и N, «устойчивые» точки — по обеим.

// Общая поправка (в долях обновления) по проверкам промахов — пока своей у игрока мало. Старт −0.3:
// логи 04–09.11 — сервер брал меньше обновлений 21 раз, больше — 10
float aheadBias = -0.3f;

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

// step — полшага (половина смещения за обновление): модели N−1 и N = out ∓ step
// phaseOut — где внутри периода пакетов врага окажется наш выстрел на сервере (0..1, −1 — враг молчит);
// riskOut — фаза у границы: сервер может взять и N−1, и N, ±1 обновление — монетка
int predictAhead(const PlayerRecords& list, LagRecord& out, float realtime, float latency, float interval, float gravity, float bias, csgo::Vector& step,
                 float* phaseOut = nullptr, bool* riskOut = nullptr) noexcept
{
    const auto& newest = list.at(0);
    const int choke = typicalChoke(list);
    const float updateTime = static_cast<float>(choke) * interval;
    // дольше обычного молчит — не шлёт (зарядка DT) или simtime ушёл назад (дефенсив, запись не принята):
    // у сервера ровно наша запись. Запас на дрожание сети: не меньше периода + 30 мс
    const float silence = realtime - newest.receivedAt;
    // Сколько пакетов врага сервер успеет обработать, пока до него едет наш выстрел. Раньше — floor от
    // (тишина + пинг) / период по одной свежей записи: её время прихода дрожит на тик и больше, а выстрел,
    // пришедший на границу пакета, — монетка между N−1 и N. Логи 04–09.11: из 89 промахов прогноза
    // 31 — «сервер взял ±N обн.», 24 — «позиция верна, сервер был между обновлениями».
    // Теперь сетка прихода пакетов — по медиане остатков времён прихода нескольких записей по модулю
    // периода, а близость к границе отдаётся наружу: там рейдж ждёт середину периода.
    float anchor = newest.receivedAt;
    float jitter = interval;
    {
        std::array<float, 6> residual{};
        const int n = std::min(list.count, static_cast<int>(residual.size()));
        for (int age = 0; age < n; ++age)
            residual[age] = std::remainder(list.at(age).receivedAt - newest.receivedAt, updateTime);
        if (n >= 3) {
            auto sorted = residual;
            std::sort(sorted.begin(), sorted.begin() + n);
            const float median = sorted[n / 2];
            anchor += median;
            float spread = 0.0f;
            for (int k = 0; k < n; ++k)
                spread += std::abs(std::remainder(residual[k] - median, updateTime));
            jitter = spread / static_cast<float>(n) + interval * 0.5f;
        }
    }
    const bool silent = silence > std::max(updateTime * 1.5f, updateTime + 0.03f);
    // поправка — сдвиг фазы в долях обновления, а не целое число обновлений (раньше lround: 0.34 = 0)
    const float grid = std::clamp(realtime - anchor + latency, 0.0f, 0.5f) / updateTime + bias;
    const int updates = silent ? 0 : std::max(static_cast<int>(std::floor(grid)), 0);
    const float phase = silent ? -1.0f : grid - std::floor(grid);
    const float margin = std::clamp((jitter + interval) / updateTime, 0.1f, 0.45f);
    // период в тик — граница ничего не стоит (сдвиг за обновление крошечный)
    const bool risky = !silent && updateTime > interval * 1.5f && (phase < margin || phase > 1.0f - margin);
    if (phaseOut)
        *phaseOut = phase;
    if (riskOut)
        *riskOut = risky;
    const int ticks = std::min(updates * choke, static_cast<int>(0.3f / interval));

    // время между записями: у дефенсива simtime почти стоит, а враг идёт по настоящему времени —
    // берём большее из разницы simtime и времени между пакетами (минус тик дрожания приёма)
    const auto moveTime = [interval](const LagRecord& later, const LagRecord& earlier) {
        return std::max(later.simulationTime - earlier.simulationTime, later.receivedAt - earlier.receivedAt - interval);
    };
    auto velocity = newest.velocity;
    float turnRate = 0.0f;
    if (list.count >= 2) {
        const auto& previous = list.at(1);
        const float dt = moveTime(newest, previous);
        if (dt > 0.0f && dt <= 0.5f && (newest.origin - previous.origin).squareLength() < 64.0f * 64.0f) {
            velocity = (newest.origin - previous.origin) / dt;
            if (!(newest.flags & FL_ONGROUND))
                velocity.z -= 0.5f * gravity * dt;
            if (list.count >= 3) {
                const auto& older = list.at(2);
                const float dt2 = moveTime(previous, older);
                if (dt2 > 0.0f && dt2 <= 0.5f) {
                    const auto before = (previous.origin - older.origin) / dt2;
                    if (velocity.length2D() > 30.0f && before.length2D() > 30.0f) {
                        const float turn = std::remainder(std::atan2(velocity.y, velocity.x) - std::atan2(before.y, before.x), 2.0f * 3.14159265f);
                        turnRate = std::clamp(Helpers::rad2deg(turn) / (0.5f * (dt + dt2)), -300.0f, 300.0f);
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
    const bool onGround = newest.flags & FL_ONGROUND;
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

    step = csgo::Vector{};
    const csgo::Vector full{ velocity.x * updateTime, velocity.y * updateTime, onGround ? 0.0f : velocity.z * updateTime };
    if (updates > 0 && !risky) {
        // фаза в середине периода: сервер возьмёт ровно N-е обновление — модель на N, «устойчивые»
        // точки проверяем с допуском в четверть шага (дрожание сети)
        step = full * 0.25f;
    } else if (updates > 0) {
        step = full * 0.5f;
        position -= step;
    } else {
        // Враг молчит дольше обычного: считали, что у сервера ровно наша запись. Логи 31.10–01.11: так
        // 32 попадания на 36 промахов прогноза — у сервера то наша запись, то уже следующая. Модель ставим
        // посередине между ними, а точки берём только общие для обеих (stepSafe с шагом в полобновления):
        // раньше шаг тут был нулевой, и проверка общих точек для «+0» не работала вовсе.
        step = full * 0.5f;
        position += step;
    }
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
    out.simulationTime = newest.simulationTime + static_cast<float>(updates) * updateTime;
    return updates;
}

[[nodiscard]] int timeToTicks(float time, float interval) noexcept
{
    return static_cast<int>(0.5f + time / interval);
}

// ---------- геометрия: хитбоксы-капсулы ----------

struct Capsule {
    csgo::Vector a, b;
    float radius;
    int group;
};
struct Capsules {
    std::array<Capsule, 32> items;
    int count = 0;
};

[[nodiscard]] csgo::Vector normalized(csgo::Vector v) noexcept
{
    const float length = v.length();
    return length > 0.0f ? v / length : v;
}

// кратчайшее расстояние между отрезками p1-q1 и p2-q2; s — где на первом ближайшая точка (0..1)
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

void shiftCapsules(Capsules& caps, const csgo::Vector& delta) noexcept
{
    for (int k = 0; k < caps.count; ++k) {
        caps.items[k].a += delta;
        caps.items[k].b += delta;
    }
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

// хитгруппа первого хитбокса на луче, −1 — мимо модели
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
        const float entry = s * length - std::sqrt(std::max(capsule.radius * capsule.radius - distance * distance, 0.0f));
        if (entry < nearest) {
            nearest = entry;
            group = capsule.group;
        }
    }
    return group;
}

bool rayHits(const csgo::Vector& start, const csgo::Vector& end, const Capsules& capsules) noexcept
{
    for (int i = 0; i < capsules.count; ++i) {
        float s;
        if (segmentDistance(start, end, capsules.items[i].a, capsules.items[i].b, s) <= capsules.items[i].radius)
            return true;
    }
    return false;
}

// ---------- разброс: тот же генератор, что у игры ----------
// Копия CUniformRandomStream из vstdlib (проверено офлайн: 1024 из 1024 чисел совпали).

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

// FX_FireBullets: отклонение = a·inaccuracy по углу b + c·spread по углу d
struct SpreadSeed {
    float a, cosB, sinB;
    float c, cosD, sinD;
};

// дробовик: дробина k берёт сид выстрела + k (FX_FireBullets: iSeed++ на каждую) — запас на 16 дробин
constexpr int SPREAD_TABLE = HITCHANCE_SEEDS + 16;

const std::array<SpreadSeed, SPREAD_TABLE>& spreadSeeds() noexcept
{
    static const auto table = [] {
        std::array<SpreadSeed, SPREAD_TABLE> result{};
        UniformRandomStream stream;
        for (int i = 0; i < SPREAD_TABLE; ++i) {
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

// Хитшанс: % сидов, при которых пуля по скелету цели даст не меньше minDamage
// (первый хитбокс на луче решает, куда придёт пуля: целился в голову, попал в грудь — тоже засчитано).
// У дробовика урон всех дробин этого сида складывается.
int hitchance(const csgo::Entity& weapon, const csgo::WeaponInfo* weaponData, const csgo::Entity& entity, const csgo::Vector& eye, const csgo::Vector& angle,
              const Capsules& capsules, float baseDamage, int minDamage) noexcept
{
    const float inaccuracy = weapon.getInaccuracy();
    float spread = weapon.weaponMode() == 1 ? weaponData->spreadAlt : weaponData->spread;
    if (!(spread >= 0.0f && spread < 0.2f))
        spread = 0.0f;
    const bool negev = weapon.itemDefinitionIndex() == WeaponId::Negev;
    const int recoilIndex = weapon.recoilIndex();
    const float sp = std::sin(Helpers::deg2rad(angle.x)), cp = std::cos(Helpers::deg2rad(angle.x));
    const float sy = std::sin(Helpers::deg2rad(angle.y)), cy = std::cos(Helpers::deg2rad(angle.y));
    const csgo::Vector forward{ cp * cy, cp * sy, -sp };
    const csgo::Vector right{ sy, -cy, 0.0f };
    const csgo::Vector up{ sp * cy, sp * sy, cp };
    // урон одной пули/дробины по хитгруппе
    std::array<float, 11> groupDamage{};
    for (int group = 0; group < static_cast<int>(groupDamage.size()); ++group)
        groupDamage[group] = autowall::scaleDamage(entity, group, weaponData, baseDamage);
    const int bullets = std::clamp(weaponData->bullets, 1, SPREAD_TABLE - HITCHANCE_SEEDS);
    const auto& seeds = spreadSeeds();

    int hits = 0;
    for (int i = 0; i < HITCHANCE_SEEDS; ++i) {
        float total = 0.0f;
        for (int pellet = 0; pellet < bullets; ++pellet) {
            const auto& seed = seeds[i + pellet];
            float a = seed.a, c = seed.c;
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
            if (group >= 0)
                total += group < static_cast<int>(groupDamage.size()) ? groupDamage[group] : baseDamage;
        }
        if (total >= static_cast<float>(minDamage))
            ++hits;
    }
    return hits * 100 / HITCHANCE_SEEDS;
}

// ---------- автостоп ----------

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

// «Точность»: разброс не растёт, пока скорость ниже ~34% максимальной
[[nodiscard]] float autoStopTarget(AutoStop mode, float maxSpeed) noexcept
{
    return mode == AutoStop::Accuracy ? maxSpeed * 0.33f : 0.0f;
}

[[nodiscard]] float afterFriction(float speed, const MovementVars& vars, float interval) noexcept
{
    return std::max(0.0f, speed - std::max(speed, vars.stopSpeed) * vars.friction * interval);
}

// за сколько тиков трение + встречный ввод погасят скорость: за столько до готовности оружия и тормозим
[[nodiscard]] int ticksToStop(float speed, float targetSpeed, float maxSpeed, const MovementVars& vars, float interval) noexcept
{
    int ticks = 0;
    while (speed > targetSpeed + 1.0f && ticks < 16) {
        speed = std::max(targetSpeed, afterFriction(speed, vars, interval) - vars.accelerate * interval * maxSpeed);
        ++ticks;
    }
    return ticks;
}

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
        const float move = std::hypot(cmd->forwardmove, cmd->sidemove);
        if (move > targetSpeed && move > 0.0f) {
            cmd->forwardmove *= targetSpeed / move;
            cmd->sidemove *= targetSpeed / move;
        }
        return;
    }
    // встречный ввод ровно на остаток скорости (больше — перелёт назад и снова разброс)
    const float wish = std::clamp((nextSpeed - targetSpeed) / std::max(vars.accelerate * interval, 0.001f), 0.0f, 450.0f);
    const float relative = Helpers::deg2rad(velocity.toAngle().y - cmd->viewangles.y);
    cmd->forwardmove = -std::cos(relative) * wish;
    cmd->sidemove = std::sin(relative) * wish;
}

// ---------- оружие ----------

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

constexpr std::array<std::pair<int, std::array<Hitbox, 4>>, 7> hitboxGroups{ {
    { HB_Head, { Hitbox::Head, Hitbox::Max, Hitbox::Max, Hitbox::Max } },
    { HB_Chest, { Hitbox::UpperChest, Hitbox::Thorax, Hitbox::LowerChest, Hitbox::Max } },
    { HB_Stomach, { Hitbox::Belly, Hitbox::Max, Hitbox::Max, Hitbox::Max } },
    { HB_Pelvis, { Hitbox::Pelvis, Hitbox::Max, Hitbox::Max, Hitbox::Max } },
    { HB_Arms, { Hitbox::RightUpperArm, Hitbox::LeftUpperArm, Hitbox::RightForearm, Hitbox::LeftForearm } },
    { HB_Legs, { Hitbox::RightThigh, Hitbox::LeftThigh, Hitbox::RightCalf, Hitbox::LeftCalf } },
    { HB_Feet, { Hitbox::RightFoot, Hitbox::LeftFoot, Hitbox::Max, Hitbox::Max } },
} };

// Фристенд-догадка: десинкеры прячут настоящую голову — за стену, а в открытую — подальше от стрелка.
// +1 — тело на стороне «взгляд + десинк».
float freestandSide(const csgo::EngineTrace& engineTrace, const csgo::Vector& eye, const LagRecord& record) noexcept
{
    const auto headPlus = record.plus[HEAD_BONE].origin();
    const auto headMinus = record.minus[HEAD_BONE].origin();
    const auto exposed = [&](const csgo::Vector& point) {
        csgo::Trace trace;
        engineTrace.traceRay({ eye, point }, 0x1400B, { localPlayer.get().getPOD() }, trace);   // только стены
        return trace.fraction > 0.97f;
    };
    // помимо головы — грудь (кость на ~28 ед. ниже головы): по одной точке «открыто/закрыто» решается случайно
    int chestBone = 3;
    float chestDistance = 1e9f;
    for (int bone = 1; bone < HEAD_BONE; ++bone) {
        const float distance = std::abs(record.plus[bone].origin().z - (headPlus.z - 28.0f));
        if (distance < chestDistance) {
            chestDistance = distance;
            chestBone = bone;
        }
    }
    const int plusOpen = (exposed(headPlus) ? 1 : 0) + (exposed(record.plus[chestBone].origin()) ? 1 : 0);
    const int minusOpen = (exposed(headMinus) ? 1 : 0) + (exposed(record.minus[chestBone].origin()) ? 1 : 0);
    if (plusOpen != minusOpen)
        return plusOpen < minusOpen ? 1.0f : -1.0f;
    return (headPlus - eye).squareLength() > (headMinus - eye).squareLength() ? 1.0f : -1.0f;
}

// ---------- выстрелы: что запомнили, чтобы потом понять, почему промах ----------

enum class SideSource { None, Bot, NoDesync, Air, Layers, Evidence, Jitter, Guess, Brute };
constexpr const char* sideSourceNames[]{ "без резольвера", "бот", "не десинкает", "в воздухе", "слои", "память", "джиттер", "догадка", "перебор" };

struct ShotRecord {
    float time = 0.0f;              // realtime выстрела, 0 — ждать нечего
    csgo::Vector eye{};
    csgo::Vector aimPoint{};
    bool isHead = false;
    bool edge = false;
    int damage = 0;
    int hitchance = 0;
    float side = 0.0f;              // +1/−1 — по какой стороне стреляли, 0 — без резольвера
    SideSource source = SideSource::None;
    float phase = 0.0f;             // фаза джиттера на момент записи
    float guess = 0.0f;             // что сказала фристенд-догадка (0 — не спрашивали)
    float layerDelta = 0.0f;        // слои: угол ног от взгляда, по которому собран скелет
    float layerMax = 0.0f;          // ...и максимум десинка на той записи
    float layerConfidence = 0.0f;
    bool animFixed = false;         // скелет собран прогоном анимации (анимфикс), а не подменой поз
    bool sideSafe = false;          // точка попадает и в найденный угол, и в максимум той же стороны
    bool safe = false;              // точка общая для всех трёх скелетов (промах по ней — модель вне всех трёх)
    bool flipping = false;          // ноги по слоям перекидываются между обновлениями (джиттер десинка)
    RecordKind kind = RecordKind::Live;
    int backtrackTicks = 0;
    int updatesAhead = 0;
    float aheadPhase = -1.0f;       // фаза выстрела внутри периода пакетов врага (прогноз)
    float headScore = -1.0f, bodyScore = -1.0f;
    float predictedSimulationTime = 0.0f;
    csgo::Vector predictedOrigin{}; // где стояла модель, по которой стреляли
    bool jitter = false;            // враг джиттерил: сервер мог взять другую фазу позы
    float updateTime = 0.0f;
    const csgo::StudioHitboxSet* set = nullptr;
    Capsules aimed;                 // скелет, по которому стреляли
    Capsules plus, minus;           // крайние скелеты той же записи
    bool haveImpact = false;
    csgo::Vector impact{};
};
std::array<ShotRecord, 65> shots{};
int lastShotIndex = 0;

struct ImpactListener : csgo::GameEventListener {
    void fireGameEvent(csgo::GameEventPOD* eventPointer) override
    {
        const auto event = csgo::GameEvent::from(retSpoofGadgets->client, eventPointer);
        if (!engine || !localPlayer || !memoryPtr || engine->getPlayerForUserID(event.getInt("userid")) != localPlayer.get().getNetworkable().index())
            return;
        if (lastShotIndex <= 0)
            return;
        auto& shot = shots[lastShotIndex];
        if (shot.time <= 0.0f || memoryPtr->globalVars->realtime - shot.time > 0.5f)
            return;
        const csgo::Vector position{ event.getFloat("x"), event.getFloat("y"), event.getFloat("z") };
        // пробития дают несколько импактов — нужен самый дальний
        if (!shot.haveImpact || (position - shot.eye).squareLength() > (shot.impact - shot.eye).squareLength()) {
            shot.impact = position;
            shot.haveImpact = true;
        }
    }
};
ImpactListener impactListener;
bool impactRegistered = false;

// доказанная сторона против того, что говорила догадка (счёт с потолком, старое вытесняется)
void scoreGuess(Brain& brain, float guess, float truth) noexcept
{
    if (guess == 0.0f || truth == 0.0f)
        return;
    std::uint8_t& counter = (guess > 0.0f) == (truth > 0.0f) ? brain.guessRight : brain.guessWrong;
    counter = static_cast<std::uint8_t>(std::min(counter + 1, 12));
    if (brain.guessRight + brain.guessWrong > 12) {
        brain.guessRight = static_cast<std::uint8_t>(brain.guessRight * 3 / 4);
        brain.guessWrong = static_cast<std::uint8_t>(brain.guessWrong * 3 / 4);
    }
}

// По лучу пули видно, на какой стороне стояла модель: луч проходит сквозь один крайний скелет и мимо другого.
// landed — пуля попала (модель была там, где луч), иначе промах (модель была там, где луча нет).
[[nodiscard]] float provenSide(const ShotRecord& shot, bool landed) noexcept
{
    if (!shot.haveImpact || shot.plus.count == 0 || shot.minus.count == 0)
        return 0.0f;
    const auto end = shot.eye + normalized(shot.impact - shot.eye) * 8192.0f;
    const bool plus = rayHits(shot.eye, end, shot.plus);
    const bool minus = rayHits(shot.eye, end, shot.minus);
    if (plus == minus)
        return 0.0f;
    return (plus == landed) ? 1.0f : -1.0f;
}

[[nodiscard]] std::string shotText(const ShotRecord& shot) noexcept
{
    char text[256];
    int length = std::snprintf(text, sizeof(text), "%s%s, сторона %s (%s), запись %s", shot.isHead ? "голова" : "тело", shot.edge ? " (край)" : "",
        shot.side > 0.0f ? "+" : shot.side < 0.0f ? "−" : "0", sideSourceNames[static_cast<int>(shot.source)], kindNames[static_cast<int>(shot.kind)]);
    if (shot.source == SideSource::Layers && length > 0 && length < static_cast<int>(sizeof(text)))
        length += std::snprintf(text + length, sizeof(text) - length, " [ноги %+.0f°/%.0f°, %.0f%%%s]", shot.layerDelta, shot.layerMax,
            shot.layerConfidence * 100.0f, shot.sideSafe ? ", общая точка" : "");
    if (shot.animFixed && length > 0 && length < static_cast<int>(sizeof(text)))
        length += std::snprintf(text + length, sizeof(text) - length, " [анимфикс]");
    if (shot.flipping && length > 0 && length < static_cast<int>(sizeof(text)))
        length += std::snprintf(text + length, sizeof(text) - length, " [флипы]");
    if (shot.safe && length > 0 && length < static_cast<int>(sizeof(text)))
        length += std::snprintf(text + length, sizeof(text) - length, " [безопасная]");
    if (length > 0 && length < static_cast<int>(sizeof(text))) {
        if (shot.kind == RecordKind::Ahead || shot.kind == RecordKind::Extrapolated)
            length += shot.aheadPhase >= 0.0f
                ? std::snprintf(text + length, sizeof(text) - length, " +%d обн. (фаза %.2f)", shot.updatesAhead, shot.aheadPhase)
                : std::snprintf(text + length, sizeof(text) - length, " +%d обн.", shot.updatesAhead);
        else if (shot.backtrackTicks > 0)
            length += std::snprintf(text + length, sizeof(text) - length, " бт %dт", shot.backtrackTicks);
    }
    // выстрел в тело, а голова тоже была — с какими оценками (чтобы видеть, почему не голова)
    if (!shot.isHead && shot.headScore >= 0.0f && length > 0 && length < static_cast<int>(sizeof(text)))
        length += std::snprintf(text + length, sizeof(text) - length, " [голова %.2f / тело %.2f]", shot.headScore, shot.bodyScore);
    if (length > 0 && length < static_cast<int>(sizeof(text)))
        std::snprintf(text + length, sizeof(text) - length, ", урон %d, хш %d%%", shot.damage, shot.hitchance);
    return text;
}

// Промах: причина по импакту пули и обучение резольвера
void evaluateMiss(int index) noexcept
{
    auto& shot = shots[index];
    auto& brain = brains[index];
    const float realtime = memoryPtr->globalVars->realtime;
    if (index == lastShotIndex) {
        std::scoped_lock lock{ viewMutex };
        lastShot.result = -1;
    }

    const char* reason = nullptr;
    char detail[160] = "";
    bool positionRight = false;   // прогноз позиции подтвердился — промах ушёл в резольвер
    // промахи подряд (сжимают мультипоинт, включают безопасные точки) — только те, где виновата цель:
    // разброс, сторона, прогноз; смерть, лаг и стена тут ни при чём
    if (!shot.haveImpact) {
        reason = localDiedAt >= shot.time ? "нас убили раньше, чем сервер принял выстрел" : "сервер не засчитал выстрел (лаг)";
    } else if ((shot.impact - shot.eye).length() + 8.0f < (shot.aimPoint - shot.eye).length()) {
        reason = "пуля застряла раньше цели (автоволл)";
    } else {
        ++brain.misses;
        if (!rayHits(shot.eye, shot.eye + normalized(shot.impact - shot.eye) * 8192.0f, shot.aimed)) {
            reason = "разброс";
        } else if ((shot.kind == RecordKind::Ahead || shot.kind == RecordKind::Extrapolated) && shot.set && shot.updateTime > 0.0f) {
            // прогноз: в какую из пришедших потом записей пуля попала бы
            const auto& list = records[index];
            const auto end = shot.eye + normalized(shot.impact - shot.eye) * 8192.0f;
            float closest = 1e9f;
            static Capsules test;
            for (int age = 0; age < list.count; ++age) {
                const auto& record = list.at(age);
                const float offset = (record.simulationTime - shot.predictedSimulationTime) / shot.updateTime;
                if (std::abs(offset) > 3.5f || std::abs(offset) >= std::abs(closest))
                    continue;
                const csgo::matrix3x4* variants[]{ record.bones, record.plus, record.minus };
                for (int v = 0; v < (record.variants ? 3 : 1); ++v) {
                    buildCapsules(*shot.set, variants[v], test);
                    if (rayHits(shot.eye, end, test)) {
                        closest = offset;
                        break;
                    }
                }
            }
            const int rounded = closest < 1e8f ? static_cast<int>(std::lround(closest)) : 99;
            // Луч лёг в пришедшую потом запись ровно на предсказанное время — позиция угадана (по логам 31.10–01.11
            // так 19 из 59 «промахов прогноза»: «сервер взял +0»). Если при этом одна крайняя сторона десинка
            // под лучом, а другая нет — модель на сервере была на другой стороне: это промах резольвера,
            // и его надо учить, а не списывать на прогноз (раньше такие промахи не учили ни то, ни другое).
            if (rounded == 0 && provenSide(shot, false) != 0.0f) {
                positionRight = true;
                ++brain.aheadChecks;
            } else if (closest < 1e8f) {
                const int offset = rounded;
                // Модель стоит между N−1 и N, но если промах лёг в N−1 или N+1 — сервер был не там, где
                // мы ждали, и поправку надо сдвигать (раньше −1 считался «и так внутри», и по логам 31.10
                // «сервер взял −1/−2» шли подряд, не меняя прогноз). 0 — сервер брал то же, что и мы.
                if (offset != 0) {
                    aheadBias = std::clamp(aheadBias + (offset > 0 ? 0.34f : -0.34f), -2.0f, 3.0f);
                    brain.aheadBias = std::clamp(brain.aheadBias + (offset > 0 ? 0.5f : -0.5f), -2.0f, 3.0f);
                }
                ++brain.aheadChecks;
                reason = "прогноз";
                if (offset == 0)
                    std::snprintf(detail, sizeof(detail), " (позиция верна, сервер был между обновлениями)");
                else
                    std::snprintf(detail, sizeof(detail), " (сервер взял %+d обн.)", offset);
            } else if (shot.jitter && [&] {
                           // позиция была верной, а поза — от другой фазы джиттера: поза пришедших записей,
                           // перенесённая на нашу прогнозную позицию, ложится под луч
                           for (int age = 0; age < list.count; ++age) {
                               const auto& record = list.at(age);
                               if (std::abs(record.simulationTime - shot.predictedSimulationTime) > shot.updateTime * 3.5f)
                                   continue;
                               buildCapsules(*shot.set, record.bones, test);
                               shiftCapsules(test, shot.predictedOrigin - record.origin);
                               if (rayHits(shot.eye, end, test))
                                   return true;
                           }
                           return false;
                       }()) {
                reason = "джиттер (сервер был в другой фазе)";
            } else {
                // ни одна пришедшая потом запись не легла под луч — враг сменил движение. Сторону тут не доказать:
                // крайние скелеты стояли там же, где неверная позиция, — резольвер на этом не учим
                reason = "прогноз (враг сменил движение)";
            }
        }
    }

    if (!reason) {
        // пуля прошла сквозь нашу модель, а на сервере модели там не было — сторона десинка
        reason = positionRight ? "резольвер (прогноз позиции верный)" : "резольвер";
        if (shot.isHead)
            brain.headResolverMisses = static_cast<std::uint8_t>(std::min(brain.headResolverMisses + 1, 9));
        const float proven = provenSide(shot, false);
        if (shot.side == 0.0f && shot.source == SideSource::NoDesync)
            brain.desyncProven = true;
        scoreGuess(brain, shot.guess, proven);
        if (proven != 0.0f) {
            std::snprintf(detail, sizeof(detail), " (модель была на %s)", proven > 0.0f ? "+" : "−");
            if (shot.source == SideSource::Jitter)
                addJitterLink(brain, proven, shot.phase, 3.0f);
            else
                addEvidence(brain, proven, 4.0f, realtime);
            brain.bruteSide = proven;
            brain.bruteUntil = realtime + 2.0f;
            brain.bruteProven = true;
        } else if (shot.side != 0.0f) {
            if (shot.source == SideSource::Jitter)
                addJitterLink(brain, -shot.side, shot.phase, 1.5f);
            else
                addEvidence(brain, -shot.side, 2.0f, realtime);
            brain.bruteSide = -shot.side;
            brain.bruteUntil = realtime + 1.5f;
            brain.bruteProven = false;
        }
        if (shot.source == SideSource::Layers) {
            // Импакт показал, что модель была на той же стороне, по которой стреляли: сторона верна,
            // не хватило величины угла (слои нашли ноги ближе к взгляду, чем на сервере). Такой промах
            // не должен штрафовать слои и уводить резольвер в зеркало — он учит доводить угол до максимума.
            if (proven != 0.0f && shot.side != 0.0f && proven == shot.side) {
                hvh::resolver::onMagnitudeMiss(index);
                std::snprintf(detail, sizeof(detail), " (модель была на %s: сторона верна, ноги дальше — доводим до максимума)", proven > 0.0f ? "+" : "−");
            } else {
                hvh::resolver::onMiss(index, proven != 0.0f);
            }
        }
    }

    char text[640];
    std::snprintf(text, sizeof(text), "[newerwin] промах по %s: %s%s | %s", playerName(index).c_str(), reason, detail, shotText(shot).c_str());
    static constexpr std::uint8_t red[4]{ 255, 110, 110, 255 };
    report(red, text);
    if (shot.haveImpact)
        extras::onShotResult(shot.impact, false);
    shot.time = 0.0f;
}

// Слои дали ноги почти по взгляду и без уверенности — сторона там случайна (лог 31.10, новая сборка:
// все 3 промаха по слоям в голову — «+6°/29° 3%», «−5°/35° 30%» и т. п.). Такую находку не берём,
// сторона идёт из памяти/перебора/догадки, а у них по логам голова без промахов резольвера.
[[nodiscard]] bool layerSideUsable(const LagRecord& record) noexcept
{
    const float delta = std::abs(std::remainder(record.resolvedYaw - record.eyeAngles.y, 360.0f));
    const float limit = std::max(record.maxDesync, 1.0f);
    return record.resolvedConfidence >= 0.35f || delta >= 0.4f * limit;
}

// ---------- кандидаты-записи и выбор точки ----------

struct AimPoint {
    csgo::Vector position{};
    float damage = 0.0f;        // с множителем хитгруппы и бронёй
    float baseDamage = 0.0f;    // долетает до точки без множителя — для хитшанса
    bool isHead = false;
    bool edge = false;
    int group = 0;
    bool sideSafe = false;      // общая для найденного угла и максимума той же стороны
    bool safe = false;          // общая для всех трёх скелетов
};

struct Target {
    int index = 0;
    AimPoint point;
    AimPoint fallback;          // вторая группа (тело вместо головы и наоборот) — если по первой нет хитшанса
    float score = -1.0f;
    float reliability = 1.0f;   // оценка шанса, что модель верна (без хитшанса разброса)
    float fov = 0.0f;
    int minDamage = 0;
    RecordKind kind = RecordKind::Live;
    float simulationTime = 0.0f;
    csgo::Vector origin{};
    int backtrackTicks = 0;
    int updatesAhead = 0;
    float aheadPhase = -1.0f;
    float headScore = -1.0f, bodyScore = -1.0f;   // за что выбрали зону (для лога)
    int choke = 1;
    float side = 0.0f;
    SideSource source = SideSource::None;
    float phase = 0.0f;
    float guess = 0.0f;
    float layerDelta = 0.0f;
    float layerMax = 0.0f;
    float layerConfidence = 0.0f;
    bool animFixed = false;
    bool flipping = false;
    const csgo::StudioHitboxSet* set = nullptr;
    const csgo::StudioHdr* studio = nullptr;
    csgo::matrix3x4 bones[RECORD_BONES];
    csgo::matrix3x4 plus[RECORD_BONES];
    csgo::matrix3x4 minus[RECORD_BONES];
    bool haveVariants = false;
};

[[nodiscard]] const char* sideLabel(SideSource source, float side) noexcept
{
    switch (source) {
    case SideSource::Bot: return "бот";
    case SideSource::NoDesync: return "нет десинка";
    case SideSource::Air: return "воздух";
    case SideSource::Layers: return side >= 0.0f ? "слои +" : "слои −";
    case SideSource::Evidence: return side > 0.0f ? "память +" : "память −";
    case SideSource::Jitter: return side > 0.0f ? "джиттер +" : "джиттер −";
    case SideSource::Guess: return side > 0.0f ? "догадка +" : "догадка −";
    case SideSource::Brute: return side > 0.0f ? "перебор +" : "перебор −";
    default: return "";
    }
}

bool runImpl(csgo::UserCmd* cmd, const EngineInterfaces& engineInterfaces, const ClientInterfaces& clientInterfaces, const OtherInterfaces& interfaces, const Memory& memory) noexcept;
float lerpTime = 0.0f;
bool revolverRelease = false;   // R8 взведён, а выстрела этим тиком нет — отпустить ЛКМ, иначе выстрелит в пустоту

} // namespace

// ================= публичное =================

bool minDamageOverrideActive() noexcept { return minDamageOverride; }
bool forceBodyActive() noexcept { return forceBody; }
bool forceShotActive() noexcept { return forceShot; }
int currentTarget() noexcept { return target; }
bool aimingNow() noexcept
{
    // рейдж не запускался пару тиков (нож, граната, мёртв) — считаем, что не целимся
    const int now = memoryPtr ? memoryPtr->globalVars->tickCount : 0;
    return target != 0 && fireReadyNow && now - fireReadyTick <= 2;
}
const char* status() noexcept { return statusText; }

float resolverOffsetFor(int index) noexcept
{
    if (index <= 0 || index >= 65 || !memoryPtr)
        return 0.0f;
    if (hvh::resolver::valid(index)) {
        const float limit = std::max(hvh::resolver::maxDesync(index), 1.0f);
        return std::clamp(hvh::resolver::delta(index) / limit, -1.0f, 1.0f);
    }
    const float evidence = sideEvidence(brains[index], memoryPtr->globalVars->realtime);
    return std::clamp(evidence / 3.0f, -1.0f, 1.0f);
}

TargetView targetView(int index) noexcept
{
    if (index <= 0 || index >= static_cast<int>(views.size()))
        return {};
    std::scoped_lock lock{ viewMutex };
    return views[index];
}

ShotView shotView() noexcept
{
    std::scoped_lock lock{ viewMutex };
    return lastShot;
}

void onPlayerHurt(int victimIndex, int attackerIndex, int localIndex, int hitgroup, int damage, int healthLeft) noexcept
{
    // попали по нам — тихо, только в файл: куда и что в этот момент делал наш AA (для подстройки конфига)
    if (victimIndex == localIndex && attackerIndex != localIndex && attackerIndex > 0 && attackerIndex < 65) {
        static constexpr const char* groups[]{ "тело", "голова", "грудь", "живот", "левая рука", "правая рука", "левая нога", "правая нога", "шея" };
        float pitch = 0.0f, yaw = 0.0f;
        const bool sent = hvh::antiaim::sentAngles(pitch, yaw);
        float speed = 0.0f;
        bool onGround = true;
        if (localPlayer && localPlayer.get().isAlive()) {
            speed = localPlayer.get().velocity().length2D();
            onGround = (localPlayer.get().flags() & 1) != 0;
        }
        char text[384];
        std::snprintf(text, sizeof(text), "[по нам] %s: %s −%d (осталось %d) | AA: %s, сторона %+d%s%s, pitch %.0f, %s, скорость %.0f, окно LC %d, заряд %d, нас видят: %s",
            playerName(attackerIndex).c_str(), hitgroup >= 0 && hitgroup < 9 ? groups[hitgroup] : "?", damage, healthLeft,
            hvh::antiaim::stateName(), hvh::antiaim::desyncSide(), hvh::antiaim::defensiveActive() ? ", скрытые углы" : "",
            hvh::antiaim::isFreestanding() ? ", фристенд" : "", sent ? pitch : 0.0f, onGround ? "земля" : "воздух", speed,
            defensive::windowLeft(), tickbase::chargedTicks(), tickbase::threatened() ? "да" : "нет");
        fileLog(text);
        return;
    }
    if (attackerIndex != localIndex || victimIndex <= 0 || victimIndex >= 65 || !memoryPtr)
        return;
    auto& shot = shots[victimIndex];
    auto& brain = brains[victimIndex];
    brain.misses = 0;
    if (shot.time <= 0.0f)
        return;
    const float realtime = memoryPtr->globalVars->realtime;
    if (victimIndex == lastShotIndex) {
        std::scoped_lock lock{ viewMutex };
        lastShot.result = 1;
    }
    extras::onShotResult(shot.haveImpact ? shot.impact : shot.aimPoint, true);

    // попадание — свидетельство: геометрией (луч прошёл только сквозь один крайний скелет) или стороной выстрела
    const float proven = provenSide(shot, true);
    const float side = proven != 0.0f ? proven : shot.side;
    scoreGuess(brain, shot.guess, side);
    const float weight = proven != 0.0f ? 3.0f : 1.5f;
    if (shot.source == SideSource::Jitter)
        addJitterLink(brain, side, shot.phase, weight);
    else if (shot.source != SideSource::NoDesync && shot.source != SideSource::Bot)
        addEvidence(brain, side, weight, realtime);
    if (shot.source == SideSource::Layers)
        hvh::resolver::onHit(victimIndex);
    brain.bruteSide = 0.0f;
    if (hitgroup == 1)
        brain.headResolverMisses = 0;
    std::uint8_t& hits = shot.isHead ? brain.headHits : brain.bodyHits;
    hits = static_cast<std::uint8_t>(std::min<int>(hits + 1, 250));

    static constexpr const char* hitgroups[]{ "тело", "голова", "грудь", "живот", "левая рука", "правая рука", "левая нога", "правая нога", "шея" };
    char text[640];
    std::snprintf(text, sizeof(text), "[newerwin] попал в %s: %s −%d (осталось %d) | %s", playerName(victimIndex).c_str(),
        hitgroup >= 0 && hitgroup < 9 ? hitgroups[hitgroup] : "?", damage, healthLeft, shotText(shot).c_str());
    static constexpr std::uint8_t green[4]{ 120, 230, 120, 255 };
    report(green, text);
    shot.time = 0.0f;
}

void updateEventListener(const EngineInterfaces& engineInterfaces, const Memory& memory, bool forceRemove) noexcept
{
    const bool wanted = !forceRemove && config.rage.enabled;
    if (wanted && !impactRegistered) {
        engineInterfaces.getGameEventManager(memory.getEventDescriptor).addListener(&impactListener, csgo::bullet_impact);
        impactRegistered = true;
    } else if (!wanted && impactRegistered) {
        engineInterfaces.getGameEventManager(memory.getEventDescriptor).removeListener(&impactListener);
        impactRegistered = false;
    }
}

void captureNetworkAngles(const EngineInterfaces& engineInterfaces, const ClientInterfaces& clientInterfaces) noexcept
{
    if (!config.rage.enabled)
        return;
    const int maxClients = std::min(engineInterfaces.getEngine().getMaxClients(), 64);
    for (int i = 1; i <= maxClients; ++i) {
        const auto entity = csgo::Entity::from(retSpoofGadgets->client, clientInterfaces.getEntityList().getEntity(i));
        if (entity.getPOD() == nullptr || entity.getNetworkable().isDormant()) {
            network[i].simulationTime = -1.0f;
            continue;
        }
        if (entity.simulationTime() == network[i].simulationTime)
            continue;   // пакета на него не было — в поле интерполированный угол
        network[i] = { entity.simulationTime(), entity.eyeAngles() };
        if (const auto* layers = entity.animOverlays(); layers && entity.animOverlayCount() > csgo::LAYER_ALIVELOOP) {
            const auto& alive = layers[csgo::LAYER_ALIVELOOP];
            network[i].aliveCycle = alive.cycle;
            network[i].aliveRate = alive.playbackRate;
            network[i].aliveSequence = alive.sequence;
        }
    }
}

void drawLogs(ImDrawList* drawList) noexcept
{
    if (!config.view.screenLogs || !drawList)
        return;
    constexpr double lifetime = 6.0, fade = 1.0;
    const double now = steadySeconds();
    ImVec2 position{ 8.0f, 6.0f };
    std::scoped_lock lock{ screenMutex };
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
    if (victimIndex == localIndex && memoryPtr)
        localDiedAt = memoryPtr->globalVars->realtime;
}

void onRoundStart() noexcept
{
    for (auto& shot : shots)
        shot.time = 0.0f;
    for (auto& brain : brains) {
        brain.misses = 0;
        brain.bruteSide = 0.0f;
        brain.headResolverMisses = 0;    // новый раунд — голова снова в деле (иначе голову не били полкарты)
    }
    hvh::resolver::onRoundStart();
}

bool run(csgo::UserCmd* cmd, const EngineInterfaces& engineInterfaces, const ClientInterfaces& clientInterfaces, const OtherInterfaces& interfaces, const Memory& memory, float lerp) noexcept
{
    lerpTime = lerp;
    revolverRelease = false;
    const bool shot = runImpl(cmd, engineInterfaces, clientInterfaces, interfaces, memory);
    if (!shot && revolverRelease)
        cmd->buttons &= ~csgo::UserCmd::IN_ATTACK;
    return shot;
}

namespace
{

// ---------- кнайфбот ----------
// Нож бьёт трассой из глаз: ЛКМ на 48 ед., ПКМ (удар) на 32 ед. Урон: ЛКМ 25 (первый взмах 40 — не
// рассчитываем на него), ПКМ 65; в спину — 90 и 180. Броня режет урон ножа примерно до 85%.
// В спину — когда мы смотрим врагу в спину: направление на него совпадает с его взглядом (как в игре: > 0.475).
// Лагкомп для ножа тот же, что для пуль: бьём по записи и ставим tickcount под неё.
bool knifeBot(csgo::UserCmd* cmd, const csgo::Entity& weapon, const EngineInterfaces& engineInterfaces, const ClientInterfaces& clientInterfaces,
    const Memory& memory, float maxUnlag) noexcept
{
    const auto& rage = config.rage;
    if (!rage.knifeBot || (!rage.autoFire && !(cmd->buttons & (csgo::UserCmd::IN_ATTACK | csgo::UserCmd::IN_ATTACK2))))
        return false;
    const float serverTime = memory.globalVars->serverTime();
    const float interval = memory.globalVars->intervalPerTick;
    if (localPlayer.get().nextAttack() > serverTime)
        return false;
    const bool primaryReady = weapon.nextPrimaryAttack() <= serverTime;
    const bool secondaryReady = weapon.nextSecondaryAttack() <= serverTime;
    if (!primaryReady && !secondaryReady)
        return false;

    float latencyIn = 0.0f;
    if (const auto channel = engineInterfaces.getEngine().getNetworkChannel())
        latencyIn = csgo::NetworkChannel::from(retSpoofGadgets->client, channel).getLatency(1);

    constexpr float primaryRange = 48.0f, secondaryRange = 32.0f;
    const auto eye = localPlayer.get().getEyePosition();
    struct Best {
        int index = 0;
        const LagRecord* record = nullptr;
        csgo::Vector point{};
        bool secondary = false;
        bool lethal = false;
        float distance = 1e9f;
    } best;
    static Capsules caps;

    const int maxClients = std::min(engineInterfaces.getEngine().getMaxClients(), 64);
    for (int i = 1; i <= maxClients; ++i) {
        const auto entity = csgo::Entity::from(retSpoofGadgets->client, clientInterfaces.getEntityList().getEntity(i));
        if (entity.getPOD() == nullptr || entity.getPOD() == localPlayer.get().getPOD() || entity.getNetworkable().isDormant()
            || !entity.isAlive() || !entity.isOtherEnemy(memory, localPlayer.get()) || entity.gunGameImmunity())
            continue;
        if ((entity.getAbsOrigin() - eye).squareLength() > 200.0f * 200.0f)
            continue;
        const auto model = entity.getRenderable().getModel();
        const auto studio = model ? engineInterfaces.getModelInfo().getStudioModel(model) : nullptr;
        const auto set = studio ? studio->getHitboxSet(entity.hitboxSet()) : nullptr;
        if (!set)
            continue;
        const float hp = static_cast<float>(entity.health());
        const float armor = entity.armor() > 0 ? 0.85f : 1.0f;
        const auto& list = records[i];
        for (int age = 0; age < list.count; ++age) {
            const auto& record = list.at(age);
            if (age > 0 && horizontalJump(record.origin, list.at(age - 1).origin))
                break;
            if (!recordValid(record.simulationTime, serverTime, latencyIn, lerpTime, maxUnlag, interval))
                continue;
            // ближайшая к глазам точка тела: по оси капсулы, расстояние — до её поверхности
            buildCapsules(*set, record.bones, caps);
            float distance = 1e9f;
            csgo::Vector point{};
            for (int k = 0; k < caps.count; ++k) {
                const auto& capsule = caps.items[k];
                const auto axis = capsule.b - capsule.a;
                const float lengthSquared = axis.squareLength();
                const float t = lengthSquared > 0.0f ? std::clamp((eye - capsule.a).dotProduct(axis) / lengthSquared, 0.0f, 1.0f) : 0.0f;
                const auto nearest = capsule.a + axis * t;
                const float surface = (nearest - eye).length() - capsule.radius;
                if (surface < distance) {
                    distance = surface;
                    point = nearest;
                }
            }
            if (distance > primaryRange)
                continue;

            const float yawRadians = Helpers::deg2rad(record.eyeAngles.y);
            csgo::Vector toVictim = record.origin - eye;
            toVictim.z = 0.0f;
            const float flat = toVictim.length();
            const bool backstab = flat > 0.1f && (toVictim.x * std::cos(yawRadians) + toVictim.y * std::sin(yawRadians)) / flat > 0.475f;
            const float primaryDamage = (backstab ? 90.0f : 25.0f) * armor;
            const float secondaryDamage = (backstab ? 180.0f : 65.0f) * armor;

            bool secondary = false, lethal = false, use = false;
            if (secondaryReady && distance <= secondaryRange && secondaryDamage >= hp) {
                secondary = lethal = use = true;
            } else if (primaryReady && primaryDamage >= hp) {
                lethal = use = true;
            } else if (rage.knifeBotMode == 1) {
                // не наповал: ПКМ сильнее, но медленнее и короче — если достаём, бьём им
                if (secondaryReady && distance <= secondaryRange)
                    secondary = use = true;
                else if (primaryReady)
                    use = true;
            }
            if (!use)
                continue;
            if ((lethal && !best.lethal) || (lethal == best.lethal && distance < best.distance)) {
                best = { i, &record, point, secondary, lethal, distance };
            }
        }
    }
    if (!best.record)
        return false;

    auto angle = (best.point - eye).toAngle();
    angle.normalize();
    cmd->viewangles = angle;
    cmd->buttons &= ~(csgo::UserCmd::IN_ATTACK | csgo::UserCmd::IN_ATTACK2);
    cmd->buttons |= best.secondary ? csgo::UserCmd::IN_ATTACK2 : csgo::UserCmd::IN_ATTACK;
    cmd->tickCount = timeToTicks(best.record->simulationTime, interval) + timeToTicks(lerpTime, interval);
    if (!rage.silent)
        engineInterfaces.getEngine().setViewAngles(cmd->viewangles);
    target = best.index;
    statusText = best.secondary ? "нож: ПКМ" : "нож: ЛКМ";
    return true;
}

// ---------- дормант-аимбот ----------
// Враг вне PVS: сервер его не шлёт, но звуки (шаги, выстрел, перезарядка) приходят с координатами, а
// замеченных союзниками — радар. Держим последнюю позицию и время; стреляем в живот через стену.

struct DormantTrack {
    csgo::Vector origin{};
    float at = -1.0f;               // realtime последней достоверной позиции
};
std::array<DormantTrack, 65> dormantTracks{};

// каждый тик: живые враги — позиция как есть; дормант — по звуку / радару, опущенная на пол
void trackDormant(const EngineInterfaces& engineInterfaces, const ClientInterfaces& clientInterfaces, const Memory& memory, int maxClients, float realtime) noexcept
{
    for (int i = 1; i <= maxClients; ++i) {
        auto& track = dormantTracks[i];
        const auto entity = csgo::Entity::from(retSpoofGadgets->client, clientInterfaces.getEntityList().getEntity(i));
        if (!entity.getPOD() || entity.getPOD() == localPlayer.get().getPOD() || !entity.isOtherEnemy(memory, localPlayer.get()) || !entity.isAlive()) {
            track.at = -1.0f;
            continue;
        }
        if (realtime < track.at)
            track.at = -1.0f;
        if (!entity.getNetworkable().isDormant()) {
            track.origin = entity.getAbsOrigin();
            track.at = realtime;
            continue;
        }
        bool heard = false;
        csgo::Vector heardAt{};
        for (int c = 0; c < memory.activeChannels->count; ++c) {
            const auto& channel = memory.channels[memory.activeChannels->list[c]];
            if (channel.soundSource != i || (channel.origin.x == 0.0f && channel.origin.y == 0.0f))
                continue;
            heardAt = channel.origin;
            heard = true;
            break;
        }
        if (!heard) {
            auto radarAt = track.origin;
            if (track.at > 0.0f && hvh::radar::origin(i, track.at, radarAt)) {
                heardAt = radarAt;
                heard = true;
            }
        }
        if (!heard)
            continue;
        // звук оружия звучит из рук, шаги — из ног: опускаем точку на пол, дальше от него считаем высоту
        csgo::Trace trace;
        engineInterfaces.engineTrace().traceRay({ heardAt + csgo::Vector{ 0.0f, 0.0f, 4.0f }, heardAt - csgo::Vector{ 0.0f, 0.0f, 90.0f } },
            0x1400B, { entity.getPOD() }, trace);
        track.origin = trace.fraction < 1.0f ? trace.endpos : heardAt;
        track.at = realtime;
    }
}

bool dormantShot(csgo::UserCmd* cmd, const EngineInterfaces& engineInterfaces, const ClientInterfaces& clientInterfaces, const OtherInterfaces& interfaces,
                 const Memory& memory, const csgo::WeaponInfo* weaponData, const RageWeapon& settings, int maxClients,
                 const csgo::Vector& eye, float cone, bool needScope, bool scopeAllowed, AutoStop stopMode, float stopMaxSpeed,
                 const MovementVars& moveVars, float interval, float realtime) noexcept
{
    const auto& rage = config.rage;
    // свежесть позиции -> «точность»: 100% — только что, 0% — 1.5 с назад
    const float maxAge = 1.5f * (1.0f - static_cast<float>(std::clamp(rage.dormantAccuracy, 25, 95)) / 100.0f);
    const int minDamageSetting = minDamageOverride ? settings.minDamageOverride : settings.minDamage;

    int bestIndex = 0;
    float bestDamage = 0.0f;
    csgo::Vector bestPoint{};
    for (int i = 1; i <= maxClients; ++i) {
        const auto& track = dormantTracks[i];
        if (track.at < 0.0f || realtime - track.at > maxAge)
            continue;
        const auto entity = csgo::Entity::from(retSpoofGadgets->client, clientInterfaces.getEntityList().getEntity(i));
        if (!entity.getPOD() || !entity.getNetworkable().isDormant() || !entity.isAlive() || !entity.isOtherEnemy(memory, localPlayer.get()))
            continue;
        // живот: стоя ~40 ед. от пола, сидя ~28 (флаги — с последнего обновления, другого нет)
        const bool ducking = entity.flags() & (1 << 1);
        const auto point = track.origin + csgo::Vector{ 0.0f, 0.0f, ducking ? 28.0f : 40.0f };
        const auto toPoint = (point - eye).toAngle();
        const float fov = std::hypot(std::remainder(toPoint.x - cmd->viewangles.x, 360.0f), std::remainder(toPoint.y - cmd->viewangles.y, 360.0f));
        if (fov > rage.fov)
            continue;
        const float raw = autowall::getDamageToPoint(engineInterfaces, interfaces, point, weaponData, rage.autowall);
        if (raw <= 0.0f)
            continue;
        const float damage = autowall::scaleDamage(entity, HitGroup::Stomach, weaponData, raw);
        const int health = std::max(entity.health(), 1);
        const int need = minDamageSetting > 100 ? health + (minDamageSetting - 100) : std::min(minDamageSetting, health);
        if (damage < static_cast<float>(need) || damage <= bestDamage)
            continue;
        bestIndex = i;
        bestDamage = damage;
        bestPoint = point;
    }
    if (!bestIndex)
        return false;

    target = bestIndex;
    const bool onGround = (localPlayer.get().flags() & FL_ONGROUND) && !(cmd->buttons & csgo::UserCmd::IN_JUMP);
    if (needScope && onGround) {
        if (scopeAllowed)
            cmd->buttons |= csgo::UserCmd::IN_ATTACK2;
        statusText = "дормант: скоуп";
        return false;
    }
    if (stopMode != AutoStop::Off && onGround)
        autoStop(cmd, stopMode, stopMaxSpeed, interval, moveVars);
    // пятно разброса на этой дистанции должно лечь в живот (~7 ед.): иначе ждём, пока автостоп погасит скорость
    if (cone * (bestPoint - eye).length() > 7.0f) {
        statusText = "дормант: ждёт точности";
        return false;
    }
    if (tickbase::hideShotsActive() && tickbase::isInstalled() && tickbase::sendPacket() && !tickbase::isShifting()) {
        tickbase::chokeNextPacket();
        statusText = "дормант: хайдшот в след. тике";
        return false;
    }
    auto aimAngle = (bestPoint - eye).toAngle() - localPlayer.get().aimPunchAngle() * 2.0f;
    aimAngle.normalize();
    cmd->viewangles = aimAngle;
    cmd->buttons |= csgo::UserCmd::IN_ATTACK;
    statusText = "дормант: выстрел";
    if (!rage.silent)
        engineInterfaces.getEngine().setViewAngles(cmd->viewangles);
    if (!tickbase::isShifting())
        tickbase::onShot();
    return true;
}

bool runImpl(csgo::UserCmd* cmd, const EngineInterfaces& engineInterfaces, const ClientInterfaces& clientInterfaces, const OtherInterfaces& interfaces, const Memory& memory) noexcept
{
    const auto& rage = config.rage;
    memoryPtr = &memory;
    engine.emplace(engineInterfaces.getEngine());
    minDamageOverride = rage.minDamageOverrideKey.isSet() && rage.minDamageOverrideKey.isDown();
    forceBody = rage.forceBodyKey.isSet() && rage.forceBodyKey.isDown();
    forceShot = rage.forceShot && (!rage.forceShotKey.isSet() || rage.forceShotKey.isToggled());
    target = 0;
    statusText = "";
    const float realtime = memory.globalVars->realtime;

    // выстрелы без player_hurt за 0.7 с — промахи
    for (int i = 1; i < 65; ++i) {
        if (shots[i].time > 0.0f && realtime - shots[i].time > 0.7f)
            evaluateMiss(i);
    }

    if (!rage.enabled || !localPlayer || !localPlayer.get().isAlive()) {
        for (auto& list : records)
            list.clear();
        hvh::resolver::clear();
        return false;
    }

    const float interval = memory.globalVars->intervalPerTick;
    const int maxClients = std::min(engineInterfaces.getEngine().getMaxClients(), 64);
    static const auto maxUnlagVar = interfaces.getCvar().findVar("sv_maxunlag");
    const float maxUnlag = maxUnlagVar ? csgo::ConVar::from(retSpoofGadgets->client, maxUnlagVar).getFloat() : 0.2f;

    // ---- записи и резольвер — каждый тик, даже когда стрелять нечем ----
    std::array<bool, 65> isBot{};
    for (int i = 1; i <= maxClients; ++i) {
        const auto entity = csgo::Entity::from(retSpoofGadgets->client, clientInterfaces.getEntityList().getEntity(i));
        if (entity.getPOD() == nullptr || entity.getPOD() == localPlayer.get().getPOD() || entity.getNetworkable().isDormant()
            || !entity.isAlive() || !entity.isOtherEnemy(memory, localPlayer.get())) {
            records[i].clear();
            continue;
        }
        csgo::PlayerInfo info{};
        const bool haveInfo = engineInterfaces.getEngine().getPlayerInfo(i, info);
        isBot[i] = haveInfo && info.fakeplayer;
        track(brains[i], entity, i, haveInfo ? info.userId : -1, interval);
        updateRecords(records[i], entity, memory, rage.resolver && !isBot[i], i, maxUnlag);

        const auto& list = records[i];
        std::scoped_lock lock{ viewMutex };
        auto& view = views[i];
        view.updatedAt = realtime;
        view.breakingLagComp = list.count && list.at(0).breakingLagComp;
        view.charged = list.count && memory.globalVars->serverTime() - list.at(0).simulationTime > 0.15f + interval * 2.0f;
    }

    if (rage.dormantAim)
        trackDormant(engineInterfaces, clientInterfaces, memory, maxClients, realtime);

    if (localPlayer.get().isDefusing())
        return false;
    const auto weapon = csgo::Entity::from(retSpoofGadgets->client, localPlayer.get().getActiveWeapon());
    if (!weapon.getPOD())
        return false;
    const auto weaponType = weapon.getWeaponType();
    if (weaponType == WeaponType::Knife)
        return knifeBot(cmd, weapon, engineInterfaces, clientInterfaces, memory, maxUnlag);
    if (weapon.clip() <= 0 || weaponType == WeaponType::Grenade || weaponType == WeaponType::C4)
        return false;
    // зевс — свой режим: только наповал, тело, свой хитшанс; выключен — не стреляем им вовсе
    const bool taser = weapon.itemDefinitionIndex() == WeaponId::Taser;
    if (taser && !rage.zeusBot)
        return false;
    const bool revolver = weapon.itemDefinitionIndex() == WeaponId::Revolver;
    static const bool havePostpone = Netvars::get(fnv::hash("CWeaponCSBase->m_flPostponeFireReadyTime")) != 0;
    if (revolver && !havePostpone)
        return false;
    const auto weaponData = weapon.getWeaponData();
    if (!weaponData)
        return false;

    // без автострельбы и без ЛКМ не стреляем, но точки для визуала ищем
    const bool visualOnly = !rage.autoFire && !(cmd->buttons & csgo::UserCmd::IN_ATTACK);
    const bool viewWanted = config.view.aimDot;
    if (visualOnly && !viewWanted)
        return false;

    static RageWeapon zeusSettings;
    if (taser) {
        zeusSettings = {};
        zeusSettings.hitboxes = zeusSettings.multipoint = HB_Chest | HB_Stomach | HB_Pelvis;
        zeusSettings.bodyScale = 0.5f;
        zeusSettings.minDamage = 100;               // не больше хп — значит, только наповал
        zeusSettings.hitchance = std::clamp(rage.zeusHitchance, 1, 100);
        zeusSettings.autoStop = static_cast<int>(AutoStop::Accuracy);
        zeusSettings.autoScope = false;
        zeusSettings.bodyIfLethal = true;
    }
    const auto& settings = taser ? zeusSettings : weaponSettings(groupOf(weapon));
    extras::setMinDamage(minDamageOverride ? settings.minDamageOverride : settings.minDamage, minDamageOverride);
    const float serverTime = memory.globalVars->serverTime();
    // во время сдвига DT команды идут пачкой без предсказания между ними: nextPrimaryAttack ещё старый
    static float firedAt = -1.0f;
    static float firedCycle = 0.0f;
    const bool cycling = tickbase::isShifting() && firedAt >= 0.0f && serverTime >= firedAt && serverTime < firedAt + firedCycle;
    const bool weaponReady = localPlayer.get().nextAttack() <= serverTime && weapon.nextPrimaryAttack() <= serverTime && !cycling;
    // R8 стреляет, только когда ЛКМ продержали 0.234 с (взвод), и сам — как только взвёлся. Взводим сами;
    // в тик готовности либо выстрел, либо ЛКМ отпускается (взвод начнётся заново)
    bool canShoot = weaponReady;
    float revolverReadyAt = 0.0f;
    if (revolver) {
        const float fireReady = weapon.postponeFireReadyTime();
        const bool cocking = fireReady > 0.0f && fireReady < serverTime + 1.0f;
        canShoot = weaponReady && cocking && fireReady <= serverTime;
        revolverReadyAt = cocking ? fireReady : serverTime + 0.234375f;
        if (rage.autoFire && weaponReady) {
            if (canShoot)
                revolverRelease = true;
            else
                cmd->buttons |= csgo::UserCmd::IN_ATTACK;
        }
    }
    const bool needScope = settings.autoScope && weapon.isSniperRifle() && !localPlayer.get().isScoped();
    // после выстрела игра сама возвращает зум в тик готовности — наш ПКМ в тот же тик перещёлкивал зум
    // (второй уровень/выход из скопа). Жмём, только если тик спустя игра зум не вернула.
    const bool scopeAllowed = weapon.nextPrimaryAttack() + interval * 1.5f <= serverTime && weapon.nextSecondaryAttack() <= serverTime;
    const auto stopMode = static_cast<AutoStop>(settings.autoStop);
    const auto moveVars = movementVars(interfaces.getCvar());
    const float stopMaxSpeed = localPlayer.get().isScoped() || needScope ? weaponData->maxSpeedAlt : weaponData->maxSpeed;
    const int stopTicks = stopMode == AutoStop::Off ? 0
        : ticksToStop(localPlayer.get().velocity().length2D(), autoStopTarget(stopMode, stopMaxSpeed), stopMaxSpeed, moveVars, interval);
    const float readyIn = std::max({ localPlayer.get().nextAttack(), weapon.nextPrimaryAttack(), revolverReadyAt }) - serverTime;
    const bool readySoon = readyIn <= interval * static_cast<float>(std::max(stopTicks, 1));
    fireReadyNow = canShoot || readyIn <= interval * static_cast<float>(std::max(stopTicks, 4));
    fireReadyTick = memory.globalVars->tickCount;

    // Между выстрелами полный поиск не нужен (автоволл — самое дорогое): только ради раннего стопа/скопа
    // или отрисовки точек — дешёвый (одна запись), для отрисовки раз в 4 тика
    const bool idle = !canShoot && !readySoon;
    const bool viewOnly = visualOnly || (idle && !settings.autoStopEarly && !needScope);
    if (idle && !settings.autoStopEarly && !needScope && !viewWanted) {
        statusText = "перезарядка";
        return false;
    }
    if (viewOnly) {
        static int viewTick = 0;
        if (!visualOnly)
            statusText = "перезарядка";
        if (++viewTick % 4 != 0)
            return false;
    }
    const bool cheap = idle || visualOnly;
    const bool doubleTapReady = tickbase::fastFireEnabled() && tickbase::isCharged() && !tickbase::isShifting();
    // вторая пуля DT сразу — только у быстрого оружия (авто, пистолеты); у скаута/AWP перезарядка больше секунды
    const bool doubleTapKill = doubleTapReady && weaponData->cycletime <= 0.4f;
    // команда этого тика чокается — уйдёт со следующим пакетом (выстрел его форсирует), на тик позже;
    // с DT сдвиг уходит тем же пакетом, что и выстрел
    float sendDelay = tickbase::isInstalled() && !tickbase::sendPacket() && !doubleTapReady ? interval : 0.0f;
    // Фейкдак: выстрел едет в чоке до конца цикла — сервер получит его через (лимит − чок) тиков
    if (movement::isFakeDucking())
        sendDelay = interval * static_cast<float>(std::max(tickbase::fakeDuckChoke() - tickbase::chokedCommands(), 1));
    statusText = "нет целей";

    float latencyOut = 0.0f, latencyIn = 0.0f;
    if (const auto channel = engineInterfaces.getEngine().getNetworkChannel()) {
        const auto net = csgo::NetworkChannel::from(retSpoofGadgets->client, channel);
        latencyOut = net.getLatency(0);
        latencyIn = net.getLatency(1);
    }
    static const auto gravityVar = interfaces.getCvar().findVar("sv_gravity");
    const float gravity = gravityVar ? csgo::ConVar::from(retSpoofGadgets->client, gravityVar).getFloat() : 800.0f;

    const auto eye = localPlayer.get().getEyePosition();
    const csgo::Vector worldUp{ 0.0f, 0.0f, 1.0f };
    // разброс в долях дистанции: радиус пятна пуль на d юнитах ≈ d·cone
    const float cone = std::max(weapon.getInaccuracy(), 0.0f)
        + std::clamp(weapon.weaponMode() == 1 ? weaponData->spreadAlt : weaponData->spread, 0.0f, 0.2f);
    // мультипоинт по всему телу — до ~70 точек на цель, каждая автоволл: бюджет на тик против фризов
    int pointBudget = 240;

    static std::array<Target, 3> top;
    int topCount = 0;
    static LagRecord predicted;
    static LagRecord live;
    static csgo::matrix3x4 bones[RECORD_BONES];
    static Capsules aimCaps, plusCaps, minusCaps, earlierCaps, laterCaps;

    for (int i = 1; i <= maxClients; ++i) {
        const auto entity = csgo::Entity::from(retSpoofGadgets->client, clientInterfaces.getEntityList().getEntity(i));
        if (entity.getPOD() == nullptr || entity.getPOD() == localPlayer.get().getPOD() || entity.getNetworkable().isDormant()
            || !entity.isAlive() || !entity.isOtherEnemy(memory, localPlayer.get()) || entity.gunGameImmunity())
            continue;
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

        auto& brain = brains[i];
        const auto& list = records[i];

        // ---- записи-кандидаты по приоритету ----
        struct Candidate {
            const LagRecord* record;
            RecordKind kind;
            int age;
            int updatesAhead;
        };
        std::array<Candidate, 4> candidates{};
        int candidateCount = 0;
        csgo::Vector aheadStep{};
        float aheadPhase = -1.0f;
        bool aheadRisk = false;

        if (list.count && list.at(0).breakingLagComp) {
            // ломает лагкомп: сервер стреляет по текущей позиции — только прогноз
            const int updates = predictAhead(list, predicted, realtime, latencyOut + latencyIn + sendDelay, interval, gravity, brain.aheadChecks >= 2 ? brain.aheadBias : aheadBias, aheadStep, &aheadPhase, &aheadRisk);
            candidates[candidateCount++] = { &predicted, RecordKind::Extrapolated, 0, updates };
        } else {
            int onshot = -1, standing = -1, newest = -1, oldest = -1;
            for (int age = 0; age < list.count; ++age) {
                const auto& record = list.at(age);
                // сервер идёт от свежей записи к старым и на первом разрыве > 64 ед. бросает откат:
                // всё, что старше разрыва, недостижимо — выстрел уйдёт по текущей позиции
                if (age > 0 && horizontalJump(record.origin, list.at(age - 1).origin))
                    break;
                if (record.animStale || !recordValid(record.simulationTime, serverTime, latencyIn, lerpTime, maxUnlag, interval, sendDelay + 2.0f * interval))
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
                if (age < 0)
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
            // старая — только если заметно в другом месте: выглядывающего ловим до того, как спрятался
            if (oldest >= 0 && newest >= 0 && (list.at(oldest).origin - list.at(newest).origin).squareLength() > 16.0f)
                add(oldest, RecordKind::Oldest);

            if (candidateCount == 0 && list.count) {
                const int updates = predictAhead(list, predicted, realtime, latencyOut + latencyIn + sendDelay, interval, gravity, brain.aheadChecks >= 2 ? brain.aheadBias : aheadBias, aheadStep, &aheadPhase, &aheadRisk);
                candidates[candidateCount++] = { &predicted, RecordKind::Ahead, 0, updates };
            }
            if (candidateCount == 0) {
                // записей ещё нет (только что появился) — интерполированная модель, tickcount не трогаем
                static csgo::matrix3x4 setup[MAXSTUDIOBONES];
                if (!entity.setupBones(setup, MAXSTUDIOBONES, BONE_USED_BY_HITBOX, memory.globalVars->currenttime))
                    continue;
                std::copy(setup, setup + RECORD_BONES, live.bones);
                live.simulationTime = entity.simulationTime();
                live.origin = entity.getAbsOrigin();
                live.eyeAngles = serverEyeAngles(i, entity);
                live.absYaw = entity.getAbsAngle().y;
                live.velocity = entity.velocity();
                live.flags = entity.flags();
                live.choked = 1;
                live.maxDesync = std::clamp(entity.getMaxDesyncAngle(), 0.0f, 60.0f);
                live.jitterPhase = brain.jitterPhase;
                live.variants = false;
                live.resolved = false;
                if (rage.resolver && !isBot[i]) {
                    rotateVariant(live, 1.0f, live.plus);
                    rotateVariant(live, -1.0f, live.minus);
                    live.variants = true;
                }
                candidates[candidateCount++] = { &live, RecordKind::Live, 0, 0 };
            }
        }
        if (cheap)
            candidateCount = 1;

        const int health = entity.health();
        const float hp = static_cast<float>(health);
        const int minDamageSetting = minDamageOverride ? settings.minDamageOverride : settings.minDamage;
        // больше 100 — «HP+N»: убить с запасом; иначе хп меньше мин. урона — достаточно добить
        const int minDamage = minDamageSetting > 100 ? health + (minDamageSetting - 100) : std::min(minDamageSetting, health);
        const bool safeRequired = rage.safePoint == 2 || (rage.safePoint == 1 && brain.misses > 0);
        // своя статистика: по голове мажем, а по телу попадаем — голове меньше доверия
        const float headRate = (brain.headHits + 0.5f) / (brain.headShots + 1.0f);
        const float bodyRate = (brain.bodyHits + 0.5f) / (brain.bodyShots + 1.0f);
        // статистика головы — только если в голову правда стреляли (раньше 4 попадания в тело без единого выстрела
        // в голову уже давали ей штраф)
        const bool headUnlucky = (brain.bodyShots >= 4 && brain.headShots >= 3 && bodyRate > headRate + 0.1f) || brain.headResolverMisses == 1;
        // два промаха резольвера в голову подряд — до попадания в голову бьём тело, голову только если
        // тело недоступно (наповал по голове 1.5×0.35 ≈ 0.5 — меньше любого убийства телом)
        const bool headBanned = brain.headResolverMisses >= 2;

        std::array<std::pair<csgo::Vector, std::uint8_t>, 64> viewPoints, shownPoints;
        int viewPointCount = 0, shownPointCount = 0;
        const char* resolverText = "";
        const char* shownResolver = "";
        // лучшая запись игрока: каждая запись-кандидат оценивается целиком, берётся самая полезная
        // (выглядывал — в старой записи голова открыта, в свежей уже за стеной)
        static Target playerBest;
        playerBest.score = -1.0f;

        for (int c = 0; c < candidateCount; ++c) {
            const auto& record = *candidates[c].record;
            const bool predictedRecord = candidates[c].kind == RecordKind::Ahead || candidates[c].kind == RecordKind::Extrapolated;
            const bool airborne = !(record.flags & FL_ONGROUND);

            // ---- резольвер: по какому скелету стреляем и насколько уверены ----
            std::copy(record.bones, record.bones + RECORD_BONES, bones);
            float side = 0.0f;
            float guess = 0.0f;
            bool sideCertain = true;
            // доверие к стороне ≈ доля попаданий в тело при таком источнике (логи 29+31.10);
            // голова к ошибке стороны чувствительнее — для неё квадрат
            float sideTrust = 1.0f;
            SideSource source = SideSource::None;
            const bool resolve = rage.resolver && !isBot[i] && record.variants;
            if (!rage.resolver) {
                source = SideSource::None;
            } else if (isBot[i]) {
                source = SideSource::Bot;
            } else if (!desyncing(brain)) {
                source = SideSource::NoDesync;
            } else if (!record.variants) {
                source = SideSource::None;
                sideCertain = false;
                sideTrust = 0.6f;
            } else if (airborne) {
                // в прыжке десинк мал, модели почти совпадают — поворот по догадке только уводит точку
                source = SideSource::Air;
                sideTrust = 0.8f;
            } else if (record.resolved && layerSideUsable(record) && !(predictedRecord && (jittering(brain) || desyncFlipping(brain))) && !(brain.bruteSide != 0.0f && realtime < brain.bruteUntil)) {
                // Слои — лучший источник (логи 29+31.10: 152 попадания на 24 промаха резольвера, 86%), в том числе
                // при низкой уверенности: скелет собран ровно под найденный угол ног, а не под крайний.
                // Уступают только перебору сразу после промаха по вине резольвера.
                source = SideSource::Layers;
                side = std::remainder(record.resolvedYaw - record.eyeAngles.y, 360.0f) >= 0.0f ? 1.0f : -1.0f;
                sideCertain = record.resolvedConfidence >= 0.5f;
                sideTrust = sideCertain ? 0.85f : 0.75f;
            } else {
                // сторона: перебор после промаха > связь с фазой (джиттер) > свидетельства > фристенд
                const float evidence = sideEvidence(brain, realtime);
                if (brain.bruteSide != 0.0f && realtime < brain.bruteUntil) {
                    side = brain.bruteSide;
                    source = SideSource::Brute;
                    sideCertain = false;
                    sideTrust = brain.bruteProven ? 0.75f : 0.5f;
                } else if (jittering(brain) && std::abs(brain.jitterLink) >= 1.0f) {
                    // за время до сервера фаза могла перещёлкнуться: нечётное число обновлений — обратная
                    const float phase = candidates[c].updatesAhead % 2 ? -record.jitterPhase : record.jitterPhase;
                    side = brain.jitterLink > 0.0f ? phase : -phase;
                    source = SideSource::Jitter;
                    sideCertain = std::abs(brain.jitterLink) >= 3.0f;
                    sideTrust = sideCertain ? 0.75f : 0.6f;
                } else if (std::abs(evidence) >= 1.0f && !jittering(brain)) {
                    side = evidence > 0.0f ? 1.0f : -1.0f;
                    source = SideSource::Evidence;
                    sideCertain = std::abs(evidence) >= 3.0f;
                    sideTrust = sideCertain ? 0.8f : 0.65f;
                } else {
                    guess = freestandSide(engineInterfaces.engineTrace(), eye, record);
                    // у этого игрока догадка доказанно ошибается — его антиаим прячет голову наоборот
                    side = brain.guessWrong >= 3 && brain.guessWrong > 2 * brain.guessRight ? -guess : guess;
                    source = SideSource::Guess;
                    sideCertain = false;
                    sideTrust = 0.6f;
                }
                std::copy(side > 0.0f ? record.plus : record.minus, (side > 0.0f ? record.plus : record.minus) + RECORD_BONES, bones);
            }
            resolverText = sideLabel(source, side);

            // ---- точки ----
            // Каждая точка: урон (автоволл до точки записи, реальная хитгруппа на луче) и надёжность —
            // оценка вероятности попасть при том, чего мы точно не знаем (калибровка по логам 29+31.10):
            //  * сторона: точка, общая для всех трёх скелетов, от неё не зависит; иначе × доверие к стороне
            //    (тело), × его квадрат (голова: догадкой 31% против 52% у тела, перебором 15% против 42%);
            //  * прогноз «вперёд»: тело ×0.7, голова ×0.4 (логи: тело 47% против 81% на свежей, голова 27% против 80%);
            //    точка, не попадающая и в модель N−1, и в N, — ещё ×0.6;
            //  * край капсулы ×0.9, сквозь стену ×0.85.
            // Выбор — по пользе: убить сейчас (1.5) > убить двумя пулями DT (1.1) > урон (урон/хп), всё × надёжность.
            const bool checkSafe = !cheap && resolve;
            // Строгий отбор (логи 04–09.11, 412 выстрелов по живым): прогноз в воздухе 21%, прогноз по
            // джиттеру/флипам 32–41%, небезопасные точки 20–43%, голова в прыжке/на джиттере ~30%.
            // Отброшенное попадало в 36%, оставшееся — в 65%. Лучше подождать тик-другой настоящую запись.
            // Перебор и «не десинкает» не трогаем — там сторона известна (7/7, 3/6).
            const bool strict = rage.strictShots && checkSafe && source != SideSource::NoDesync && source != SideSource::Brute;
            if (strict && predictedRecord && (airborne || jittering(brain) || desyncFlipping(brain))) {
                statusText = "строгий: ждём настоящую запись";
                continue;
            }
            // Выстрел придёт к серверу на границе пакета врага — сервер возьмёт N−1 или N наугад. Если за
            // обновление враг сдвигается заметно (> 4 ед.), ждём середину периода: это не дольше полупериода
            // (при чоке 14 — до ~100 мс), а попадание перестаёт быть монеткой.
            if (predictedRecord && aheadRisk && !cheap && aheadStep.length() * 2.0f > 4.0f) {
                statusText = "прогноз: ждём середину обновления";
                continue;
            }
            static Capsules centerCaps;
            if (checkSafe) {
                buildCapsules(*set, record.bones, centerCaps);
                buildCapsules(*set, record.plus, plusCaps);
                buildCapsules(*set, record.minus, minusCaps);
            }
            // Точка головы безопасна, только если луч в каждой модели первым входит именно в голову.
            // Раньше хватало любой капсулы: луч в голову одной модели и в плечо другой считался безопасным,
            // и по логам 31.10 так вышло 16 выстрелов «в голову», попавших в тело.
            const auto hitsAs = [&](const Capsules& caps, const csgo::Vector& end, bool head) {
                return head ? firstHitGroup(caps, eye, end) == HITGROUP_HEAD : rayHits(eye, end, caps);
            };
            const auto isSafe = [&](const csgo::Vector& point, bool head) {
                if (!checkSafe)
                    return true;
                const auto end = eye + normalized(point - eye) * weaponData->range;
                return hitsAs(centerCaps, end, head) && hitsAs(plusCaps, end, head) && hitsAs(minusCaps, end, head);
            };
            // Слои: сторону находят надёжнее, чем величину (4 из 5 доказанных промахов в голову — сторона
            // верна, ноги дальше). Точка, общая для найденного угла и полного десинка той же стороны,
            // от величины не зависит — ей доверяем как стороне, а не как стороне и углу сразу.
            const bool layerSide = checkSafe && source == SideSource::Layers && side != 0.0f;
            const bool flipping = desyncFlipping(brain);
            const bool headNeedsSafe = (source == SideSource::Layers && (record.resolvedConfidence < 0.6f || flipping))
                || source == SideSource::Air || source == SideSource::Jitter;
            const csgo::matrix3x4* sideExtreme = side > 0.0f ? record.plus : record.minus;
            static Capsules sideCaps;
            if (layerSide)
                buildCapsules(*set, sideExtreme, sideCaps);
            const auto isSideSafe = [&](const csgo::Vector& point, bool head) {
                if (!layerSide)
                    return false;
                const auto end = eye + normalized(point - eye) * weaponData->range;
                return hitsAs(aimCaps, end, head) && hitsAs(sideCaps, end, head);
            };
            buildCapsules(*set, bones, aimCaps);

            const bool checkSteps = predictedRecord && !cheap && aheadStep.squareLength() > 1.0f;
            if (checkSteps) {
                earlierCaps = aimCaps;
                laterCaps = aimCaps;
                for (int k = 0; k < aimCaps.count; ++k) {
                    earlierCaps.items[k].a -= aheadStep;
                    earlierCaps.items[k].b -= aheadStep;
                    laterCaps.items[k].a += aheadStep;
                    laterCaps.items[k].b += aheadStep;
                }
            }
            const auto stepSafe = [&](const csgo::Vector& point) {
                const auto end = eye + normalized(point - eye) * weaponData->range;
                return rayHits(eye, end, earlierCaps) && rayHits(eye, end, laterCaps);
            };
            // Джиттер на прогнозе: позицию мы угадываем, а в какой фазе рывка враг будет на сервере — нет.
            // Поза двух свежих записей (обе фазы), перенесённая на прогнозную позицию: точка должна попадать в обе.
            const bool checkPhases = predictedRecord && !cheap && list.count >= 2 && (jittering(brain) || desyncFlipping(brain));
            static Capsules phaseCaps[2];
            csgo::Vector phaseShift[2]{};
            if (checkPhases) {
                for (int k = 0; k < 2; ++k) {
                    phaseShift[k] = record.origin - list.at(k).origin;
                    buildCapsules(*set, list.at(k).bones, phaseCaps[k]);
                    shiftCapsules(phaseCaps[k], phaseShift[k]);
                }
            }
            const auto phaseSafe = [&](const csgo::Vector& point, bool head) {
                const auto end = eye + normalized(point - eye) * weaponData->range;
                return hitsAs(phaseCaps[0], end, head) && hitsAs(phaseCaps[1], end, head);
            };

            // край капсулы прощает только при известной стороне: иначе «край» — промах по чужой стороне
            float sideScale = 1.0f;
            if (source == SideSource::Layers)
                sideScale = std::clamp(record.resolvedConfidence * 1.4f, 0.35f, 1.0f);
            else if (resolve && !sideCertain)
                sideScale = side == 0.0f ? 0.35f : 0.45f;

            struct Scored {
                AimPoint point;
                float score = -1.0f;
                bool safe = false;
                float reliability = 1.0f;
            };
            Scored head, body, any;
            const auto offer = [](Scored& slot, const AimPoint& point, float score, bool safe, float reliability) {
                if (score > slot.score)
                    slot = { point, score, safe, reliability };
            };
            viewPointCount = 0;

            for (const auto& [flag, hitboxes] : hitboxGroups) {
                // тело наповал по надёжной точке — лучше уже не будет (голова может только сравняться)
                if (!viewOnly && body.score >= 1.5f * 0.99f)
                    break;
                if (!(settings.hitboxes & flag))
                    continue;
                const bool isHead = flag == HB_Head;
                if (isHead && forceBody)
                    continue;
                const bool isLimb = flag == HB_Arms || flag == HB_Legs || flag == HB_Feet;
                if (isLimb && cheap)
                    continue;
                const bool multipoint = (settings.multipoint & flag) && (!cheap || viewOnly);
                // каждый промах прижимает точки к центру; прогноз — тоже
                const float shrink = (brain.misses >= 2 ? 0.4f : brain.misses == 1 ? 0.7f : 1.0f) * (predictedRecord ? 0.75f : 1.0f);
                const float maxScale = (isHead ? settings.headScale : settings.bodyScale) * sideScale;
                float trust = 1.0f;
                // логи 04–08.11 по «вперёд»: голова 30%, тело 43% (на настоящих записях 65–90%)
                if (predictedRecord)
                    trust *= isHead ? 0.5f : 0.7f;
                // прогноз в воздухе: стрейф меняет курс между обновлениями — 12 из 55 (22%)
                if (predictedRecord && airborne)
                    trust *= 0.5f;
                // прогноз по джиттерящему/перекидывающему десинк: 20 из 64 (31%)
                if (predictedRecord && (jittering(brain) || desyncFlipping(brain)))
                    trust *= 0.75f;
                // Бан/невезение головы — про ошибку стороны десинка: штрафуют только небезопасные точки
                // (ниже, по точке). Раньше штраф шёл на всю голову, и после двух промахов в начале раунда
                // голову не били до конца раунда даже там, где она общая для всех сторон. Логи 04–09.11:
                // безопасная голова по настоящим записям — 35/55 (64%), безопасное тело — 59%.
                const float headSidePenalty = !isHead ? 1.0f : headBanned ? 0.35f : headUnlucky ? 0.6f : 1.0f;
                if (!isHead && settings.preferBody)
                    trust *= 1.3f;
                // голове раньше шёл квадрат доверия — при DT тело всегда выигрывало у головы
                const float sideFactor = sideTrust;

                for (const auto hitboxId : hitboxes) {
                    if (hitboxId == Hitbox::Max)
                        break;
                    const auto hitbox = set->getHitbox(hitboxId);
                    if (!hitbox || hitbox->bone < 0 || hitbox->bone >= RECORD_BONES)
                        continue;
                    const auto min = transform(hitbox->bbMin, bones[hitbox->bone]);
                    const auto max = transform(hitbox->bbMax, bones[hitbox->bone]);
                    const auto center = (min + max) * 0.5f;

                    // мультипоинт: центр, края поперёк взгляда, верх головы, точки по оси вытянутых хитбоксов;
                    // размер — ползунок или авто: пятно разброса + погрешность анимации бегущего внутри капсулы
                    std::array<csgo::Vector, 14> points{ center };
                    std::size_t count = 1;
                    const float radius = hitbox->capsuleRadius;
                    if (multipoint && radius > 0.0f) {
                        float scale = maxScale;
                        if (settings.autoScale) {
                            const float blur = (center - eye).length() * cone * 0.5f;
                            const float animError = 0.5f + std::min(record.velocity.length2D(), 300.0f) * 0.006f;
                            scale = std::min(scale, (radius * 0.95f - blur - animError) / radius);
                        }
                        scale *= shrink;
                        if (scale >= 0.2f) {
                            const auto view = normalized(center - eye);
                            csgo::Vector across{ view.y, -view.x, 0.0f };
                            across = across.squareLength() > 0.0001f ? normalized(across) : csgo::Vector{ 1.0f, 0.0f, 0.0f };
                            const float offset = radius * scale;
                            points[count++] = center + across * offset;
                            points[count++] = center - across * offset;
                            if (isHead) {
                                // макушка и две диагонали сверху: голова из-за угла часто видна только верхом
                                points[count++] = center + worldUp * offset;
                                points[count++] = center + (worldUp + across) * (offset * 0.7f);
                                points[count++] = center + (worldUp - across) * (offset * 0.7f);
                            } else {
                                // низ капсулы поперёк взгляда (видно из-под укрытия)
                                points[count++] = center - worldUp * (offset * 0.8f);
                            }
                        }
                        const auto axis = max - min;
                        if (!isHead && flag != HB_Chest && axis.length() > radius && scale > 0.0f) {
                            const float along = 0.5f * std::min(scale + 0.3f, 0.9f);
                            points[count++] = center + axis * along;
                            points[count++] = center - axis * along;
                            // углы капсулы: край по оси + край поперёк
                            if (scale >= 0.2f) {
                                const auto view = normalized(center - eye);
                                csgo::Vector across{ view.y, -view.x, 0.0f };
                                across = across.squareLength() > 0.0001f ? normalized(across) : csgo::Vector{ 1.0f, 0.0f, 0.0f };
                                const float side = radius * scale * 0.7f;
                                points[count++] = center + axis * along + across * side;
                                points[count++] = center - axis * along - across * side;
                            }
                        }
                    }
                    // конечности без мультипоинта: хотя бы концы кости — из-за укрытия чаще торчит кисть/локоть
                    if (isLimb && !multipoint) {
                        const auto axis = max - min;
                        points[count++] = center + axis * 0.4f;
                        points[count++] = center - axis * 0.4f;
                    }
                    // сторона не доказана: середина между тем же хитбоксом в крайних скелетах — если капсулы
                    // перекрываются, точка попадает при любой стороне (проверка безопасности её подтвердит)
                    const auto centerOf = [&](const csgo::matrix3x4* skeleton) {
                        return (transform(hitbox->bbMin, skeleton[hitbox->bone]) + transform(hitbox->bbMax, skeleton[hitbox->bone])) * 0.5f;
                    };
                    if (checkSafe && !sideCertain && (isHead || flag == HB_Chest) && count < points.size()) {
                        const auto middle = (centerOf(record.plus) + centerOf(record.minus)) * 0.5f;
                        if ((middle - center).squareLength() > 1.0f)
                            points[count++] = middle;
                    }
                    // голова по слоям: середина между найденным углом и максимумом той же стороны —
                    // самая вероятная точка, общая для обеих моделей
                    if (layerSide && isHead && count < points.size()) {
                        const auto middle = (center + centerOf(sideExtreme)) * 0.5f;
                        if ((middle - center).squareLength() > 1.0f)
                            points[count++] = middle;
                    }
                    // джиттер: середина головы между двумя фазами — туда, где она будет в любой из них
                    if (checkPhases && isHead && count < points.size()) {
                        const auto middle = (centerOf(list.at(0).bones) + phaseShift[0] + centerOf(list.at(1).bones) + phaseShift[1]) * 0.5f;
                        if ((middle - center).squareLength() > 1.0f)
                            points[count++] = middle;
                    }

                    for (std::size_t p = 0; p < count; ++p) {
                        if (p > 0 && pointBudget-- <= 0)
                            break;
                        const bool safe = isSafe(points[p], isHead);
                        if ((safeRequired || strict) && checkSafe && !safe)
                            continue;
                        // голова у прыгающего/джиттерящего — мимо даже по общей точке (31% и 29%)
                        if (strict && isHead && (source == SideSource::Air || source == SideSource::Jitter))
                            continue;
                        const bool sideSafe = !safe && isSideSafe(points[p], isHead);
                        // Край головы — только если он общий для всех трёх скелетов. Логи 31.10–01.11: край головы
                        // 53% попаданий и 32% промахов резольвера против 62% и 16% у центра; сессия 01.11 01:00 —
                        // край 1 из 8, центр 8 из 12. Край попадает лишь в одну модель, и любая ошибка угла ног
                        // уводит голову из-под него; центр прощает её гораздо лучше.
                        // Раньше небезопасный край головы выкидывался: голова торчит краем из-за укрытия, центр за стеной —
                        // и стрелять «некуда». Теперь край берётся со скидкой (центр при равных всё равно лучше).
                        const bool headEdgeUnsafe = isHead && p > 0 && checkSafe && !safe;
                        // Голова при сомнительной стороне — только в точки, общие для всех трёх скелетов.
                        // Сессия 01.11 01:09–01:21: 4 промаха в «общую точку» головы по слоям с уверенностью 28–58%,
                        // и импакт доказал другую сторону; ещё 2 доказанных — голова в воздухе. Тело по тем же
                        // слоям попадает ~85%, поэтому лучше тело, чем голова наугад. Якорь LBY (100%) и
                        // уверенные слои (≥60%) по-прежнему бьют голову по найденному углу.
                        // Раньше центр головы тут выкидывался — рейдж «фоткал» по зелёной голове. Теперь только штраф ниже.
                        const bool headUnsafe = isHead && headNeedsSafe && checkSafe && !safe;
                        // дальность оружия автоволл не проверяет (у зевса ~183 ед.)
                        if ((points[p] - eye).squareLength() > weaponData->range * weaponData->range)
                            continue;
                        const float baseDamage = autowall::getDamageToPoint(engineInterfaces, interfaces, points[p], weaponData, rage.autowall);
                        if (baseDamage < 1.0f)
                            continue;
                        int group = firstHitGroup(aimCaps, eye, points[p] + normalized(points[p] - eye) * 2.0f);
                        if (group < 0)
                            group = hitbox->group;
                        float damage = autowall::scaleDamage(entity, group, weaponData, baseDamage);
                        // дробовик: дробины × доля, что ляжет в тело (пятно ~d·cone против ~8 ед. корпуса);
                        // точный ответ потом даст хитшанс с суммой по дробинам
                        if (weaponData->bullets > 1) {
                            const float blur = (points[p] - eye).length() * cone + 0.001f;
                            damage *= static_cast<float>(weaponData->bullets) * std::clamp(8.0f * 8.0f / (blur * blur), 0.1f, 1.0f);
                        }
                        // Сквозь стену автоволл переоценивает пробитие («пуля застряла» — 23 промаха с уроном
                        // от 8 до 191). В строгом режиме урон через стену засчитываем с запасом 20%.
                        const float openDamageHere = static_cast<float>(weaponData->damage) * std::pow(weaponData->rangeModifier, (points[p] - eye).length() / 500.0f);
                        const bool throughWall = baseDamage < openDamageHere * 0.97f;
                        const float countedDamage = strict && throughWall ? damage * 0.8f : damage;
                        const bool lethal = countedDamage >= hp;
                        const bool qualified = countedDamage >= static_cast<float>(minDamage);
                        // конечность — если набирает мин. урон (раньше только наповал: торчащую руку игнорили)
                        if (isLimb && !lethal && !qualified)
                            continue;

                        // край тела, не общий для всех скелетов: по логам 58% против 74% у центра
                        float reliability = trust * (p > 0 ? (safe ? 0.9f : 0.75f) : 1.0f);
                        if (sideSafe)
                            reliability *= sideTrust;
                        else if (!safe)
                            reliability *= sideFactor * (isHead && layerSide && !hvh::resolver::extremeActive(i) ? 0.85f : 1.0f);
                        // по игроку идёт перебор вариантов резольвера — угол под вопросом,
                        // голова по нему ненадёжна (тело прощает ошибку в стороне, голова нет)
                        if (isHead && !safe && hvh::resolver::bruteforcing(i))
                            reliability *= 0.7f;
                        if (headUnsafe)
                            reliability *= 0.8f;
                        // Логи 04–08.11 по настоящим записям: голова по догадке 42% против 82% у тела,
                        // небезопасная голова в воздухе 1 из 5, по джиттеру 0 из 3 — при сомнительной стороне тело
                        if (isHead && source == SideSource::Guess && !safe)
                            reliability *= 0.65f;
                        if (!safe)
                            reliability *= headSidePenalty;
                        if (isHead && !safe && (source == SideSource::Air || source == SideSource::Jitter))
                            reliability *= 0.55f;
                        if (headEdgeUnsafe)
                            reliability *= 0.8f;
                        // Прогноз: точка, не попадающая в обе соседние модели (N−1 и N) — скидка. Раньше голову
                        // и врага в воздухе по таким точкам не били вовсе: голова горела зелёным, выстрела не было.
                        if (checkSteps && !stepSafe(points[p]))
                            reliability *= isHead || airborne ? 0.6f : 0.45f;
                        // не попадает в одну из фаз джиттера — промах в половине случаев
                        if (checkPhases && !phaseSafe(points[p], isHead))
                            reliability *= isHead ? 0.4f : 0.6f;
                        // зелёным рисуем только то, что реально пошло в выбор цели
                        if ((lethal || qualified) && viewPointCount < static_cast<int>(viewPoints.size()))
                            viewPoints[viewPointCount++] = { points[p], static_cast<std::uint8_t>(lethal ? 2 : 1) };
                        // сквозь стену: автоволл иногда переоценивает пробитие (логи: 51 «пуля застряла»)
                        const float openDamage = static_cast<float>(weaponData->damage) * std::pow(weaponData->rangeModifier, (points[p] - eye).length() / 500.0f);
                        if (baseDamage < openDamage * 0.97f)
                            reliability *= 0.85f;
                        // польза: убить сейчас 1.5; при заряженном DT пол-хп — убийство двумя пулями 1.1; иначе доля хп
                        const float value = lethal ? 1.5f : doubleTapKill && damage * 2.0f >= hp ? 1.1f : damage / hp;
                        const float score = value * reliability;
                        const AimPoint point{ points[p], damage, baseDamage, isHead, p > 0, group, sideSafe, safe && checkSafe };
                        offer(any, point, score, safe, reliability);
                        if (qualified)
                            offer(isHead ? head : body, point, score, safe, reliability);
                        // центр уже убивает надёжно — края хуже по хитшансу, автоволл на них не тратим
                        if (p == 0 && lethal && reliability >= trust * 0.99f && !viewOnly)
                            break;
                    }
                }
            }

            // ---- выбор точки на этой записи ----
            // при равной пользе — тело: отдача та же, а хитбокс больше и хитшанс выше
            const bool headFirst = head.score > body.score && !(settings.bodyIfLethal && body.score >= 0.0f && body.point.damage >= hp);
            Scored chosen = headFirst ? head : body;
            Scored other = headFirst ? body : head;
            int effectiveMinDamage = minDamage;
            if (chosen.score < 0.0f) {
                // мин. урон не набран — стреляем только с форсшутом (в лучшую точку). Раньше при пороге
                // «убить» (HP+N) бил в тело от пол-хп в расчёте на вторую пулю DT: со скаутом второй пули
                // нет, и МД 100 на деле не работал — рейдж бил в тело без форсшута.
                if (any.score < 0.0f || !forceShot) {
                    statusText = safeRequired ? "нет безопасных точек" : "мало урона";
                    if (c == 0) {
                        shownPoints = viewPoints;
                        shownPointCount = viewPointCount;
                        shownResolver = resolverText;
                    }
                    continue;
                }
                chosen = any;
                other = {};
                effectiveMinDamage = std::max(1, static_cast<int>(std::floor(any.point.damage)));
            }

            if (c == 0 || chosen.score > playerBest.score) {
                shownPoints = viewPoints;
                shownPointCount = viewPointCount;
                shownResolver = resolverText;
            }
            // при равной пользе — запись раньше по приоритету (onshot, стоит, свежая, старая)
            if (chosen.score > playerBest.score) {
                auto& t = playerBest;
                t.index = i;
                t.point = chosen.point;
                t.fallback = other.score >= 0.0f ? other.point : AimPoint{};
                t.score = chosen.score;
                t.reliability = chosen.reliability;
                t.fov = fov;
                t.minDamage = effectiveMinDamage;
                t.kind = candidates[c].kind;
                t.simulationTime = record.simulationTime;
                t.origin = record.origin;
                t.backtrackTicks = list.count && candidates[c].age > 0 ? timeToTicks(list.at(0).simulationTime - record.simulationTime, interval) : 0;
                t.updatesAhead = candidates[c].updatesAhead;
                t.aheadPhase = predictedRecord ? aheadPhase : -1.0f;
                t.headScore = head.score;
                t.bodyScore = body.score;
                t.choke = record.choked;
                t.side = side;
                t.source = source;
                t.phase = record.jitterPhase;
                t.guess = guess;
                t.layerDelta = source == SideSource::Layers ? std::remainder(record.resolvedYaw - record.eyeAngles.y, 360.0f) : 0.0f;
                t.layerMax = record.maxDesync;
                t.layerConfidence = record.resolvedConfidence;
                t.animFixed = record.resolvedByAnimation || record.animFixed;
                t.flipping = flipping;
                t.set = set;
                t.studio = studio;
                std::copy(bones, bones + RECORD_BONES, t.bones);
                t.haveVariants = record.variants;
                if (record.variants) {
                    std::copy(record.plus, record.plus + RECORD_BONES, t.plus);
                    std::copy(record.minus, record.minus + RECORD_BONES, t.minus);
                }
            }
            // надёжное убийство — лучше не найти, остальные записи не сканируем
            if (playerBest.score >= 1.5f * 0.99f)
                break;
        }

        // ---- в топ целей (между игроками — по пользе выстрела) ----
        if (playerBest.score >= 0.0f) {
            int slot = topCount;
            while (slot > 0 && (playerBest.score > top[slot - 1].score || (playerBest.score == top[slot - 1].score && fov < top[slot - 1].fov)))
                --slot;
            if (slot < static_cast<int>(top.size())) {
                for (int k = std::min(topCount, static_cast<int>(top.size()) - 1); k > slot; --k)
                    top[k] = top[k - 1];
                topCount = std::min(topCount + 1, static_cast<int>(top.size()));
                top[slot] = playerBest;
            }
            std::scoped_lock lock{ viewMutex };
            auto& view = views[i];
            view.aimAt = realtime;
            view.aimPoint[0] = playerBest.point.position.x;
            view.aimPoint[1] = playerBest.point.position.y;
            view.aimPoint[2] = playerBest.point.position.z;
            view.aimLethal = playerBest.point.damage >= hp;
            view.aimDamage = static_cast<int>(playerBest.point.damage);
            view.aimGroup = playerBest.point.group;
            view.aimedNow = false;
        }

        std::scoped_lock lock{ viewMutex };
        auto& view = views[i];
        view.resolver = isBot[i] ? "бот" : shownResolver;
        view.pointsAt = realtime;
        view.pointCount = shownPointCount;
        for (int k = 0; k < shownPointCount; ++k) {
            view.points[k][0] = shownPoints[k].first.x;
            view.points[k][1] = shownPoints[k].first.y;
            view.points[k][2] = shownPoints[k].first.z;
            view.pointState[k] = shownPoints[k].second;
        }
    }

    // Диагностика «держит цель и не стреляет»: цель есть дольше 0.3 с без выстрела — причина в лог (раз за эпизод)
    static float hesitateSince = -1.0f;
    static bool hesitateLogged = false;
    // видимых целей нет — дормант-аимбот по слышимым врагам (зевс — только по видимым)
    if (topCount == 0 && !visualOnly && canShoot && rage.dormantAim && !taser
        && dormantShot(cmd, engineInterfaces, clientInterfaces, interfaces, memory, weaponData, settings, maxClients, eye, cone,
                       needScope, scopeAllowed, stopMode, stopMaxSpeed, moveVars, interval, realtime)) {
        firedAt = serverTime;
        firedCycle = weaponData->cycletime;
        return true;
    }
    if (topCount == 0 || visualOnly) {
        hesitateSince = -1.0f;
        hesitateLogged = false;
        return false;
    }
    if (hesitateSince < 0.0f || realtime < hesitateSince)
        hesitateSince = realtime;
    const auto hesitate = [&](const char* reason) {
        if (hesitateLogged || realtime - hesitateSince < 0.3f)
            return;
        hesitateLogged = true;
        const auto& t = top[0];
        char text[256];
        std::snprintf(text, sizeof(text), "[рейдж] держит %s %.1f с без выстрела: %s | %s, урон %.0f, нужно %d, хш нужен %d%%, скорость %.0f",
            playerName(t.index).c_str(), realtime - hesitateSince, reason, t.point.isHead ? "голова" : "тело", t.point.damage, t.minDamage,
            settings.hitchance, localPlayer.get().velocity().length2D());
        fileLog(text);
    };
    // оружие будет готово к следующему тику — та команда уйдёт сразу, а не через весь фейклаг
    if (!canShoot && readyIn <= interval)
        tickbase::sendNextPacket();

    target = top[0].index;
    // с нажатым прыжком тик уже воздушный: стоп и скоп только ломают бхоп
    const bool onGround = (localPlayer.get().flags() & FL_ONGROUND) && !(cmd->buttons & csgo::UserCmd::IN_JUMP);
    const bool scoping = needScope && onGround;
    if (scoping && scopeAllowed)
        cmd->buttons |= csgo::UserCmd::IN_ATTACK2;
    if (stopMode != AutoStop::Off && onGround && (canShoot || readySoon || settings.autoStopEarly))
        autoStop(cmd, stopMode, stopMaxSpeed, interval, moveVars);
    // идёт передёргивание/перезарядка — это не «держит без выстрела»
    if (!canShoot && !readySoon) {
        hesitateSince = realtime;
        statusText = "перезарядка";
        return false;
    }
    if (scoping) {
        statusText = "скоуп";
        hesitate(statusText);
        return false;
    }
    if (!canShoot) {
        statusText = readySoon ? "автостоп" : "перезарядка";
        hesitate(statusText);
        return false;
    }

    // ---- хитшанс: цель, её вторая группа, следующая цель — первая, где набрался ----
    static Capsules capsules;
    csgo::Vector angle{};
    int chance = 0, firstChance = -1, chosenSlot = -1;
    for (int k = 0; k < topCount && chosenSlot < 0; ++k) {
        auto& t = top[k];
        buildCapsules(*t.set, t.bones, capsules);
        const auto entity = csgo::Entity::from(retSpoofGadgets->client, clientInterfaces.getEntityList().getEntity(t.index));
        angle = (t.point.position - eye).toAngle();
        chance = hitchance(weapon, weaponData, entity, eye, angle, capsules, t.point.baseDamage, t.minDamage);
        if (firstChance < 0)
            firstChance = chance;
        // Прогнозная запись («вперёд», экстраполяция) — отдельный разговор. Хитшанс считает только
        // разброс по сидам и ничего не знает о том, что сама запись может быть выдумана неверно, а
        // статистика по ней плохая: за 07.11 по прогнозу 18 попаданий на 47 промахов (28%), по
        // реальным записям — 216 на 59 (79%). Это почти половина всех промахов сессии.
        // Поэтому планку для прогноза поднимаем: пусть лучше рейдж промолчит и дождётся записи.
        const bool predictedShot = t.kind == RecordKind::Ahead || t.kind == RecordKind::Extrapolated;
        const int needChance = predictedShot ? std::min(settings.hitchance + 25, 95) : settings.hitchance;
        // Хитшанс по прогнозу почти всегда 100% — мажет сама модель. Порог надёжности: медленному оружию
        // (скаут, AWP, дигл/R8 — промах стоит секунду, за которую убивают нас) по плохому прогнозу не
        // стрелять, ждать настоящую запись; автоматам промах дешевле — порог ниже. Наповал по надёжной
        // точке проходит всегда (тело на земле без джиттера ≈ 0.7).
        if (predictedShot) {
            const auto group = groupOf(weapon);
            const bool slowWeapon = group == WeaponGroup::Scout || group == WeaponGroup::Awp || group == WeaponGroup::Heavy;
            const float needReliability = slowWeapon ? 0.42f : 0.28f;
            if (t.reliability < needReliability) {
                if (firstChance < 0)
                    firstChance = 0;
                continue;
            }
        }
        if (chance < needChance && t.fallback.damage > 0.0f) {
            const auto fallbackAngle = (t.fallback.position - eye).toAngle();
            if (const int fallbackChance = hitchance(weapon, weaponData, entity, eye, fallbackAngle, capsules, t.fallback.baseDamage, t.minDamage);
                fallbackChance >= needChance) {
                t.point = t.fallback;
                angle = fallbackAngle;
                chance = fallbackChance;
            }
        }
        if (chance >= needChance)
            chosenSlot = k;
    }
    if (chosenSlot < 0) {
        static char text[32];
        std::snprintf(text, sizeof(text), "хитшанс %d%%", firstChance);
        statusText = text;
        hesitate(statusText);
        return false;
    }
    if (chosenSlot != 0) {
        std::swap(top[0], top[chosenSlot]);
        buildCapsules(*top[0].set, top[0].bones, capsules);
    }
    const auto& best = top[0];
    target = best.index;

    // Хайдшоты: выстрел только в команде, которая уйдёт в чоке — следующая (с углами AA) её закроет.
    // Этот тик отправляется — стреляем следующим, его заранее держим в чоке
    if (tickbase::hideShotsActive() && tickbase::isInstalled() && tickbase::sendPacket() && !tickbase::isShifting()) {
        tickbase::chokeNextPacket();
        statusText = "хайдшот: выстрел в след. тике";
        return false;
    }

    // Фейкдак: стреляем только с последнего тика стоя (Movement.cpp: первая половина пачки — встаём,
    // вторая — садимся). Это самая высокая точка глаза за цикл — ради неё ФД и нужен. Раньше рейдж
    // стрелял на любом тике: с полуприседа (в укрытие) и форсил пакет — враги видели нас стоя.
    if (movement::isFakeDucking() && tickbase::chokedCommands() != tickbase::fakeDuckChoke() / 2 - 1) {
        statusText = "фейкдак: ждём верхнюю точку";
        return false;
    }

    // ---- выстрел ----
    auto aimAngle = angle - localPlayer.get().aimPunchAngle() * 2.0f;
    aimAngle.normalize();
    cmd->viewangles = aimAngle;
    cmd->buttons |= csgo::UserCmd::IN_ATTACK;
    // сервер откатит цель ровно в эту запись; прогноз и живую модель — tickcount не трогаем
    if (best.kind != RecordKind::Live && best.kind != RecordKind::Extrapolated && best.kind != RecordKind::Ahead)
        // как на сервере: targettick = tick_count − TIME_TO_TICKS(lerp), lerp округляется отдельно
        cmd->tickCount = timeToTicks(best.simulationTime, interval) + timeToTicks(lerpTime, interval);
    firedAt = serverTime;
    firedCycle = weaponData->cycletime;
    hesitateSince = -1.0f;
    hesitateLogged = false;

    auto& shot = shots[best.index];
    shot = {};
    shot.time = realtime;
    shot.eye = eye;
    shot.aimPoint = best.point.position;
    shot.isHead = best.point.isHead;
    shot.edge = best.point.edge;
    shot.damage = static_cast<int>(best.point.damage);
    shot.hitchance = chance;
    shot.side = best.side;
    shot.source = best.source;
    shot.phase = best.phase;
    shot.guess = best.guess;
    shot.layerDelta = best.layerDelta;
    shot.layerMax = best.layerMax;
    shot.layerConfidence = best.layerConfidence;
    shot.animFixed = best.animFixed;
    shot.sideSafe = best.point.sideSafe;
    shot.safe = best.point.safe;
    shot.flipping = best.flipping;
    shot.kind = best.kind;
    shot.backtrackTicks = best.backtrackTicks;
    shot.updatesAhead = best.updatesAhead;
    shot.aheadPhase = best.aheadPhase;
    shot.headScore = best.headScore;
    shot.bodyScore = best.bodyScore;
    shot.predictedSimulationTime = best.simulationTime;
    shot.predictedOrigin = best.origin;
    shot.jitter = jittering(brains[best.index]) || desyncFlipping(brains[best.index]);
    shot.updateTime = static_cast<float>(std::max(best.choke, 1)) * interval;
    shot.set = best.set;
    shot.aimed = capsules;
    if (best.haveVariants) {
        buildCapsules(*best.set, best.plus, shot.plus);
        buildCapsules(*best.set, best.minus, shot.minus);
    }
    lastShotIndex = best.index;
    {
        auto& brain = brains[best.index];
        std::uint8_t& attempts = best.point.isHead ? brain.headShots : brain.bodyShots;
        attempts = static_cast<std::uint8_t>(std::min<int>(attempts + 1, 250));
    }

    {
        std::scoped_lock lock{ viewMutex };
        views[best.index].aimedNow = true;
        lastShot.at = realtime;
        lastShot.result = 0;
        lastShot.count = 0;
        // скелет: каждая кость хитбоксов — линия к родителю, по матрицам той записи, куда стреляли
        if (best.studio) {
            for (int b = 0; b < std::min(best.studio->numBones, RECORD_BONES) && lastShot.count < 64; ++b) {
                const auto bone = best.studio->getBone(b);
                if (!bone || bone->parent < 0 || bone->parent >= RECORD_BONES || !(bone->flags & BONE_USED_BY_HITBOX))
                    continue;
                const auto from = best.bones[b].origin();
                const auto to = best.bones[bone->parent].origin();
                auto& n = lastShot.count;
                lastShot.a[n][0] = from.x; lastShot.a[n][1] = from.y; lastShot.a[n][2] = from.z;
                lastShot.b[n][0] = to.x; lastShot.b[n][1] = to.y; lastShot.b[n][2] = to.z;
                ++n;
            }
        }
    }
    {
        static char text[96];
        std::snprintf(text, sizeof(text), "выстрел: %s, %s, %s", best.point.isHead ? "голова" : "тело",
            sideSourceNames[static_cast<int>(best.source)], kindNames[static_cast<int>(best.kind)]);
        statusText = text;
    }

    if (!rage.silent)
        engineInterfaces.getEngine().setViewAngles(cmd->viewangles);
    if (!tickbase::isShifting())
        tickbase::onShot();
    return true;
}

} // namespace

}

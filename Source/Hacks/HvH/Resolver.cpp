#include <algorithm>
#include <array>
#include <bit>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "HvhConfig.h"
#include "Ragebot.h"
#include "Resolver.h"
#include "ResolverScoring.h"

#include <Helpers.h>
#include <Interfaces/ClientInterfaces.h>
#include <Interfaces/EngineInterfaces.h>
#include <Memory.h>
#include <Netvars.h>
#include <fnv.h>
#include <CSGO/AnimationLayer.h>
#include <CSGO/AnimState.h>
#include <CSGO/Engine.h>
#include <CSGO/Entity.h>
#include <CSGO/EntityList.h>
#include <CSGO/GlobalVars.h>
#include <CSGO/LocalPlayer.h>
#include <RetSpoof/RetSpoofGadgets.h>
#include <CSGO/Vector.h>

namespace hvh::resolver
{

namespace
{

constexpr int MAX_PLAYERS = 65;
constexpr int LAYERS = csgo::ANIMATION_LAYER_COUNT;
constexpr int MAX_CANDIDATES = 24;
constexpr std::size_t STATE_SIZE = sizeof(csgo::AnimState);

// Режимы коррекции поверх угла от слоёв: если слои врут (сильный джиттер, дефенсив),
// счёт по попаданиям/промахам сам уводит на зеркальный или половинный угол.
enum Mode { ModeLayers, ModeMirror, ModeHalf, ModeCount };
constexpr const char* modeNames[]{ "слои", "зеркало", "половина" };
// счёт держим в рамках: иначе серия промахов по чужой вине запирает режим навсегда
constexpr int scoreLimit = 8;

struct Layers {
    std::array<csgo::AnimationLayer, LAYERS> items{};
    int count = 0;
};

struct PlayerState {
    int userId = -1;
    bool tracked = false;
    float lastSimulationTime = 0.0f;
    float lastEyeYaw = 0.0f;
    std::uint8_t chokeHistory = 0;

    bool valid = false;
    float footYaw = 0.0f;
    float eyeYaw = 0.0f;
    float delta = 0.0f;
    float maxDesync = 58.0f;
    float confidence = 0.0f;
    float error = 0.0f;
    int signals = 0;
    int candidates = 0;
    float lastFoundTime = -1.0f;    // simtime, когда угол последний раз нашёлся по слоям
    float foundRatio = 0.0f;        // дельта той находки в долях максимума, без коррекции по промахам
    bool carried = false;           // угол не из слоёв, а доведён от прошлого найденного

    std::array<int, ModeCount> score{};
    int mode = ModeLayers;

    bool haveShadow = false;
    alignas(16) std::byte shadow[STATE_SIZE]{};
    Layers shadowLayers;

    Layers afterClient;
    bool haveAfterClient = false;

    char status[224]{};
};

std::array<PlayerState, MAX_PLAYERS> states{};

bool offsetsBroken = false;     // animstate не по тому офсету — дальше не лезем
int layerDiffUpdates = 0;       // обновлений, где сетевые слои отличались от клиентских
int layerUpdates = 0;
bool layersUnavailable = false;
bool warnedUnavailable = false;
bool warnedOffsets = false;
int layoutFailures = 0;

bool diagnosticsPrinted = false;
bool cleared = false;           // состояния уже сброшены (чтобы не чистить их каждый тик)

void consoleWarn(const Memory& memory, const char* text) noexcept
{
    static constexpr std::uint8_t yellow[4]{ 255, 210, 90, 255 };
    if (memory.conColorMsg)
        memory.conColorMsg(yellow, "%s\n", text);
    hvh::ragebot::logToFile(text);
}

// Где резольвер по слоям останавливается — сводка раз в 20 с только в лог-файл (в консоль не пишем).
// По логам ни разу не было выстрела в режиме «слои», а предупреждений тоже нет — значит, выходит молча.
enum Exit { ExitNoAnimstate, ExitNoLayers, ExitFewLayers, ExitLayout, ExitInsane, ExitUnavailable, ExitSilent, ExitLowConfidence, ExitCarried, ExitValid, ExitCount };
constexpr const char* exitNames[]{ "нет animstate", "нет слоёв", "слоёв меньше 13", "раскладка не та", "слои-мусор",
    "сервер не шлёт слои", "слои молчат", "низкая уверенность", "доведён от прошлого", "угол найден" };
std::array<int, ExitCount> exits{};
float lastSummary = 0.0f;

void countExit(Exit exit) noexcept
{
    ++exits[exit];
}

void writeSummary(const Memory& memory) noexcept
{
    const float now = memory.globalVars->realtime;
    if (now - lastSummary < 20.0f)
        return;
    lastSummary = now;
    int total = 0;
    for (const int count : exits)
        total += count;
    if (total == 0)
        return;
    char text[512];
    int length = std::snprintf(text, sizeof(text), "[ECLIPSE] резольвер по слоям за 20 с (%d разборов):", total);
    for (int i = 0; i < ExitCount && length > 0 && length < static_cast<int>(sizeof(text)); ++i) {
        if (exits[i])
            length += std::snprintf(text + length, sizeof(text) - length, " %s %d;", exitNames[i], exits[i]);
    }
    hvh::ragebot::logToFile(text);
    exits.fill(0);
}

[[nodiscard]] float normalizeYaw(float yaw) noexcept
{
    return std::remainder(yaw, 360.0f);
}

// Слои, пришедшие по сети, обязаны выглядеть как слои: вес и цикл в [0, 1], последовательность задана
[[nodiscard]] bool sane(const Layers& layers) noexcept
{
    for (int i = 0; i < layers.count; ++i) {
        const auto& layer = layers.items[i];
        if (!std::isfinite(layer.weight) || !std::isfinite(layer.cycle) || !std::isfinite(layer.playbackRate))
            return false;
        if (layer.weight < -0.01f || layer.weight > 1.01f || layer.cycle < -0.01f || layer.cycle > 1.01f)
            return false;
        if (layer.sequence < -1 || layer.sequence > 4096)
            return false;
    }
    return true;
}

[[nodiscard]] bool differentLayers(const Layers& a, const Layers& b) noexcept
{
    if (a.count != b.count)
        return true;
    for (int i = 0; i < a.count; ++i) {
        if (a.items[i].sequence != b.items[i].sequence
            || std::abs(a.items[i].weight - b.items[i].weight) > 0.0005f
            || std::abs(a.items[i].cycle - b.items[i].cycle) > 0.0005f
            || std::abs(a.items[i].playbackRate - b.items[i].playbackRate) > 0.0005f)
            return true;
    }
    return false;
}

void captureLayers(const csgo::Entity& entity, Layers& out) noexcept
{
    const auto layers = entity.animOverlays();
    const int count = std::min(entity.animOverlayCount(), LAYERS);
    out.count = layers && count > 0 ? count : 0;
    for (int i = 0; i < out.count; ++i)
        out.items[i] = layers[i];
}

// Прогон анимации пишет не только в animstate: кое-что ложится в самого игрока. Позы и слои
// возвращаем по netvar-офсетам, а это — все поля игрока, в которые пишет сама Update и её helper'ы
// (нашли по разбору: +0xfd4 это m_flPlaybackRate, +0xfec ещё одно поле анимации, и два блока
// «флаг + три времени» — таймеры анимации). Клиент всё равно пересчитает их своим обновлением
// в этом же кадре, так что на незнакомой сборке просто не трогаем, вместо записи по чужому офсету.
struct EntityScratch {
    int offset;
    int size;
};
constexpr EntityScratch entityScratch[]{ { 0xfd4, 4 }, { 0xfec, 4 }, { 0xd83a, 14 }, { 0x10c9a, 14 } };
constexpr int knownAnimStateOffset = 0xa250;

[[nodiscard]] bool knownBuild() noexcept
{
    return csgo::animStateOffset == knownAnimStateOffset;
}

void saveEntityScratch(const csgo::Entity& entity, std::byte* out) noexcept
{
    if (!knownBuild())
        return;
    const auto base = reinterpret_cast<const std::byte*>(entity.getPOD());
    std::size_t at = 0;
    for (const auto& field : entityScratch) {
        std::memcpy(out + at, base + field.offset, field.size);
        at += field.size;
    }
}

void restoreEntityScratch(const csgo::Entity& entity, const std::byte* saved) noexcept
{
    if (!knownBuild())
        return;
    const auto base = reinterpret_cast<std::byte*>(entity.getPOD());
    std::size_t at = 0;
    for (const auto& field : entityScratch) {
        std::memcpy(base + field.offset, saved + at, field.size);
        at += field.size;
    }
}

void restoreLayers(const Layers& source, csgo::AnimationLayer* layers) noexcept
{
    for (int i = 0; i < source.count; ++i)
        layers[i] = source.items[i];
}

// Один прогон анимации врага с подставленным углом ног. Возвращает угол ног ПОСЛЕ обновления:
// движок сам доворачивает ноги к взгляду и обрезает их по максимальному десинку, так что
// результат всегда физически возможен — именно его и надо отдавать рейджботу.
float simulate(csgo::AnimState* animstate, csgo::AnimationLayer* layers, const Memory& memory,
    const std::byte* base, const Layers& baseLayers, float candidate, float eyeYaw, float pitch, float delta) noexcept
{
    std::memcpy(animstate, base, STATE_SIZE);
    restoreLayers(baseLayers, layers);
    animstate->footYaw = candidate;
    animstate->footYawLast = candidate;
    animstate->lastUpdateTime = memory.globalVars->currenttime - delta;
    animstate->lastUpdateFrame = memory.globalVars->framecount - 1;
    memory.updateAnimationState(animstate, true, eyeYaw, pitch);
    return animstate->footYaw;
}

void resolvePlayer(PlayerState& state, const csgo::Entity& entity, const Memory& memory, float interval, int steps) noexcept
{
    const bool wasValid = state.valid;
    state.valid = false;
    state.signals = 0;
    state.carried = false;

    auto* animstate = entity.getAnimstate();
    auto* layers = entity.animOverlays();
    const int count = std::min(entity.animOverlayCount(), LAYERS);
    if (!animstate || !layers || count < LAYERS || !memory.updateAnimationState) {
        countExit(!animstate ? ExitNoAnimstate : !layers ? ExitNoLayers : ExitFewLayers);
        return;
    }

    // Проверки перед тем, как копировать и прогонять animstate. Раскладка структуры (0x3b8 байт,
    // офсеты полей) разобрана под клиент 13546: если офсет animstate найден другой или поля не
    // похожи на себя, значит сборка не та — тогда лучше остаться без слоёв, чем писать в чужую память.
    const bool layoutOk = knownBuild()
        && animstate->player == entity.getPOD()
        && animstate->maxBodyYawDegrees > 30.0f && animstate->maxBodyYawDegrees < 80.0f
        && animstate->duckAmount >= 0.0f && animstate->duckAmount <= 1.0f
        // доля скорости считается от шага (макс. скорость * 0.52): бег ~1.9, распрыжка больше —
        // старый предел 1.1 отбрасывал всех бегущих, а 50 таких подряд выключали резольвер насовсем
        && std::isfinite(animstate->walkSpeedPortion) && animstate->walkSpeedPortion >= 0.0f && animstate->walkSpeedPortion < 20.0f;
    if (!layoutOk) {
        countExit(ExitLayout);
        // Один игрок мог просто не успеть обзавестись готовым animstate (только заспавнился) —
        // выключаем резольвер лишь когда не сходится подряд у всех
        if (++layoutFailures > 50) {
            offsetsBroken = true;
            if (!warnedOffsets) {
                warnedOffsets = true;
                consoleWarn(memory, "[ECLIPSE] animstate не совпал с разобранной раскладкой — резольвер по геометрии");
            }
        }
        return;
    }
    layoutFailures = 0;

    Layers net;
    net.count = count;
    std::copy(layers, layers + count, net.items.begin());
    if (!sane(net)) {
        countExit(ExitInsane);
        return;
    }

    // Шлёт ли сервер слои вообще: сравниваем с тем, что оставила клиентская анимация в прошлом кадре
    if (state.haveAfterClient) {
        ++layerUpdates;
        if (differentLayers(net, state.afterClient))
            ++layerDiffUpdates;
        if (layerUpdates > 300 && layerDiffUpdates == 0) {
            layersUnavailable = true;
            if (!warnedUnavailable) {
                warnedUnavailable = true;
                consoleWarn(memory, "[ECLIPSE] сервер не присылает слои анимации — резольвер работает по геометрии");
            }
            countExit(ExitUnavailable);
            return;
        }
    }

    static std::byte backupState[STATE_SIZE];
    static Layers backupLayers;
    std::memcpy(backupState, animstate, STATE_SIZE);
    backupLayers = net;
    std::array<float, 24> backupPoses{};
    std::copy(std::begin(entity.poseParameters()), std::end(entity.poseParameters()), backupPoses.begin());
    std::byte backupScratch[40]{};
    saveEntityScratch(entity, backupScratch);
    const auto backupAbsAngles = entity.getAbsAngle();
    const auto backupRotation = entity.angRotation();

    const auto eyeAngles = entity.eyeAngles();
    const float eyeYaw = eyeAngles.y;
    const float pitch = eyeAngles.x;
    const float maxDesync = std::clamp(animstate->getMaxDesyncAngle(), 0.0f, 60.0f);
    state.maxDesync = maxDesync;

    // Сколько времени прошло на сервере между прошлым и этим обновлением врага
    const float updateDelta = state.tracked
        ? std::clamp(entity.simulationTime() - state.lastSimulationTime, interval, 0.25f) : interval;

    // Углы ног ДО обновления: сетка от взгляда на ±максимальный десинк, LBY (сервер ставит его
    // настоящим углом ног, когда враг стоит) и продолжение нашей прошлой догадки.
    std::array<float, MAX_CANDIDATES> candidates{};
    int candidateCount = 0;
    // Враг не чокает — клиент считает его анимацию в такт с сервером, перебирать нечего:
    // хватит проверить то, что уже есть (и LBY рядом). Десинка без чока не бывает.
    const bool desyncing = !state.tracked || state.chokeHistory != 0;
    const int half = desyncing && maxDesync > 1.0f ? std::clamp(steps / 2, 1, 10) : 0;
    for (int i = -half; i <= half && candidateCount < MAX_CANDIDATES; ++i)
        candidates[candidateCount++] = normalizeYaw(eyeYaw + maxDesync * static_cast<float>(i) / static_cast<float>(std::max(half, 1)));
    const auto addCandidate = [&](float yaw) {
        yaw = normalizeYaw(yaw);
        for (int i = 0; i < candidateCount; ++i) {
            if (std::abs(normalizeYaw(candidates[i] - yaw)) < 1.0f)
                return;
        }
        if (candidateCount < MAX_CANDIDATES)
            candidates[candidateCount++] = yaw;
    };
    addCandidate(entity.lby());
    addCandidate(animstate->footYaw);
    if (state.haveShadow)
        addCandidate(reinterpret_cast<const csgo::AnimState*>(state.shadow)->footYaw);
    if (desyncing && state.tracked) {
        // Взгляд успел перескочить (джиттер, ручной антиаим) — ноги до обновления стояли
        // относительно ПРОШЛОГО взгляда и могут быть дальше максимального десинка от нового
        addCandidate(state.lastEyeYaw);
        addCandidate(state.lastEyeYaw + maxDesync);
        addCandidate(state.lastEyeYaw - maxDesync);
    }

    // слои всех кандидатов подряд: по кандидату на строку из LAYERS слоёв
    static std::array<csgo::AnimationLayer, MAX_CANDIDATES * LAYERS> simulated{};
    static std::array<float, MAX_CANDIDATES> resultYaw{};
    static std::array<float, MAX_CANDIDATES> error{};
    const std::byte* base = state.haveShadow ? state.shadow : backupState;
    const Layers& baseLayers = state.haveShadow ? state.shadowLayers : backupLayers;

    for (int c = 0; c < candidateCount; ++c) {
        resultYaw[c] = simulate(animstate, layers, memory, base, baseLayers, candidates[c], eyeYaw, pitch, updateDelta);
        std::copy(layers, layers + count, simulated.begin() + static_cast<std::size_t>(c) * count);
    }

    const auto scored = scoreCandidates(simulated.data(), candidateCount, net.items.data(), count, resultYaw.data(), error.data());

    if (scored.signals == 0) {
        // Слои ни на что не отреагировали — нового угла нет. Тень двигаем от прошлого угла ног: прогон
        // анимации тот же, что у сервера, так что если раньше угол был найден, то и сейчас тень с ним
        // совпадает — отдаём её (иначе резольвер терял угол на каждом «молчащем» обновлении,
        // и выстрелы «вперёд» шли наугад). Без недавней находки — геометрия.
        const bool continuous = state.haveShadow;
        int closest = 0;
        const float previous = reinterpret_cast<const csgo::AnimState*>(base)->footYaw;
        for (int c = 1; c < candidateCount; ++c) {
            if (std::abs(normalizeYaw(candidates[c] - previous)) < std::abs(normalizeYaw(candidates[closest] - previous)))
                closest = c;
        }
        const float carriedYaw = simulate(animstate, layers, memory, base, baseLayers, candidates[closest], eyeYaw, pitch, updateDelta);
        std::memcpy(state.shadow, animstate, STATE_SIZE);
        captureLayers(entity, state.shadowLayers);
        state.haveShadow = true;
        if (continuous && wasValid && state.lastFoundTime > 0.0f
            && entity.simulationTime() - state.lastFoundTime < 1.0f) {
            state.footYaw = carriedYaw;
            state.eyeYaw = eyeYaw;
            state.delta = normalizeYaw(carriedYaw - eyeYaw);
            state.confidence *= 0.85f;
            state.valid = true;
            state.carried = true;
            countExit(ExitCarried);
        } else {
            state.valid = false;
            state.confidence = 0.0f;
            countExit(ExitSilent);
        }
    } else {
        state.signals = scored.signals;
        state.candidates = candidateCount;
        state.footYaw = resultYaw[scored.best];
        state.eyeYaw = eyeYaw;
        state.delta = normalizeYaw(state.footYaw - eyeYaw);
        state.error = scored.error;
        state.confidence = scored.confidence;
        // Кандидаты не разошлись — считать угол найденным нельзя, кроме случая, когда он и так
        // почти равен взгляду (там ошибиться нечем)
        state.valid = state.confidence > 0.05f || std::abs(state.delta) < 5.0f;
        if (state.valid) {
            state.lastFoundTime = entity.simulationTime();
            state.foundRatio = std::clamp(state.delta / std::max(state.maxDesync, 1.0f), -1.0f, 1.0f);
        }
        countExit(state.valid ? ExitValid : ExitLowConfidence);

        // Тень: состояние после лучшего кандидата — с него поедет следующее обновление,
        // чтобы наша история анимации шла в ногу с серверной
        simulate(animstate, layers, memory, base, baseLayers, candidates[scored.best], eyeYaw, pitch, updateDelta);
        std::memcpy(state.shadow, animstate, STATE_SIZE);
        captureLayers(entity, state.shadowLayers);
        state.haveShadow = true;
    }

    if (!diagnosticsPrinted) {
        diagnosticsPrinted = true;
        char text[320];
        std::snprintf(text, sizeof(text),
            "[ECLIPSE] резольвер по слоям: animstate +0x%x, слои +0x%x (%d шт.), maxBodyYaw %.0f°, "
            "сетевые слои %s, кандидатов %d",
            csgo::animStateOffset, csgo::animOverlayOffset, count,
            animstate->maxBodyYawDegrees, layerDiffUpdates > 0 ? "приходят" : "пока не отличались", candidateCount);
        static constexpr std::uint8_t grey[4]{ 190, 190, 190, 255 };
        if (memory.conColorMsg)
            memory.conColorMsg(grey, "%s\n", text);
        hvh::ragebot::logToFile(text);
    }

    // Вернуть игре её состояние: дальше кадр считает анимацию и кости сам
    std::memcpy(animstate, backupState, STATE_SIZE);
    restoreLayers(backupLayers, layers);
    std::copy(backupPoses.begin(), backupPoses.end(), std::begin(entity.poseParameters()));
    restoreEntityScratch(entity, backupScratch);
    const_cast<csgo::Vector&>(entity.getAbsAngle()) = backupAbsAngles;
    entity.angRotation() = backupRotation;

    if (state.carried) {
        std::snprintf(state.status, sizeof(state.status),
            "слои молчат, угол доведён от прошлого: дельта %+.1f° (макс %.0f°), уверенность %.0f%%, режим %s",
            state.delta, state.maxDesync, state.confidence * 100.0f, modeNames[std::clamp(state.mode, 0, ModeCount - 1)]);
    } else if (state.signals == 0) {
        std::snprintf(state.status, sizeof(state.status),
            "слои молчат (ноги не крутятся, кандидатов %d, макс %.0f°) — угол от геометрии",
            candidateCount, state.maxDesync);
    } else {
        std::snprintf(state.status, sizeof(state.status),
            "дельта %+.1f° (макс %.0f°), уверенность %.0f%%, ошибка %.2f, сигналов %d, кандидатов %d, режим %s",
            state.delta, state.maxDesync, state.confidence * 100.0f, state.error, state.signals, state.candidates,
            modeNames[std::clamp(state.mode, 0, ModeCount - 1)]);
    }
}

[[nodiscard]] int bestMode(const PlayerState& state) noexcept
{
    int best = ModeLayers;
    for (int mode = 1; mode < ModeCount; ++mode) {
        if (state.score[mode] > state.score[best])
            best = mode;
    }
    return best;
}

}

void onNetUpdate(const EngineInterfaces& engineInterfaces, const ClientInterfaces& clientInterfaces, const Memory& memory) noexcept
{
    if (!config.rage.enabled || !config.rage.resolver || !config.rage.layerResolver || offsetsBroken || layersUnavailable)
        return;
    if (!memory.updateAnimationState || csgo::animStateOffset <= 0) {
        offsetsBroken = true;
        if (!warnedOffsets) {
            warnedOffsets = true;
            consoleWarn(memory, "[ECLIPSE] не нашёл CCSGOPlayerAnimState::Update (игра обновилась?) — резольвер по геометрии");
        }
        return;
    }
    if (!localPlayer || !localPlayer.get().isAlive())
        return;

    cleared = false;
    const float interval = memory.globalVars->intervalPerTick;
    const int maxClients = std::min(engineInterfaces.getEngine().getMaxClients(), MAX_PLAYERS - 1);
    for (int i = 1; i <= maxClients; ++i) {
        const auto entity = csgo::Entity::from(retSpoofGadgets->client, clientInterfaces.getEntityList().getEntity(i));
        auto& state = states[i];
        if (entity.getPOD() == nullptr || entity.getPOD() == localPlayer.get().getPOD() || entity.getNetworkable().isDormant()
            || !entity.isAlive() || !entity.isOtherEnemy(memory, localPlayer.get())) {
            state = {};
            continue;
        }

        csgo::PlayerInfo playerInfo{};
        const bool haveInfo = engineInterfaces.getEngine().getPlayerInfo(i, playerInfo);
        if (haveInfo && playerInfo.fakeplayer) {   // боты не десинкают
            state.valid = false;
            continue;
        }
        if (state.userId != (haveInfo ? playerInfo.userId : -1)) {
            state = {};
            state.userId = haveInfo ? playerInfo.userId : -1;
        }

        const float simulationTime = entity.simulationTime();
        if (state.tracked && simulationTime <= state.lastSimulationTime) {
            // время не двинулось (нет нового обновления) или ушло назад (дефенсив) — считать нечего
            if (simulationTime < state.lastSimulationTime) {
                state.haveShadow = false;
                state.valid = false;
            }
            continue;
        }

        if (state.tracked && interval > 0.0f) {
            const int ticks = static_cast<int>(std::lround((simulationTime - state.lastSimulationTime) / interval));
            if (ticks >= 1 && ticks <= 17)
                state.chokeHistory = static_cast<std::uint8_t>((state.chokeHistory << 1) | (ticks > 1 ? 1 : 0));
            else
                state.haveShadow = false;   // пропустили обновления (дормант, выход из PVS) — история анимации уже не та
        }

        state.mode = bestMode(state);
        resolvePlayer(state, entity, memory, interval, config.rage.resolverSteps);

        state.lastSimulationTime = simulationTime;
        state.lastEyeYaw = entity.eyeAngles().y;
        state.tracked = true;
        state.haveAfterClient = false;
    }
    writeSummary(memory);
}

void afterClientAnimation(const EngineInterfaces& engineInterfaces, const ClientInterfaces& clientInterfaces) noexcept
{
    if (!config.rage.enabled || !config.rage.resolver || !config.rage.layerResolver || offsetsBroken || layersUnavailable)
        return;

    const int maxClients = std::min(engineInterfaces.getEngine().getMaxClients(), MAX_PLAYERS - 1);
    for (int i = 1; i <= maxClients; ++i) {
        auto& state = states[i];
        if (!state.tracked || state.haveAfterClient)
            continue;
        const auto entity = csgo::Entity::from(retSpoofGadgets->client, clientInterfaces.getEntityList().getEntity(i));
        if (entity.getPOD() == nullptr || entity.getNetworkable().isDormant())
            continue;
        captureLayers(entity, state.afterClient);
        state.haveAfterClient = state.afterClient.count > 0;
    }
}

bool available() noexcept
{
    return !offsetsBroken && !layersUnavailable;
}

bool valid(int index) noexcept
{
    return index > 0 && index < MAX_PLAYERS && available() && states[index].valid;
}

float rawFootYaw(int index) noexcept
{
    return index > 0 && index < MAX_PLAYERS ? states[index].footYaw : 0.0f;
}

float footYaw(int index) noexcept
{
    if (index <= 0 || index >= MAX_PLAYERS)
        return 0.0f;
    const auto& state = states[index];
    switch (state.mode) {
    case ModeMirror: return normalizeYaw(state.eyeYaw - state.delta);
    case ModeHalf: return normalizeYaw(state.eyeYaw + state.delta * 0.5f);
    default: return state.footYaw;
    }
}

float delta(int index) noexcept
{
    if (index <= 0 || index >= MAX_PLAYERS)
        return 0.0f;
    return normalizeYaw(footYaw(index) - states[index].eyeYaw);
}

float maxDesync(int index) noexcept
{
    return index > 0 && index < MAX_PLAYERS ? states[index].maxDesync : 0.0f;
}

bool lastFound(int index, float& simulationTime, float& ratio) noexcept
{
    if (index <= 0 || index >= MAX_PLAYERS || !available() || states[index].lastFoundTime <= 0.0f)
        return false;
    simulationTime = states[index].lastFoundTime;
    ratio = states[index].foundRatio;
    return true;
}

float confidence(int index) noexcept
{
    return index > 0 && index < MAX_PLAYERS ? states[index].confidence : 0.0f;
}

const char* modeName(int index) noexcept
{
    if (index <= 0 || index >= MAX_PLAYERS)
        return "";
    return modeNames[std::clamp(states[index].mode, 0, ModeCount - 1)];
}

const char* statusLine(int index) noexcept
{
    return index > 0 && index < MAX_PLAYERS ? states[index].status : "";
}

void onHit(int index) noexcept
{
    if (index <= 0 || index >= MAX_PLAYERS)
        return;
    auto& state = states[index];
    auto& score = state.score[std::clamp(state.mode, 0, ModeCount - 1)];
    score = std::min(score + 2, scoreLimit);
}

void onMiss(int index) noexcept
{
    if (index <= 0 || index >= MAX_PLAYERS)
        return;
    auto& state = states[index];
    auto& score = state.score[std::clamp(state.mode, 0, ModeCount - 1)];
    score = std::max(score - 1, -scoreLimit);
}

void onRoundStart() noexcept
{
    for (auto& state : states) {
        state.score.fill(0);
        state.haveShadow = false;
        state.valid = false;
    }
}

void clear() noexcept
{
    if (cleared)
        return;
    cleared = true;
    states.fill({});
    layerUpdates = 0;
    layerDiffUpdates = 0;
    layersUnavailable = false;
}

}

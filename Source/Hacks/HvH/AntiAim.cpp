#include <algorithm>
#include <array>
#include <cstdio>
#include <cmath>
#include <iterator>
#include <mutex>
#include <limits>
#include <optional>
#include <random>
#include <vector>

#include "AntiAim.h"
#include "Defensive.h"
#include "HvhConfig.h"
#include "Movement.h"
#include "Ragebot.h"
#include "Tickbase.h"

#include <Interfaces/ClientInterfaces.h>
#include <Interfaces/EngineInterfaces.h>
#include <Memory.h>
#include <Netvars.h>
#include <fnv.h>
#include <CSGO/ClientClass.h>
#include <CSGO/Constants/ClassId.h>
#include <CSGO/Engine.h>
#include <CSGO/WeaponData.h>
#include <CSGO/Constants/GameEventNames.h>
#include <CSGO/Entity.h>
#include <CSGO/EngineTrace.h>
#include <Helpers.h>
#include <CSGO/GameEvent.h>
#include <CSGO/EntityList.h>
#include <CSGO/GlobalVars.h>
#include <CSGO/NetworkChannel.h>
#include <CSGO/LocalPlayer.h>
#include <CSGO/UserCmd.h>
#include <CSGO/Vector.h>

// Антиаим по образцу hysteria: билдер по состояниям, кнопки (ручной, фристенд, edge yaw, roll),
// safe head, легит AA на E, плавные переходы, анти-брутфорс (переворот / подстройка / стойка пресетов)
// и дефенсив AA — скрытые углы на тиках, когда тикбейс откатился и лагкомп по нам не пишется.

namespace hvh::antiaim
{

namespace
{

constexpr int FL_ONGROUND = 1 << 0;
constexpr int FL_DUCKING = 1 << 1;

std::mt19937 rng{ std::random_device{}() };

Manual manual = Manual::None;
int side = 1;
AaState activeState = AaState::Default;
float currentRoll = 0.0f;
std::mutex extendedMutex;
ExtendedView extendedShown;
float tickDesync = 0.0f;        // увод тела этого тика: сторона × величина (0 — десинка нет)

// ---- переключения билдера ----
// Модификатор и джиттер десинка шагают не на каждом отправленном пакете, а раз в «задержку» пакетов.
// phase — номер переключения, от него считаются все режимы.
int phase = 0;
int packetsSinceSwitch = 0;
int switchWait = 1;
int fluctuateStep = 0;
bool switchLong = false;
float switchRandom = 0.0f;      // «рандомизация»: добавка к размаху, своя на каждое переключение
float randomYaw = 0.0f;         // модификатор «Рандом»: держим значение до следующего переключения
int randomSide = 1;             // десинк «Рандом»
float rotatePhase = 0.0f;
float spinYaw = 0.0f;
// «Скиттер»: рваный порядок, по которому резольверу трудно угадать следующий шаг
constexpr int skitterSequence[]{ -1, 1, 0, -1, 1, 0, -1, 0, 1, -1, 0, 1 };

// ---- цикл чока ----
// Десинк — это РАЗНИЦА между чокнутой и отправленной командами, поэтому всё, что шагает по
// тикам (модификатор, спин, натуральность, сторона), обязано стоять на месте внутри одного
// цикла чока: иначе эта разница прибавляется к уводу тела и ползунок десинка врёт.
bool lastSentPacket = true;     // прошлый тик ушёл на сервер → этот начинает новый цикл
bool cycleValid = false;
AntiAimState cycleState{};      // настройки билдера, замороженные на цикл
float cycleModifier = 0.0f;     // добавка модификатора, замороженная на цикл
int pinnedSide = 1;             // сторона увода тела, выбранная на цикл
bool pinnedValid = false;

// ---- плавные переходы (натуральность) ----
AntiAimState blendFrom{};
AntiAimState lastBlended{};
float blendStart = -1.0f;
AaState blendState = AaState::Default;

// ---- анти-брутфорс ----
struct Impact {
    int userId;
    csgo::Vector position;
};
std::vector<Impact> pendingImpacts;
bool antiBruteFlip = false;
float antiBruteWeight = 0.0f;   // «Подстройка»: случайный сдвиг настроек в ±силе
float lastAntiBrute = -1.0f;
int rackIndex = -1;             // какой пресет стойки сейчас загружен (−1 — свои настройки)
bool haveOriginal = false;
AntiAimPreset original;         // что было до стойки — вернём по таймеру/в начале раунда

// ---- дефенсив AA ----
struct SnapRuntime {
    bool active = false;
    int ticks = 0;              // тиков в текущем окне
    int apex = 0;               // длина окна на входе
    int counter = 0;            // номер окна
    int seenCounter = -1;       // номер окна, который мы уже обслуживаем (для сброса на новом)
    int delayed = 0;
    bool jitter = false;
    bool haveRandomPitch = false, haveRandomYaw = false;
    float randomPitch = 0.0f, randomYaw = 0.0f;
} snap;

// ---- легит AA ----
bool legitWait = false;

// ---- флики и увод от прицела ----
int flickCounter = 0;
int flickAfterShift = 0;
float flickDirection = 1.0f;
bool wasFlicking = false;

void toggleManual(const KeyBind& key, Manual direction) noexcept
{
    if (key.isSet() && key.isPressed())
        manual = manual == direction ? Manual::None : direction;
}

[[nodiscard]] float distanceToSegment(const csgo::Vector& point, const csgo::Vector& a, const csgo::Vector& b) noexcept
{
    const auto ab = b - a;
    const float lengthSquared = ab.x * ab.x + ab.y * ab.y + ab.z * ab.z;
    float t = 0.0f;
    if (lengthSquared > 0.0f)
        t = std::clamp(((point.x - a.x) * ab.x + (point.y - a.y) * ab.y + (point.z - a.z) * ab.z) / lengthSquared, 0.0f, 1.0f);
    return (point - (a + ab * t)).length();
}

// ---- контр-резольвер ----
// Анти-брут раньше реагировал вслепую: враг выстрелил — перевернуть сторону (или сдвинуть настройки).
// Но куда он целился, видно по самой пуле: импакт задаёт луч, луч проходит у нашей головы слева,
// справа или по центру от оси взгляда — это и есть сторона, которую выбрал его резольвер. По истории
// таких выстрелов на каждого врага видно, как он резольвит:
//   * «следит» — бьёт ровно в нашу настоящую сторону (слои/LBY): сторону менять бесполезно, спасает
//     только величина (low delta) и смена на каждом цикле;
//   * «статика» — бьёт туда же, куда в прошлый раз; «перебор» — в обратную от прошлого выстрела;
//   * «центр» — в середину (low delta / без десинка).
// Перед каждым циклом чока выбираем сторону и величину десинка, при которых его СЛЕДУЮЩИЙ выстрел
// вероятнее всего пройдёт мимо головы. Нет данных по врагу — работает билдер как раньше.

struct PoseSample {
    float time = -1.0f;         // realtime отправки
    float eyeYaw = 0.0f;        // отправленный взгляд
    float desync = 0.0f;        // увод тела: сторона × градусы (0 — без десинка)
    csgo::Vector origin{};
    float eyeZ = 0.0f;
};
std::array<PoseSample, 128> poseHistory{};
int poseNext = 0;

void pushPose(const PoseSample& sample) noexcept
{
    poseHistory[poseNext] = sample;
    poseNext = (poseNext + 1) % static_cast<int>(poseHistory.size());
}

[[nodiscard]] const PoseSample* poseAt(float time) noexcept
{
    const PoseSample* best = nullptr;
    float bestError = 0.25f;    // дальше четверти секунды — история не про этот выстрел
    for (const auto& sample : poseHistory) {
        if (sample.time < 0.0f)
            continue;
        const float error = std::abs(sample.time - time);
        if (error < bestError) {
            bestError = error;
            best = &sample;
        }
    }
    return best;
}

struct AimObservation {
    float time = -1.0f;
    float lateral = 0.0f;       // куда прошёл луч у головы: + вправо от оси взгляда, − влево (ед.)
    int ourSide = 0;            // наша настоящая сторона в тот момент
};

struct EnemyModel {
    int userId = -1;
    std::array<AimObservation, 12> observations{};
    int count = 0;              // всего записано (кольцо по 12)
    float lastClassified = -1.0f;
    AimObservation last{};
};
std::array<EnemyModel, 65> enemyModels{};

// Знак геометрии: при стороне +1 (ноги левее взгляда) голова смещается влево от оси взгляда, луч в неё
// даёт lateral < 0. Знак уточняется попаданиями в голову: тогда враг целился точно в нашу сторону.
float geometryVotes = -1.0f;
constexpr float centerBand = 2.5f;     // |lateral| меньше — выстрел «по центру»

[[nodiscard]] int guessedSide(float lateral) noexcept
{
    if (std::abs(lateral) < centerBand)
        return 0;
    return (lateral > 0.0f) == (geometryVotes > 0.0f) ? 1 : -1;
}

void learnServerStyle(int guess, int previousGuess, int ourSide) noexcept;

// Выстрел врага в нас: разобрать, в какую сторону он целился
void observeShot(int shooterIndex, int userId, const csgo::Vector& shooterEye, const csgo::Vector& impact, float realtime, float latency) noexcept
{
    if (shooterIndex <= 0 || shooterIndex >= 65)
        return;
    auto& model = enemyModels[shooterIndex];
    if (model.userId != userId) {
        model = {};
        model.userId = userId;
    }
    // одна пуля — несколько импактов (пробитие): берём первый
    if (model.lastClassified >= 0.0f && realtime - model.lastClassified < 0.2f && realtime >= model.lastClassified)
        return;
    // Сервер считал выстрел по нашей позе, которую враг видел: ушла от нас ~RTT + его интерп назад
    const auto* pose = poseAt(realtime - latency - 0.08f);
    if (!pose)
        return;
    auto direction = impact - shooterEye;
    const float length = direction.length();
    if (length < 1.0f)
        return;
    direction = direction / length;
    // ближайшая к нашей вертикальной оси точка луча (по горизонтали)
    const csgo::Vector toAxis{ pose->origin.x - shooterEye.x, pose->origin.y - shooterEye.y, 0.0f };
    const float horizontal = direction.x * direction.x + direction.y * direction.y;
    if (horizontal < 1e-4f)
        return;
    const float t = (toAxis.x * direction.x + toAxis.y * direction.y) / horizontal;
    if (t <= 0.0f)
        return;
    const auto point = shooterEye + direction * t;
    // только выстрелы в голову: в тело целятся в центр, стороны там не видно
    if (point.z < pose->eyeZ - 12.0f || point.z > pose->eyeZ + 8.0f)
        return;
    const float radians = Helpers::deg2rad(pose->eyeYaw);
    const csgo::Vector right{ std::sin(radians), -std::cos(radians), 0.0f };
    const csgo::Vector offset{ point.x - pose->origin.x, point.y - pose->origin.y, 0.0f };
    if (offset.length() > 20.0f)
        return;
    AimObservation observation;
    observation.time = realtime;
    observation.lateral = offset.x * right.x + offset.y * right.y;
    observation.ourSide = pose->desync > 1.0f ? 1 : pose->desync < -1.0f ? -1 : 0;
    learnServerStyle(guessedSide(observation.lateral), model.last.time >= 0.0f ? guessedSide(model.last.lateral) : 2, observation.ourSide);
    model.observations[model.count % model.observations.size()] = observation;
    ++model.count;
    model.lastClassified = realtime;
    model.last = observation;
}

// Враг попал нам в голову: он целился ровно в нашу сторону — уточняем знак геометрии
void observeHeadHit(int shooterIndex, float realtime) noexcept
{
    if (shooterIndex <= 0 || shooterIndex >= 65)
        return;
    const auto& last = enemyModels[shooterIndex].last;
    if (last.time < 0.0f || realtime - last.time > 0.5f || last.ourSide == 0 || std::abs(last.lateral) < centerBand)
        return;
    const float vote = (last.lateral > 0.0f) == (last.ourSide > 0) ? 1.0f : -1.0f;
    geometryVotes = std::clamp(geometryVotes + vote, -6.0f, 6.0f);
    if (geometryVotes == 0.0f)
        geometryVotes = vote;
}

struct CounterPlan {
    bool active = false;
    int side = 1;
    float magnitude = 1.0f;     // доля десинка из билдера: 1 — полный, 0.45 — low delta
    const char* style = "";
};
CounterPlan counterPlan;

// Шанс, что выстрел в сторону g попадёт в голову при нашей стороне s и доле десинка m (0.45…1):
// полный десинк уводит голову от центра, но в неё попадает выстрел «в нашу сторону»; low delta — наоборот
[[nodiscard]] float headRisk(int g, int s, float m) noexcept
{
    const float t = std::clamp((m - 0.45f) / 0.55f, 0.0f, 1.0f);
    if (g == s)
        return 0.45f + 0.4f * t;
    if (g == 0)
        return 0.55f - 0.45f * t;
    return 0.2f - 0.17f * t;
}

// Стиль резольвера врага: веса «следит / статика / перебор / центр» и его последний выбор
struct EnemyStyle {
    float follow = 0.0f, same = 0.0f, flip = 0.0f, center = 0.0f;
    int lastGuess = 0;
    [[nodiscard]] float mass() const noexcept { return follow + same + flip + center; }
};

// Общая память по серверу: на хвх-серверах у половины лобби один и тот же чит, и первый выстрел
// незнакомого врага разумнее встречать тем, как нас резольвят остальные, а не наугад
EnemyStyle serverStyle;

void learnServerStyle(int guess, int previousGuess, int ourSide) noexcept
{
    constexpr float decay = 0.9f;
    serverStyle.follow *= decay;
    serverStyle.same *= decay;
    serverStyle.flip *= decay;
    serverStyle.center *= decay;
    if (guess == 0)
        serverStyle.center += 1.0f;
    else if (ourSide != 0 && guess == ourSide)
        serverStyle.follow += 1.0f;
    if (guess != 0 && previousGuess != 0 && previousGuess != 2)
        (guess == previousGuess ? serverStyle.same : serverStyle.flip) += 1.0f;
}

[[nodiscard]] EnemyStyle styleOf(const EnemyModel& model, float realtime) noexcept
{
    EnemyStyle style;
    const int n = std::min<int>(model.count, static_cast<int>(model.observations.size()));
    if (n == 0 || realtime - model.last.time > 90.0f)
        return style;
    int previous = 2;
    for (int k = 0; k < n; ++k) {
        const auto& o = model.observations[(model.count - n + k) % model.observations.size()];
        const float w = std::pow(0.8f, static_cast<float>(n - 1 - k));
        const int g = guessedSide(o.lateral);
        if (g == 0)
            style.center += w;
        else if (o.ourSide != 0 && g == o.ourSide)
            style.follow += w;
        if (previous != 2 && g != 0 && previous != 0)
            (g == previous ? style.same : style.flip) += w;
        previous = g;
        style.lastGuess = g;
    }
    return style;
}

// Вероятность, что следующий выстрел врага с таким стилем попадёт в голову при стороне s
[[nodiscard]] float riskFor(const EnemyStyle& own, int s, float low) noexcept
{
    // своё весит полностью, серверное — как полвыстрела на каждый свой недостающий до трёх
    EnemyStyle st = own;
    const float borrow = std::max(0.0f, 3.0f - own.mass()) / 3.0f * 0.5f;
    st.follow += serverStyle.follow * borrow;
    st.same += serverStyle.same * borrow;
    st.flip += serverStyle.flip * borrow;
    st.center += serverStyle.center * borrow;
    const float prior = 0.8f;
    const float total = st.mass() + prior;
    float risk = st.follow / total * headRisk(s, s, low) + st.center / total * headRisk(0, s, low);
    if (st.lastGuess != 0) {
        risk += st.same / total * headRisk(st.lastGuess, s, low);
        risk += st.flip / total * headRisk(-st.lastGuess, s, low);
    } else {
        // последнего выбора нет: «статика»/«перебор» — монетка
        risk += (st.same + st.flip) / total * 0.5f * (headRisk(1, s, low) + headRisk(-1, s, low));
    }
    risk += prior / total * (headRisk(1, s, low) + headRisk(-1, s, low) + headRisk(0, s, low)) / 3.0f;
    return risk;
}

struct Threat {
    int index = 0;
    float weight = 0.0f;
};

// План на цикл против всех, кто может стрелять: риск — взвешенная сумма по врагам
// (на кого смотрим — вес 1, остальные живые рядом — 0.5)
CounterPlan planAgainst(const Threat* threats, int threatCount, float realtime, int fallbackSide) noexcept
{
    CounterPlan plan;
    std::array<EnemyStyle, 16> styles{};
    float known = serverStyle.mass();
    EnemyStyle dominant{};
    for (int t = 0; t < threatCount && t < static_cast<int>(styles.size()); ++t) {
        styles[t] = styleOf(enemyModels[threats[t].index], realtime);
        known += styles[t].mass();
        if (threats[t].weight >= 1.0f)
            dominant = styles[t];
    }
    if (threatCount == 0 || known < 0.5f)
        return plan;

    // Варианты: сторона × доля десинка. Из почти равных (риск в пределах 0.02 от лучшего) — случайный:
    // иначе при равенстве мы каждый раз выбирали бы одно и то же, и это читалось бы так же, как статика.
    struct Option {
        int side;
        float magnitude;
        float risk;
    };
    std::array<Option, 6> options{};
    int optionCount = 0;
    float bestRisk = 1e9f;
    float weightSum = 0.0f;
    for (int t = 0; t < threatCount && t < static_cast<int>(styles.size()); ++t)
        weightSum += threats[t].weight;
    for (const int s : { 1, -1 }) {
        for (const float m : { 1.0f, 0.7f, 0.45f }) {
            float risk = 0.0f;
            for (int t = 0; t < threatCount && t < static_cast<int>(styles.size()); ++t)
                risk += threats[t].weight * riskFor(styles[t], s, m);
            risk /= std::max(weightSum, 1e-3f);
            options[optionCount++] = { s, m, risk };
            bestRisk = std::min(bestRisk, risk);
        }
    }
    std::array<int, 6> near{};
    int nearCount = 0;
    for (int k = 0; k < optionCount; ++k) {
        if (options[k].risk <= bestRisk + 0.02f)
            near[nearCount++] = k;
    }
    // сторона билдера — небольшой приоритет: если она среди лучших, рисунок AA остаётся его
    int pick = near[std::uniform_int_distribution<int>{ 0, std::max(nearCount - 1, 0) }(rng)];
    for (int k = 0; k < nearCount; ++k) {
        if (options[near[k]].side == fallbackSide && options[near[k]].risk <= bestRisk + 0.005f
            && std::uniform_int_distribution<int>{ 0, 1 }(rng))
            pick = near[k];
    }
    plan.side = options[pick].side;
    plan.magnitude = options[pick].magnitude;
    plan.active = true;
    // подпись — по тому, на кого смотрим (своё, иначе серверное)
    const auto& shown = dominant.mass() >= 0.5f ? dominant : serverStyle;
    const float strongest = std::max({ shown.follow, shown.same, shown.flip, shown.center });
    plan.style = strongest < 0.5f ? "мало данных" : strongest == shown.follow ? "следит" : strongest == shown.center ? "центр"
        : strongest == shown.same ? "статика" : "перебор";
    // «следит» — его резольвер видит нашу сторону: меняем её каждый цикл, чтобы запись,
    // по которой он целится, успевала устареть
    if (strongest >= 0.5f && strongest == shown.follow)
        plan.side = counterPlan.active ? -counterPlan.side : plan.side;
    return plan;
}

void storePreset(AntiAimPreset& out) noexcept
{
    out.states = config.antiAim.states;
    out.snaps = config.antiAim.snaps;
}

void loadPreset(const AntiAimPreset& preset) noexcept
{
    config.antiAim.states = preset.states;
    config.antiAim.snaps = preset.snaps;
}

void resetAntiBrute() noexcept
{
    antiBruteFlip = false;
    antiBruteWeight = 0.0f;
    lastAntiBrute = -1.0f;
    if (haveOriginal) {
        loadPreset(original);
        haveOriginal = false;
    }
    rackIndex = -1;
}

// Сработал анти-брутфорс: враг стрелял рядом с нами или мы выстрелили сами
void triggerAntiBrute(float realtime) noexcept
{
    const auto& aa = config.antiAim;
    // одна пуля даёт несколько импактов (пробитие) — не чаще раза в 0.25 с
    if (lastAntiBrute >= 0.0f && realtime - lastAntiBrute < 0.25f)
        return;
    lastAntiBrute = realtime;
    switch (static_cast<AntiBruteMode>(aa.antiBruteMode)) {
    case AntiBruteMode::Counter:
        // сторону выберет модель врага в начале следующего цикла — по тому, куда он сейчас целился
        return;
    case AntiBruteMode::Adjust: {
        const float power = std::clamp(aa.antiBrutePower, 1, 100) * 0.01f;
        antiBruteWeight = std::uniform_real_distribution<float>{ -power, power }(rng);
        break;
    }
    case AntiBruteMode::PresetRack: {
        const int count = static_cast<int>(aa.presetRack.size());
        if (count == 0) {
            antiBruteFlip = !antiBruteFlip;
            break;
        }
        if (!haveOriginal) {
            storePreset(original);
            haveOriginal = true;
        }
        if (aa.antiBruteOrder == 0 && count > 1) {
            int next = rackIndex;
            while (next == rackIndex)
                next = std::uniform_int_distribution<int>{ 0, count - 1 }(rng);
            rackIndex = next;
        } else {
            rackIndex = (rackIndex + 1) % count;
        }
        loadPreset(aa.presetRack[rackIndex]);
        break;
    }
    default:
        antiBruteFlip = !antiBruteFlip;
        break;
    }
    // новая сторона — и новая фаза джиттера: его перебор опирался на старый ритм
    ++phase;
}

struct ImpactListener : csgo::GameEventListener {
    void fireGameEvent(csgo::GameEventPOD* eventPointer) override
    {
        const auto event = csgo::GameEvent::from(retSpoofGadgets->client, eventPointer);
        if (pendingImpacts.size() < 64)
            pendingImpacts.push_back({ event.getInt("userid"), { event.getFloat("x"), event.getFloat("y"), event.getFloat("z") } });
    }
};
ImpactListener impactListener;
bool impactListenerRegistered = false;

// Пуля врага прошла рядом с нами — и к нам ближе, чем к любому союзнику (как в hysteria: стреляли в нас)
void processImpacts(const EngineInterfaces& engineInterfaces, const ClientInterfaces& clientInterfaces, const Memory& memory) noexcept
{
    if (pendingImpacts.empty())
        return;
    const int localIndex = localPlayer.get().getNetworkable().index();
    auto body = localPlayer.get().getAbsOrigin();
    body.z += 36.0f;
    const auto head = localPlayer.get().getEyePosition();
    const int maxClients = engineInterfaces.getEngine().getMaxClients();
    float latency = 0.0f;
    if (const auto channel = engineInterfaces.getEngine().getNetworkChannel()) {
        const auto net = csgo::NetworkChannel::from(retSpoofGadgets->client, channel);
        latency = net.getLatency(0) + net.getLatency(1);
    }
    for (const auto& impact : pendingImpacts) {
        const int shooterIndex = engineInterfaces.getEngine().getPlayerForUserID(impact.userId);
        if (shooterIndex <= 0 || shooterIndex == localIndex)
            continue;
        const auto shooter = csgo::Entity::from(retSpoofGadgets->client, clientInterfaces.getEntityList().getEntity(shooterIndex));
        if (!shooter.getPOD() || shooter.getNetworkable().isDormant() || !shooter.isOtherEnemy(memory, localPlayer.get()))
            continue;
        // m_vecViewOffset у чужих игроков не приходит — глаза примерно на 64 над ногами
        auto shooterEye = shooter.getAbsOrigin();
        shooterEye.z += 64.0f;
        const float ours = std::min(distanceToSegment(body, shooterEye, impact.position), distanceToSegment(head, shooterEye, impact.position));
        if (ours > 80.0f)
            continue;
        bool closerTeammate = false;
        for (int i = 1; i <= maxClients && !closerTeammate; ++i) {
            if (i == localIndex)
                continue;
            const auto mate = csgo::Entity::from(retSpoofGadgets->client, clientInterfaces.getEntityList().getEntity(i));
            if (!mate.getPOD() || mate.getNetworkable().isDormant() || !mate.isAlive() || mate.isOtherEnemy(memory, localPlayer.get()))
                continue;
            auto center = mate.getAbsOrigin();
            center.z += 36.0f;
            closerTeammate = distanceToSegment(center, shooterEye, impact.position) < ours;
        }
        if (!closerTeammate) {
            observeShot(shooterIndex, impact.userId, shooterEye, impact.position, memory.globalVars->realtime, latency);
            triggerAntiBrute(memory.globalVars->realtime);
            // в файл: выстрелы в нас (одна пуля — несколько импактов, не чаще раза в 0.3 с на стрелка).
            // Вместе с «[по нам]» это даёт процент, с которым по нам попадают
            static std::array<float, 65> lastLogged{};
            const float now = memory.globalVars->realtime;
            if (shooterIndex < 65 && (now - lastLogged[shooterIndex] > 0.3f || now < lastLogged[shooterIndex])) {
                lastLogged[shooterIndex] = now;
                csgo::PlayerInfo info;
                const bool named = engineInterfaces.getEngine().getPlayerInfo(shooterIndex, info);
                char text[256];
                const auto& seen = enemyModels[shooterIndex].last;
                const bool fresh = seen.time >= 0.0f && now - seen.time < 0.05f;
                const int aimed = fresh ? guessedSide(seen.lateral) : 2;
                std::snprintf(text, sizeof(text), "[в нас стреляли] %s, пуля в %.0f ед. | AA: %s, сторона %+d%s, целился: %s, контр: %s", named ? info.name : "?", ours,
                    stateName(), side, snap.active ? ", скрытые углы" : "",
                    aimed == 2 ? "не в голову" : aimed == 0 ? "центр" : aimed > 0 ? "+" : "−",
                    counterPlan.active ? counterPlan.style : "нет");
                ragebot::logToFile(text);
            }
        }
    }
    pendingImpacts.clear();
}

// ---- состояние ----

// Состояние для билдера: воздух (с приседом), присед (стоя/в движении), шаг на shift, бег, стоя
AaState detectState(const csgo::UserCmd* cmd) noexcept
{
    const auto& player = localPlayer.get();
    const int flags = player.flags();
    const bool onGround = (flags & FL_ONGROUND) && !(cmd->buttons & csgo::UserCmd::IN_JUMP);
    const bool ducking = (cmd->buttons & csgo::UserCmd::IN_DUCK) || (flags & FL_DUCKING) || movement::isFakeDucking();
    const float speed = player.velocity().length2D();
    const bool moving = speed > 5.0f || std::abs(cmd->forwardmove) > 1.0f || std::abs(cmd->sidemove) > 1.0f;
    if (!onGround)
        return ducking ? AaState::AirCrouch : AaState::Air;
    if (ducking)
        return moving ? AaState::Sneaking : AaState::Crouching;
    if (moving && (cmd->buttons & csgo::UserCmd::IN_SPEED))
        return AaState::Walking;
    return moving ? AaState::Running : AaState::Standing;
}

// Какие настройки работают в этом состоянии: «Фейклаг» без эксплойтов главнее; «Воздух + присед»
// без своих — «Воздух»; «Присед в движении» — «Присед»; остальное без своих — «Общее»
AaState resolveState(AaState state) noexcept
{
    const auto& states = config.antiAim.states;
    const auto own = [&states](AaState s) { return states[static_cast<int>(s)].override; };
    // «Фейклаг» — состояние для долгого чока: пока копится заряд (DT, дефенсив, «Дефенсив» эксплойтов),
    // чок всё равно обрезан до 1 тика, и это состояние не наше
    const bool exploitActive = tickbase::doubleTapEnabled() || config.defensive.enabled || config.antiAim.defensiveAa;
    if (!exploitActive && own(AaState::Fakelag))
        return AaState::Fakelag;
    if (state == AaState::AirCrouch && !own(state))
        state = AaState::Air;
    if (state == AaState::Sneaking && !own(state))
        state = AaState::Crouching;
    return own(state) ? state : AaState::Default;
}

[[nodiscard]] int lerpInt(int a, int b, float t) noexcept
{
    return static_cast<int>(std::lround(static_cast<float>(a) + static_cast<float>(b - a) * t));
}

// Натуральность: числа плывут от прошлого состояния к новому, переключатели — на середине перехода
AntiAimState blended(AaState state, float realtime) noexcept
{
    const auto& target = config.antiAim.states[static_cast<int>(state)];
    const auto& aa = config.antiAim;
    if (!aa.naturality || aa.naturalityTime <= 0.0f) {
        blendStart = -1.0f;
        blendState = state;
        lastBlended = target;
        return target;
    }
    if (state != blendState) {
        blendFrom = lastBlended;
        blendStart = realtime;
        blendState = state;
    }
    const float t = blendStart < 0.0f ? 1.0f : std::clamp((realtime - blendStart) / aa.naturalityTime, 0.0f, 1.0f);
    AntiAimState out = t >= 0.5f ? target : blendFrom;
    out.yawOffset = lerpInt(blendFrom.yawOffset, target.yawOffset, t);
    out.addLeft = lerpInt(blendFrom.addLeft, target.addLeft, t);
    out.addRight = lerpInt(blendFrom.addRight, target.addRight, t);
    out.degree = lerpInt(blendFrom.degree, target.degree, t);
    out.spinSpeed = lerpInt(blendFrom.spinSpeed, target.spinSpeed, t);
    out.randomize = lerpInt(blendFrom.randomize, target.randomize, t);
    out.desyncLeft = lerpInt(blendFrom.desyncLeft, target.desyncLeft, t);
    out.desyncRight = lerpInt(blendFrom.desyncRight, target.desyncRight, t);
    lastBlended = out;
    return out;
}

int nextSwitchWait(const AntiAimState& st) noexcept
{
    const int ticks = std::clamp(st.delayTicks, 1, 16);
    switch (static_cast<SwitchDelay>(st.delayMode)) {
    case SwitchDelay::Static: return ticks;
    case SwitchDelay::Fluctuate:
        fluctuateStep = fluctuateStep % ticks + 1;
        return fluctuateStep;
    case SwitchDelay::Switch:
        switchLong = !switchLong;
        return switchLong ? ticks : 1;
    case SwitchDelay::Random: return std::uniform_int_distribution<int>{ 1, ticks }(rng);
    default: return 1;
    }
}

// Добавка модификатора к yaw. Размах — весь ход: центр-джиттер ±½, N-way от −½ до +½ и т. д.
float modifierYaw(const AntiAimState& st, float degree) noexcept
{
    const float half = degree * 0.5f;
    switch (static_cast<YawModifier>(st.modifier)) {
    case YawModifier::Jitter: return phase & 1 ? degree : 0.0f;
    case YawModifier::CenterJitter: return phase & 1 ? half : -half;
    case YawModifier::Spin:
        spinYaw = std::remainder(spinYaw + static_cast<float>(st.spinSpeed), 360.0f);
        return spinYaw;
    case YawModifier::Random: return randomYaw * (st.degree > 0 ? degree / static_cast<float>(st.degree) : 0.0f);
    case YawModifier::FiveWay: {
        const int ways = std::clamp(st.ways, 3, 8);
        const float t = static_cast<float>(phase % ways) / static_cast<float>(ways - 1);
        return -half + degree * t;
    }
    case YawModifier::Rotate: {
        // туда-обратно по размаху: скорость — сколько сотых хода за тик
        rotatePhase = std::fmod(rotatePhase + static_cast<float>(st.spinSpeed) * 0.01f, 2.0f);
        const float t = rotatePhase < 1.0f ? rotatePhase : 2.0f - rotatePhase;
        return -half + degree * t;
    }
    case YawModifier::Skitter: return static_cast<float>(skitterSequence[phase % std::size(skitterSequence)]) * half;
    default: return 0.0f;
    }
}

// угол на ближайшего по прицелу врага, если врагов нет — текущий угол камеры
bool freestandActive = false;
float freestandSide = 0.0f;     // +1 — голова спрятана влево, -1 — вправо
int freestandFlipTicks = 0;     // сколько тиков подряд фристенд просит другую сторону
int freestandHoldTicks = 0;     // сколько ещё держать прошлую сторону, если фристенду не от чего прятать

float atTargetsYaw(const csgo::UserCmd* cmd, const EngineInterfaces& engineInterfaces, const ClientInterfaces& clientInterfaces, const Memory& memory, csgo::Vector* targetEye = nullptr, bool* found = nullptr, bool includeDormant = false, int* foundIndex = nullptr) noexcept
{
    const auto eye = localPlayer.get().getEyePosition();
    float bestFov = std::numeric_limits<float>::max();
    float bestYaw = cmd->viewangles.y;
    if (found)
        *found = false;

    for (int i = 1; i <= engineInterfaces.getEngine().getMaxClients(); ++i) {
        const auto entity = csgo::Entity::from(retSpoofGadgets->client, clientInterfaces.getEntityList().getEntity(i));
        if (entity.getPOD() == nullptr || entity.getPOD() == localPlayer.get().getPOD() || (!includeDormant && entity.getNetworkable().isDormant())
            || !entity.isAlive() || !entity.isOtherEnemy(memory, localPlayer.get()))
            continue;

        const float yaw = (entity.getAbsOrigin() - eye).toAngle().y;
        const float fov = std::abs(std::remainder(yaw - cmd->viewangles.y, 360.0f));
        if (fov < bestFov) {
            bestFov = fov;
            bestYaw = yaw;
            if (targetEye)
                *targetEye = entity.getEyePosition();
            if (found)
                *found = true;
            if (foundIndex)
                *foundIndex = i;
        }
    }
    return bestYaw;
}

// Фристенд: куда спрятать голову от врага. Возвращает поворот yaw относительно направления на врага:
// +90 — голова влево, -90 — вправо, 0 — прятать не от чего.
// 1) стена вплотную сбоку — голову к ней;
// 2) лучи к врагу из точек слева и справа от головы: где стена ближе — там укрытие. Так ловится и стена
//    перед нами, которая кончается сбоку: луч с открытой стороны уходит мимо её края;
// 3) укрытие с обеих сторон одинаковое (стоим за стеной, край ещё не дошёл) — смотрим, куда едем:
//    сторона движения — та, что выглянет первой, голова уходит в противоположную;
// 4) иначе — какую сторону враг видит хуже (лучи от его глаз).
float freestandYaw(const csgo::EngineTrace& engineTrace, const csgo::Vector& enemyEye, float yawToEnemy) noexcept
{
    const auto eye = localPlayer.get().getEyePosition();
    const auto sidePoint = [&](float offset, float distance) {
        const float radians = Helpers::deg2rad(yawToEnemy + offset);
        return csgo::Vector{ eye.x + std::cos(radians) * distance, eye.y + std::sin(radians) * distance, eye.z };
    };
    const auto traceWalls = [&](const csgo::Vector& from, const csgo::Vector& to) {
        csgo::Trace trace;
        engineTrace.traceRay({ from, to }, 0x1400B, { localPlayer.get().getPOD() }, trace);
        return trace.startSolid ? 0.0f : trace.fraction;
    };

    // 1) стена сбоку от головы — до ~1.5 м (80 ед.; было 45, и стену в метре фристенд не видел).
    // Смотрим чуть вперёд-вбок тоже (±60°): стена, которая начинается сбоку-спереди, укрывает голову так же.
    constexpr float wallReach = 80.0f;
    const auto wallDistance = [&](float side) {
        const float straight = traceWalls(eye, sidePoint(side * 90.0f, wallReach)) * wallReach;
        const float angled = traceWalls(eye, sidePoint(side * 60.0f, wallReach)) * wallReach;
        return std::min(straight, angled);
    };
    const float leftWall = wallDistance(1.0f);
    const float rightWall = wallDistance(-1.0f);
    if (std::min(leftWall, rightWall) < wallReach - 1.0f && std::abs(leftWall - rightWall) > 6.0f)
        return leftWall < rightWall ? 90.0f : -90.0f;

    // 2) лучи к врагу из точек сбоку от головы (не дальше самого врага)
    const float toEnemyRadians = Helpers::deg2rad(yawToEnemy);
    const csgo::Vector forward{ std::cos(toEnemyRadians), std::sin(toEnemyRadians), 0.0f };
    const float reach = std::clamp((enemyEye - eye).length2D(), 64.0f, 400.0f);
    const auto coverDistance = [&](float offset) {
        float sum = 0.0f;
        for (const float distance : { 20.0f, 32.0f }) {
            const auto from = sidePoint(offset, distance);
            // точка сбоку уже в стене — укрытие вплотную
            if (traceWalls(eye, from) < 0.99f)
                continue;
            sum += traceWalls(from, from + forward * reach) * reach;
        }
        return sum * 0.5f;
    };
    const float leftCover = coverDistance(90.0f);
    const float rightCover = coverDistance(-90.0f);
    if (std::abs(leftCover - rightCover) > 24.0f)
        return leftCover < rightCover ? 90.0f : -90.0f;

    // 3) укрытие одинаковое — прячем голову от стороны, куда едем
    const auto velocity = localPlayer.get().velocity();
    const float leftSpeed = -velocity.x * forward.y + velocity.y * forward.x;   // скорость влево относительно врага
    const bool covered = leftCover < reach * 0.9f && rightCover < reach * 0.9f;
    if (std::abs(leftSpeed) > 30.0f && (covered || std::abs(leftSpeed) > 120.0f))
        return leftSpeed > 0.0f ? -90.0f : 90.0f;

    // 4) какую сторону головы враг видит хуже (луч от его глаз, только стены)
    const auto openness = [&](float offset) {
        float sum = 0.0f;
        for (const float distance : { 15.0f, 30.0f, 45.0f })
            sum += traceWalls(enemyEye, sidePoint(offset, distance));
        return sum / 3.0f;
    };
    const float left = openness(90.0f);
    const float right = openness(-90.0f);
    if (std::abs(left - right) < 0.05f)
        return 0.0f;
    return left < right ? 90.0f : -90.0f;
}


// ---- дефенсив AA ----

// Как в hysteria: своё состояние берётся, только если у него не «Как общее»; «Выкл» — выключает
SnapState selectSnapState(const csgo::UserCmd* cmd, bool haveFreestandRaw) noexcept
{
    const auto& snaps = config.antiAim.snaps;
    const auto custom = [&snaps](SnapState s) { return snaps[static_cast<int>(s)].mode != static_cast<int>(SnapMode::Default); };
    const auto& player = localPlayer.get();
    const int flags = player.flags();
    const bool jumping = !(flags & FL_ONGROUND) || (cmd->buttons & csgo::UserCmd::IN_JUMP);
    const bool ducking = (cmd->buttons & csgo::UserCmd::IN_DUCK) || (flags & FL_DUCKING);
    const bool moving = player.velocity().length2D() > 5.0f;
    if (custom(SnapState::AirCrouch) && jumping && ducking)
        return SnapState::AirCrouch;
    if (custom(SnapState::Air) && jumping)
        return SnapState::Air;
    if (custom(SnapState::EdgeBait) && haveFreestandRaw && !jumping)
        return SnapState::EdgeBait;
    if (custom(SnapState::Sneaking) && !jumping && ducking && moving)
        return SnapState::Sneaking;
    if (custom(SnapState::Crouching) && !jumping && ducking)
        return SnapState::Crouching;
    if (custom(SnapState::Walking) && !jumping && moving && (cmd->buttons & csgo::UserCmd::IN_SPEED))
        return SnapState::Walking;
    return SnapState::Default;
}

const DefensiveSnap* activeSnap(SnapState state) noexcept
{
    const auto& snaps = config.antiAim.snaps;
    if (state != SnapState::Default) {
        const auto& own = snaps[static_cast<int>(state)];
        if (own.mode == static_cast<int>(SnapMode::Off))
            return nullptr;
        if (own.mode == static_cast<int>(SnapMode::Custom))
            return &own;
    }
    const auto& def = snaps[static_cast<int>(SnapState::Default)];
    return def.mode == static_cast<int>(SnapMode::Custom) ? &def : nullptr;
}

[[nodiscard]] float fraction(float value) noexcept
{
    return value - std::floor(value);
}

[[nodiscard]] float lerp(float a, float b, float t) noexcept
{
    return a + (b - a) * t;
}

// Прогресс окна для «Spin Full»: 1 на входе в окно, 0 на его конце
[[nodiscard]] float windowProgress(const DefensiveSnap& s, int left) noexcept
{
    const int length = std::min(snap.apex, std::clamp(s.duration, 1, 13));
    if (length <= 1)
        return 0.0f;
    const int base = snap.apex - (length - 1);
    return std::clamp(static_cast<float>(left - base) / static_cast<float>(length - 1), 0.0f, 1.0f);
}

std::optional<float> snapPitch(const DefensiveSnap& s, float curtime, int left, float cameraPitch, float targetPitch) noexcept
{
    const float a = static_cast<float>(s.pitchAngle);
    const float b = static_cast<float>(s.pitchAngle2);
    switch (static_cast<SnapPitch>(s.pitch)) {
    case SnapPitch::Static: return a;
    case SnapPitch::Jitter: return snap.jitter ? a : b;
    case SnapPitch::Random: return std::uniform_real_distribution<float>{ std::min(a, b), std::max(a, b) }(rng);
    case SnapPitch::RandomStatic:
        if (!snap.haveRandomPitch) {
            snap.randomPitch = std::uniform_real_distribution<float>{ std::min(a, b), std::max(a, b) }(rng);
            snap.haveRandomPitch = true;
        }
        return snap.randomPitch;
    case SnapPitch::Spin: return lerp(a, b, fraction(curtime * static_cast<float>(s.pitchSpeed) * 0.1f));
    case SnapPitch::SpinFull: return lerp(a, b, windowProgress(s, left));
    case SnapPitch::Camera: return cameraPitch;
    case SnapPitch::AtTarget: return targetPitch;
    default: return std::nullopt;
    }
}

// yaw относительно направления на врага: 0 — лицом к нему, 180 — спиной (обычный AA)
std::optional<float> snapYaw(const DefensiveSnap& s, float curtime, int left, std::optional<float> freestandRaw) noexcept
{
    const float angle = static_cast<float>(s.yawAngle);
    const float half = angle * 0.5f;
    const float speed = static_cast<float>(s.yawSpeed);
    switch (static_cast<SnapYaw>(s.yaw)) {
    case SnapYaw::Static: return 360.0f - angle;
    case SnapYaw::Jitter: return 180.0f + (snap.jitter ? half : -half);
    case SnapYaw::Random: return 180.0f + std::uniform_real_distribution<float>{ -half, half }(rng);
    case SnapYaw::RandomJitter:
        return (std::uniform_int_distribution<int>{ 0, 1 }(rng) ? 90.0f : -90.0f) + std::uniform_real_distribution<float>{ -angle * 0.25f, angle * 0.25f }(rng);
    case SnapYaw::RandomStatic:
        if (!snap.haveRandomYaw) {
            snap.randomYaw = std::uniform_real_distribution<float>{ -half, half }(rng);
            snap.haveRandomYaw = true;
        }
        return 180.0f + snap.randomYaw;
    case SnapYaw::Spin: return 180.0f + lerp(-half, half, fraction(curtime * speed * 0.1f));
    case SnapYaw::SpinFull: return 180.0f + lerp(-half, half, windowProgress(s, left));
    case SnapYaw::SpinJitter: return (snap.jitter ? 90.0f : -90.0f) + lerp(-half, half, fraction(curtime * speed * 0.1f));
    case SnapYaw::Way90: {
        const float t = fraction(static_cast<float>(left) / static_cast<float>(std::clamp(s.duration, 1, 13)) * speed * 0.05f);
        return (snap.counter % 2 == 0 ? 90.0f : -90.0f) + lerp(-half, half, t) - 90.0f;
    }
    case SnapYaw::Sway180: return 180.0f + lerp(-half, half, std::sin(curtime * speed * 0.2f) * 0.5f + 0.5f);
    case SnapYaw::Opposite:
        // приманка у стены: на скрытых тиках голова — с открытой стороны (обычный фристенд её прячет)
        if (!freestandRaw)
            return std::nullopt;
        return freestandActive ? 180.0f : -*freestandRaw;
    default: return std::nullopt;
    }
}

// Edge yaw (как в hysteria): ближайшая стена в пределах ~32 ед. от глаз — лицом в неё, голова в стене
std::optional<float> edgeYaw(const csgo::EngineTrace& engineTrace) noexcept
{
    const auto eye = localPlayer.get().getEyePosition();
    const auto trace = [&](float yaw) {
        const float radians = Helpers::deg2rad(yaw);
        const csgo::Vector end{ eye.x + std::cos(radians) * 32.0f, eye.y + std::sin(radians) * 32.0f, eye.z };
        csgo::Trace result;
        engineTrace.traceRay({ eye, end }, 0x1400B, { localPlayer.get().getPOD() }, result);
        return result.startSolid ? 1.0f : result.fraction;
    };
    float best = 1.0f;
    std::optional<float> bestYaw;
    for (float yaw = -180.0f; yaw < 180.0f; yaw += 45.0f) {
        if (const float f = trace(yaw); f < best) {
            best = f;
            bestYaw = yaw;
        }
    }
    if (!bestYaw)
        return std::nullopt;
    const float coarse = *bestYaw;
    for (float yaw = coarse - 30.0f; yaw <= coarse + 30.0f; yaw += 10.0f) {
        if (const float f = trace(yaw); f < best) {
            best = f;
            bestYaw = yaw;
        }
    }
    return std::remainder(*bestYaw, 360.0f);
}

// Safe head (как в hysteria). Обычный: прыжок с ножом и враг не сильно ниже — голова прямо, без десинка.
// Умный: смотрим, под каким углом враг видит голову; в «опасной» полосе (своей для стойки) десинк выключаем,
// чтобы голова не уезжала из-за тела вбок, где её проще задеть.
bool safeHeadActive(const csgo::UserCmd* cmd, bool haveThreat, const csgo::Vector& threatOrigin) noexcept
{
    const auto mode = static_cast<SafeHead>(config.antiAim.safeHead);
    if (mode == SafeHead::Off || !haveThreat)
        return false;
    const auto& player = localPlayer.get();
    const int flags = player.flags();
    const bool onGround = (flags & FL_ONGROUND) && !(cmd->buttons & csgo::UserCmd::IN_JUMP);
    const bool crouching = (cmd->buttons & csgo::UserCmd::IN_DUCK) || (flags & FL_DUCKING);
    const auto weapon = csgo::Entity::from(retSpoofGadgets->client, player.getActiveWeapon());
    const bool knife = weapon.getPOD() && weapon.getWeaponType() == WeaponType::Knife;

    if (mode == SafeHead::Basic)
        return !onGround && knife && player.origin().z - threatOrigin.z > -32.0f;

    const auto head = player.getEyePosition();
    const float threatEyeZ = threatOrigin.z + 64.0f;
    const float distance = std::max((player.origin() - threatOrigin).length(), 1.0f);
    const float ratio = (head.z - threatEyeZ) / distance;
    float low = 0.0f, high = 0.75f;
    if (onGround && !crouching) {
        low = 0.25f;
        high = 0.5f;
    } else if (onGround && crouching) {
        low = -0.05f;
        high = 0.3f;
    } else if (!crouching) {
        low = 0.15f;
        high = 0.75f;
    } else if (knife) {
        low = -0.05f;
        high = 0.55f;
    } else {
        low = 0.1f;
        high = 0.75f;
    }
    return ratio >= low && ratio <= high;
}

// Легит AA: можно ли забрать E себе. Дефьюз и бомбу под ногами у КТ, смотрящего вниз, не трогаем
bool legitAllowed(const csgo::UserCmd* cmd, const ClientInterfaces& clientInterfaces) noexcept
{
    const auto& player = localPlayer.get();
    if (Netvars::get(fnv::hash("CCSPlayer->m_bIsDefusing")) && player.isDefusing())
        return false;
    if (player.getTeamNumber() == csgo::Team::CT && cmd->viewangles.x > 15.0f) {
        const auto list = clientInterfaces.getEntityList();
        const int highest = list.getHighestEntityIndex();
        for (int i = 1; i <= highest; ++i) {
            const auto entity = csgo::Entity::from(retSpoofGadgets->client, list.getEntity(i));
            if (!entity.getPOD() || entity.getNetworkable().isDormant())
                continue;
            const auto clientClass = entity.getNetworkable().getClientClass();
            if (clientClass && clientClass->classId == ClassId::PlantedC4 && (entity.getAbsOrigin() - player.origin()).length() < 61.0f)
                return false;
        }
    }
    return true;
}

// ======== план → поза → применение ========
//
// Тик антиаима проходит три стадии:
//   1) план — кто владеет углами (билдер, фристенд, стена, ручной, safe head, наклон,
//      extended yaw, легит), какой у него базовый yaw и какие права остаются билдеру;
//   2) поза — pitch, «фейковый» yaw (его видят враги и по нему сервер ставит голову) и
//      «настоящий» yaw (за ним сервер тянет ноги);
//   3) применение — какой из двух углов уходит в эту команду, микродвижение под десинк, отчёт
//      индикаторам.
//
// Раньше стадий не было. Права билдера описывались тремя флагами (noOffset/noModifier/noSnap)
// и forceDesync, которые переписывались в восьми местах по ходу run(): побеждал тот режим, что
// стоял ниже в тексте, приоритет нигде не был записан, и каждый новый режим добавлял ещё одну
// запись флага. Пара углов тоже не была выражена: в команду писался один yaw, а «настоящий»
// получался тем, что на чокнутом тике к нему дописывалась добавка — поэтому модификатор, спин
// и натуральность, шагающие каждый тик, успевали развести чокнутую и отправленную команды, и
// увод тела выходил не тот, что задан ползунком.

// Владельцы по возрастанию силы: кто ниже, тот и забирает тик.
enum class Owner { Builder, Freestand, Edge, Manual, SafeHead, Roll, Extended, Legit };

struct Plan {
    Owner owner = Owner::Builder;
    float yaw = 0.0f;                   // базовый угол, к которому билдер добавляет своё
    bool staticAngles = false;          // билдеру тик не отдаём: ни смещения, ни модификатора
    bool allowSnap = true;              // можно ли отдать тик скрытым углам дефенсива
    std::optional<int> forcedDesync;    // 0 — без десинка, ±1 — полный в эту сторону
    float roll = 0.0f;
    std::optional<float> freestandRaw;  // сырое направление укрытия (нужно дефенсиву «у стены»)
};

// Всё, что стадиям нужно знать о тике: посчитали один раз, дальше только читаем.
struct Frame {
    csgo::UserCmd* cmd = nullptr;
    const EngineInterfaces* engine = nullptr;
    const ClientInterfaces* clients = nullptr;
    const Memory* memory = nullptr;
    csgo::Vector camera{};          // взгляд камеры до подмены
    csgo::Vector eye{};
    float realtime = 0.0f;
    float curtime = 0.0f;
    bool sendPacket = false;        // эта команда уйдёт на сервер (иначе её чокнут)
    bool cycleStart = false;        // с этого тика начинается новый цикл чока
    bool legit = false;
    bool haveThreat = false;
    float toTarget = 0.0f;          // yaw на ближайшего по прицелу врага
    float targetPitch = 0.0f;
    csgo::Vector threatOrigin{};
    AntiAimState st{};              // настройки билдера этого цикла
    int inverterSide = 1;
    bool defensiveOn = false;
    float maxDesync = 58.0f;
};

// Поза, которая уходит на сервер в этом тике.
struct Pose {
    float pitch = 0.0f;
    float fakeYaw = 0.0f;   // отправленная команда
    float realYaw = 0.0f;   // чокнутые команды
    float roll = 0.0f;
    int side = 0;           // сторона увода тела (+1 влево, −1 вправо; 0 — без десинка)
    float desync = 0.0f;    // увод тела в градусах, уже обрезанный по максимуму стойки
};

// Владелец сильнее — забирает план себе. Права только ужесточаются: статику уже не снять,
// запрет скрытых углов не отменить, а сторону десинка задаёт сильнейший из тех, кто её требует.
void claim(Plan& plan, Owner owner, bool staticAngles, bool allowSnap, std::optional<int> desync) noexcept
{
    if (static_cast<int>(owner) >= static_cast<int>(plan.owner))
        plan.owner = owner;
    plan.staticAngles = plan.staticAngles || staticAngles;
    plan.allowSnap = plan.allowSnap && allowSnap;
    if (desync)
        plan.forcedDesync = desync;
}

// Максимальный увод тела для нашей стойки — его же считает сервер (CCSGOPlayerAnimState),
// дальше он тело не пустит. Присед и скорость его режут, поэтому «58» в воздухе значит меньше.
[[nodiscard]] float localMaxDesync() noexcept
{
    const float value = localPlayer.get().getMaxDesyncAngle();
    return value > 1.0f ? std::clamp(value, 0.0f, 60.0f) : 58.0f;
}

// Чокнутую команду уводим ЗА цель с запасом: сервер доводит ноги не мгновенно, и ровно заданный
// угол давал бы недовод. Запас фиксированный — до переделки тут стоял множитель ×2, из-за
// которого всё от ~29° и выше упиралось в максимум стойки, и половина шкалы ничего не значила.
constexpr float desyncMargin = 12.0f;

// ---- стадия 1: план ----
[[nodiscard]] Plan buildPlan(const Frame& f) noexcept
{
    const auto& aa = config.antiAim;
    Plan plan;
    plan.yaw = f.camera.y;
    freestandActive = false;

    if (f.legit) {
        // E зажато (дверь, бомба, подбор): углы настоящие, но тело уводим по-прежнему —
        // десинк мешает только прицелу, а прицел тут и не нужен
        freestandSide = 0.0f;
        freestandHoldTicks = 0;
        claim(plan, Owner::Legit, true, false, std::nullopt);
        return plan;
    }

    switch (static_cast<YawBase>(aa.yawBase)) {
    case YawBase::Backward: plan.yaw += 180.0f; break;
    case YawBase::AtTargets: plan.yaw = f.toTarget + 180.0f; break;
    default: break;
    }

    // ---- фристенд (сырое направление нужно ещё и дефенсиву «у стены») ----
    const bool freestandHeld = aa.freestandKey.isSet() && aa.freestandKey.isDown();
    const bool wantFreestand = aa.freestanding && (freestandHeld || (!aa.freestandKey.isSet() && manual == Manual::None));
    const bool wantEdgeBait = f.defensiveOn && aa.snaps[static_cast<int>(SnapState::EdgeBait)].mode != static_cast<int>(SnapMode::Default);
    bool freestandOverridesManual = false;
    if (wantFreestand || wantEdgeBait) {
        csgo::Vector enemyEye;
        bool found = false;
        float toEnemy = atTargetsYaw(f.cmd, *f.engine, *f.clients, *f.memory, &enemyEye, &found, true);
        // врагов не видно — считаем, что они там, куда смотрит камера (стену сбоку это не меняет)
        if (!found) {
            toEnemy = f.camera.y;
            const float radians = Helpers::deg2rad(toEnemy);
            enemyEye = { f.eye.x + std::cos(radians) * 2000.0f, f.eye.y + std::sin(radians) * 2000.0f, f.eye.z };
        }
        float offset = freestandYaw(f.engine->engineTrace(), enemyEye, toEnemy);
        if (offset != 0.0f)
            plan.freestandRaw = offset;
        if (wantFreestand) {
            if (offset != 0.0f)
                freestandHoldTicks = 8;
            // Гистерезис: новую сторону берём, только если она держится 3 тика подряд;
            // пропало, от чего прятать, — ещё 8 тиков держим прошлую
            if (offset != 0.0f && freestandSide != 0.0f && (offset > 0.0f) != (freestandSide > 0.0f)) {
                if (++freestandFlipTicks < 3)
                    offset = freestandSide * 90.0f;
                else
                    freestandFlipTicks = 0;
            } else {
                freestandFlipTicks = 0;
            }
            if (offset == 0.0f && freestandSide != 0.0f && freestandHoldTicks > 0) {
                --freestandHoldTicks;
                offset = freestandSide * 90.0f;
            }
            if (offset != 0.0f) {
                plan.yaw = toEnemy + offset;
                freestandActive = true;
                freestandSide = offset > 0.0f ? 1.0f : -1.0f;
                freestandOverridesManual = freestandHeld;
                // сторону тела фристенд задаёт сам (ниже, в десинке): тело уходит туда же, куда голова
                claim(plan, Owner::Freestand, aa.freestandStatic, true, std::nullopt);
            } else {
                freestandSide = 0.0f;
            }
        }
    }
    if (!wantFreestand) {
        freestandSide = 0.0f;
        freestandHoldTicks = 0;
    }

    // ---- edge yaw: стена вплотную — лицом в неё ----
    if (aa.edgeYaw && (!aa.edgeKey.isSet() || aa.edgeKey.isDown()) && manual == Manual::None && !freestandActive) {
        if (const auto edge = edgeYaw(f.engine->engineTrace())) {
            plan.yaw = *edge;
            claim(plan, Owner::Edge, aa.edgeStatic, true,
                aa.edgeStatic ? std::optional<int>{ f.inverterSide } : std::nullopt);
        }
    }

    // ---- ручной AA ----
    {
        const auto direction = freestandOverridesManual ? Manual::None : manual;
        switch (direction) {
        case Manual::Left: plan.yaw = f.camera.y + 90.0f; break;
        case Manual::Right: plan.yaw = f.camera.y - 90.0f; break;
        case Manual::Back: plan.yaw = f.camera.y + 180.0f; break;
        case Manual::Forward: plan.yaw = f.camera.y; break;
        default: break;
        }
        if (direction != Manual::None)
            claim(plan, Owner::Manual, aa.staticOnManual, true,
                aa.staticOnManual ? std::optional<int>{ f.inverterSide } : std::nullopt);
    }

    // ---- safe head: голова в «опасной» полосе — тело не уводим, иначе её проще задеть ----
    if (manual == Manual::None && safeHeadActive(f.cmd, f.haveThreat, f.threatOrigin))
        claim(plan, Owner::SafeHead, true, true, 0);

    // ---- наклон ----
    if (aa.roll != 0) {
        float roll = static_cast<float>(std::clamp(aa.roll, -50, 50)) * (aa.inverter.isToggled() ? -1.0f : 1.0f);
        const bool onGround = localPlayer.get().flags() & FL_ONGROUND;
        if (localPlayer.get().velocity().length2D() > 5.0f || !onGround)
            roll = std::clamp(roll, -40.0f, 40.0f);
        plan.roll = roll;
        claim(plan, Owner::Roll, true, false, roll > 0.0f ? 1 : -1);
    }

    // ---- extended yaw: пока он работает, углы стоят на месте ----
    // LBY сервер ставит по углу чокнутой команды, а билдер между чокнутой и отправленной успел бы
    // увести yaw модификатором — и тело встало бы не туда. В логе 07.11 при заданных 92° разница
    // «взгляд ↔ LBY» гуляла от +34 до +180; статичный угол даёт ровно заданный.
    const bool standing = (localPlayer.get().flags() & FL_ONGROUND)
        && localPlayer.get().velocity().length2D() <= 2.0f
        && std::abs(f.cmd->forwardmove) < 1.0f && std::abs(f.cmd->sidemove) < 1.0f
        && !(f.cmd->buttons & csgo::UserCmd::IN_JUMP);
    if (aa.extendedYaw > 0 && aa.desync && tickbase::isInstalled() && standing)
        claim(plan, Owner::Extended, true, false, f.inverterSide);

    return plan;
}

// ---- стадия 2: pitch ----
[[nodiscard]] float buildPitch(const Frame& f, const Plan& plan) noexcept
{
    if (plan.owner == Owner::Legit)
        return f.camera.x;
    switch (static_cast<Pitch>(config.antiAim.pitch)) {
    case Pitch::Down: return 89.0f;
    case Pitch::Up: return -89.0f;
    case Pitch::Zero: return 0.0f;
    default: return f.camera.x;
    }
}

// ---- стадия 2: скрытые углы на тиках окна дефенсива ----
// Когда окно открыто, решает Defensive.cpp — тут только углы, как в NL: ядро даёт `hidden`,
// скрипт отдаёт override_hidden_pitch/yaw. Возвращает true, если скрытый угол забрал yaw:
// тогда билдеру тик не отдаём.
[[nodiscard]] bool applyDefensiveSnap(const Frame& f, const Plan& plan, Pose& pose) noexcept
{
    if (!f.defensiveOn) {
        // дефенсив выключили (или заряд ушёл под быструю стрельбу DT) — следующее окно с чистого листа
        snap.ticks = 0;
        snap.apex = 0;
        snap.delayed = 0;
        snap.haveRandomPitch = snap.haveRandomYaw = false;
        return false;
    }

    const int lcLeft = defensive::windowLeft();
    // Новое окно — новый отсчёт «длительности». Без этого счётчик тиков жил через границу окон:
    // когда окна идут вплотную (а с дефенсивом «чоком» это норма), он перешагивал «Длительность»
    // и скрытые углы отключались до первого тика без окна. Со стороны — «иногда не работает».
    if (const int windowId = defensive::windowCounter(); windowId != snap.seenCounter) {
        snap.seenCounter = windowId;
        snap.ticks = 0;
        snap.apex = 0;
        snap.delayed = 0;
        snap.haveRandomPitch = snap.haveRandomYaw = false;
    }
    const DefensiveSnap* chosen = activeSnap(selectSnapState(f.cmd, plan.freestandRaw.has_value()));
    bool inWindow = chosen && lcLeft > 0;
    if (inWindow) {
        ++snap.ticks;
        // «Длительность» — сколько тиков окна занимать. Остаток окна идёт обычным AA: запись,
        // которую сервер положит, когда окно закроется, должна быть защищённой (десинк), а не
        // скрытым углом
        inWindow = snap.ticks <= std::clamp(chosen->duration, 1, 13);
    } else {
        snap.ticks = 0;
    }

    if (!inWindow || !plan.allowSnap) {
        if (lcLeft == 0 && snap.apex != 0) {
            // окно закончилось — следующее начнётся с чистого листа
            snap.counter = defensive::windowCounter();
            snap.apex = 0;
            snap.delayed = 0;
            snap.haveRandomPitch = snap.haveRandomYaw = false;
        }
        return false;
    }

    if (!snap.active && snap.apex == 0)
        snap.apex = defensive::windowLength();
    snap.active = true;
    if (snap.delayed >= chosen->yawDelay + 1) {
        snap.jitter = !snap.jitter;
        snap.delayed = 0;
    } else if (f.sendPacket) {
        ++snap.delayed;
    }
    if (const auto pitch = snapPitch(*chosen, f.curtime, lcLeft, f.camera.x, f.targetPitch))
        pose.pitch = std::clamp(*pitch, -89.0f, 89.0f);
    if (const auto offset = snapYaw(*chosen, f.curtime, lcLeft, plan.freestandRaw)) {
        pose.fakeYaw = (f.haveThreat ? f.toTarget : f.camera.y) + *offset;
        return true;
    }
    return false;
}

// ---- стадия 2: флики ----
[[nodiscard]] bool applyFlicks(const Frame& f, Pose& pose) noexcept
{
    const auto& aa = config.antiAim;
    bool flick = false;
    if (aa.flicks && !f.legit) {
        if (tickbase::isShifting())
            flickAfterShift = 2;
        flick = tickbase::isShifting() || flickAfterShift > 0;
        if (!flick && aa.flickTrigger == static_cast<int>(FlickTrigger::Always) && f.sendPacket && ++flickCounter >= std::max(aa.flickEvery, 3)) {
            flickCounter = 0;
            flick = true;
        }
        if (!tickbase::isShifting() && f.sendPacket && flickAfterShift > 0)
            --flickAfterShift;
        // скрытые углы дефенсива главнее
        if (snap.active)
            flick = false;
    }
    if (flick) {
        if (!wasFlicking) {
            flickDirection = aa.flickSide == static_cast<int>(FlickSide::Random)
                ? (std::uniform_int_distribution<int>{ 0, 1 }(rng) ? 1.0f : -1.0f) : -flickDirection;
        }
        pose.fakeYaw += aa.flickSide == static_cast<int>(FlickSide::Custom)
            ? static_cast<float>(std::clamp(aa.flickYaw, -180, 180))
            : flickDirection * static_cast<float>(aa.flickAngle);
        if (aa.flickPitchOverride)
            pose.pitch = static_cast<float>(std::clamp(aa.flickPitch, -89, 89));
    }
    wasFlicking = flick;
    return flick;
}

// ---- стадия 2: сторона увода тела ----
// Сторону выбираем один раз на цикл чока: чокнутая и отправленная команды обязаны считать её
// одинаково, иначе пара углов разъезжается. До переделки сторона считалась каждый тик, и
// нажатие инвертера или переворот фристенда посреди цикла ломали десинк на тик.
[[nodiscard]] int pickSide(const Frame& f, const Plan& plan) noexcept
{
    if (!f.cycleStart && pinnedValid)
        return pinnedSide;

    const auto& aa = config.antiAim;
    const auto& st = f.st;
    int result = f.inverterSide;
    if (st.desyncInvert)
        result = -result;
    if (aa.antiBrute && antiBruteFlip)
        result = -result;
    if (plan.forcedDesync && *plan.forcedDesync != 0) {
        result = *plan.forcedDesync;
    } else if (!plan.staticAngles) {
        const auto mode = static_cast<DesyncMode>(st.desyncMode);
        if (mode == DesyncMode::Jitter) {
            // скиттер ведёт сторону своим порядком (минус — другая сторона), остальные — через раз
            if (static_cast<YawModifier>(st.modifier) == YawModifier::Skitter) {
                if (skitterSequence[phase % std::size(skitterSequence)] < 0)
                    result = -result;
            } else if (phase & 1) {
                result = -result;
            }
        } else if (mode == DesyncMode::Random) {
            result *= randomSide;
        }
    }
    // контр-резольвер: сторона, куда следующий выстрел этого врага скорее всего не придёт
    if (counterPlan.active && !plan.forcedDesync && !plan.staticAngles)
        result = counterPlan.side;
    // при фристенде тело уводим туда же, куда голову: от врага за укрытие
    if (freestandActive)
        result = freestandSide > 0.0f ? 1 : -1;

    pinnedSide = result;
    pinnedValid = true;
    return result;
}

// ---- стадия 2: extended yaw ----
// Стоя ноги тянутся к m_flLowerBodyYawTarget, а LBY сервер ставит либо раз в ~1.1 с, либо
// КАЖДУЮ команду, в которой мы двигаемся. Берём второй случай, и времени в нём нет вовсе:
//   * чокнутая команда несёт yaw, уведённый на extendedYaw в сторону десинка;
//   * на ней же идёт микродвижение — сервер считает нас идущими и тянет LBY за этим углом;
//   * отправленная команда несёт настоящий взгляд: его и видят враги.
// Угол взят за кламп билдера (до 180°), сервер сам упрёт тело в свой предел.
void reportExtended(const Frame& f, Pose& pose, float extendedAngle) noexcept
{
    // «Модель сервера» рисуется по уводу тела. С extended yaw тело тянется к настоящему LBY,
    // а не к ползунку десинка, — берём разницу прямо с сервера, её и покажем
    const float netLby = localPlayer.get().lby();
    if (f.sendPacket)
        pose.desync = std::remainder(netLby - pose.fakeYaw, 360.0f);

    // Раз в секунду в лог: ушёл ли LBY за уведённым углом. Держится около заданного (или упёрся
    // в ~58 — дальше сервер тело не пускает) — работает; около нуля — сервер считает нас стоящими
    if (f.sendPacket) {
        static float lastLog = -10.0f;
        if (f.realtime - lastLog > 1.0f || f.realtime < lastLog) {
            lastLog = f.realtime;
            char text[160];
            std::snprintf(text, sizeof(text), "[extended yaw] взгляд %.0f, LBY с сервера %.0f, разница %+.0f (задано %.0f, сторона %+d)",
                pose.fakeYaw, netLby, std::remainder(netLby - pose.fakeYaw, 360.0f), extendedAngle, pose.side);
            ragebot::logToFile(text);
        }
    }

    std::scoped_lock lock{ extendedMutex };
    extendedShown.active = true;
    const auto origin = localPlayer.get().getAbsOrigin();
    extendedShown.origin[0] = origin.x;
    extendedShown.origin[1] = origin.y;
    extendedShown.origin[2] = origin.z;
    extendedShown.lby = netLby;
    if (f.sendPacket)
        extendedShown.eyeYaw = pose.fakeYaw;
    extendedShown.flickYaw = extendedShown.eyeYaw + pose.side * extendedAngle;
}

// ---- стадия 2: десинк, пара углов ----
void buildDesync(const Frame& f, const Plan& plan, Pose& pose, bool flick) noexcept
{
    const auto& aa = config.antiAim;
    const auto& st = f.st;
    const auto mode = static_cast<DesyncMode>(st.desyncMode);
    const bool forcedOff = plan.forcedDesync && *plan.forcedDesync == 0;
    const bool forcedOn = plan.forcedDesync && *plan.forcedDesync != 0;
    const bool enabled = aa.desync && tickbase::isInstalled() && !forcedOff && (mode != DesyncMode::Off || forcedOn);
    if (!enabled) {
        pose.side = 0;
        pose.desync = 0.0f;
        pose.realYaw = pose.fakeYaw;
        pinnedValid = false;
        return;
    }

    pose.side = pickSide(f, plan);

    // Величина: ползунок стороны, обрезанный по максимуму стойки. Статика (ручной, стена, наклон,
    // extended yaw) уводит тело на полный максимум.
    const int limit = forcedOn ? 60 : (pose.side > 0 ? st.desyncLeft : st.desyncRight);
    float target = std::min(static_cast<float>(std::clamp(limit, 0, 60)), f.maxDesync);
    // контр-резольвер против «центра»/«следит» может выбрать low delta
    if (counterPlan.active && !forcedOn && !plan.staticAngles)
        target *= counterPlan.magnitude;
    pose.desync = static_cast<float>(pose.side) * target;

    // добавка к yaw по стороне: голова идёт за десинком, а не стоит посередине
    if (!plan.staticAngles)
        pose.fakeYaw += static_cast<float>(pose.side > 0 ? st.addLeft : st.addRight);

    // Пара углов. Чокнутые команды несут угол, уведённый за цель; отправленная — настоящий взгляд.
    // На флике увод не трогаем: сам флик и так разносит углы.
    const float extendedAngle = static_cast<float>(std::clamp(aa.extendedYaw, 0, 180));
    const float chokedOffset = plan.owner == Owner::Extended ? extendedAngle : target + desyncMargin;
    pose.realYaw = flick ? pose.fakeYaw : pose.fakeYaw + static_cast<float>(pose.side) * chokedOffset;

    if (plan.owner == Owner::Extended)
        reportExtended(f, pose, extendedAngle);
}

// ---- стадия 3: микродвижение под десинк ----
// Сервер решает, обновлять ли LBY, по нашей скорости (порог > 0.1 ед/с):
//  * с extended yaw движение нужно ровно на чокнутой команде — той, что несёт уведённый угол:
//    сервер видит скорость, ставит LBY на этот угол, и дальше ноги тянутся к нему (100°/с).
//    Отправленную команду оставляем без движения: трение за тик гасит скорость в ноль, сервер
//    идёт «стоячей» веткой и LBY не трогает — иначе он тут же уехал бы на настоящий взгляд.
//    Установившаяся скорость = sv_accelerate × forwardmove × интервал ≈ 0.086 × forwardmove,
//    поэтому значения тут не «доли max_speed», как в lua, а единицы в секунду; присед режет
//    wishspeed на 0.34 — там нужно больше.
//  * без extended yaw, стоя, движение нужно наоборот крошечное, чтобы LBY не обновлялся вовсе.
void applyBodyMovement(const Frame& f, bool extended) noexcept
{
    const auto& player = localPlayer.get();
    const int flags = player.flags();
    if (extended) {
        if (!f.sendPacket) {
            const float micro = (flags & FL_DUCKING) ? 12.0f : 5.0f;
            // знак через тик: иначе за минуту стояния набегает заметное сползание
            f.cmd->forwardmove = (f.cmd->commandNumber & 1) ? micro : -micro;
            f.cmd->sidemove = 0.0f;
        } else {
            f.cmd->forwardmove = 0.0f;
            f.cmd->sidemove = 0.0f;
        }
        return;
    }
    const bool still = (flags & FL_ONGROUND) && player.velocity().length2D() < 3.0f
        && std::abs(f.cmd->forwardmove) < 1.0f && std::abs(f.cmd->sidemove) < 1.0f
        && !(f.cmd->buttons & csgo::UserCmd::IN_JUMP);
    if (still) {
        const float micro = (flags & FL_DUCKING) ? 3.25f : 1.1f;
        f.cmd->sidemove = (f.cmd->commandNumber & 1) ? micro : -micro;
    }
}

}

void updateInput() noexcept
{
    auto& aa = config.antiAim;
    aa.inverter.handleToggle();
    toggleManual(aa.manualLeft, Manual::Left);
    toggleManual(aa.manualRight, Manual::Right);
    toggleManual(aa.manualBack, Manual::Back);
    toggleManual(aa.manualForward, Manual::Forward);
}

bool isFreestanding() noexcept
{
    return freestandActive;
}

const char* stateName() noexcept
{
    const int index = static_cast<int>(activeState);
    return index >= 0 && index < static_cast<int>(aaStateNames.size()) ? aaStateNames[index] : "";
}

bool defensiveActive() noexcept
{
    return snap.active;
}

float rollAngle() noexcept
{
    return currentRoll;
}

void fixRollMovement(csgo::UserCmd* cmd) noexcept
{
    if (currentRoll == 0.0f)
        return;
    // Сервер считает движение по forward/right из углов команды, обнулив z и нормировав.
    // С наклоном (roll) при опущенном pitch вектор «вправо» поворачивается — решаем, какие forward/side
    // дадут то же движение, что без наклона (как movement_fix в hysteria).
    const float yaw = Helpers::deg2rad(cmd->viewangles.y);
    const float pitch = Helpers::deg2rad(cmd->viewangles.x);
    const float roll = Helpers::deg2rad(currentRoll);
    const float cy = std::cos(yaw), sy = std::sin(yaw), sp = std::sin(pitch), cp = std::cos(pitch), sr = std::sin(roll), cr = std::cos(roll);
    // желаемое движение — в базисе без наклона
    const float wantX = cmd->forwardmove * cy + cmd->sidemove * sy;
    const float wantY = cmd->forwardmove * sy - cmd->sidemove * cy;
    float fx = cp * cy, fy = cp * sy;
    float rx = -sr * sp * cy + cr * sy, ry = -sr * sp * sy - cr * cy;
    const float fl = std::hypot(fx, fy), rl = std::hypot(rx, ry);
    if (fl < 1e-4f || rl < 1e-4f)
        return;
    fx /= fl; fy /= fl; rx /= rl; ry /= rl;
    const float det = fx * ry - fy * rx;
    if (std::abs(det) < 1e-4f)
        return;
    cmd->forwardmove = (wantX * ry - wantY * rx) / det;
    cmd->sidemove = (fx * wantY - fy * wantX) / det;
}

Manual manualDirection() noexcept
{
    return manual;
}

int desyncSide() noexcept
{
    return side;
}

static float sentPitch = 0.0f, sentYaw = 0.0f, sentDesync = 0.0f, sentRoll = 0.0f;
static bool hasSent = false;
// взгляд камеры до подмены — для предпросмотра в меню (видно, насколько углы уведены от настоящих)
static float cameraPitch = 0.0f, cameraYaw = 0.0f;

void storeSentAngles(float pitch, float yaw) noexcept
{
    sentPitch = pitch;
    sentYaw = yaw;
    sentDesync = tickDesync;
    sentRoll = currentRoll;
    hasSent = true;
}

bool sentServerPose(float& pitch, float& yaw, float& desync, float& roll) noexcept
{
    pitch = sentPitch;
    yaw = sentYaw;
    desync = sentDesync;
    roll = sentRoll;
    return hasSent;
}

bool sentAngles(float& pitch, float& yaw) noexcept
{
    pitch = sentPitch;
    yaw = sentYaw;
    return hasSent;
}

void cameraAngles(float& pitch, float& yaw) noexcept
{
    pitch = cameraPitch;
    yaw = cameraYaw;
}

void run(csgo::UserCmd* cmd, const EngineInterfaces& engineInterfaces, const ClientInterfaces& clientInterfaces, const Memory& memory, bool shotThisTick) noexcept
{
    auto& aa = config.antiAim;
    // взгляд камеры запоминаем до любых выходов — меню показывает его даже с выключенным AA
    cameraPitch = cmd->viewangles.x;
    cameraYaw = cmd->viewangles.y;
    currentRoll = 0.0f;
    tickDesync = 0.0f;
    snap.active = false;
    {
        std::scoped_lock lock{ extendedMutex };
        extendedShown.active = false;
    }
    if (!aa.enabled || !localPlayer || !localPlayer.get().isAlive()) {
        pinnedValid = false;
        cycleValid = false;
        return;
    }

    Frame f;
    f.cmd = cmd;
    f.engine = &engineInterfaces;
    f.clients = &clientInterfaces;
    f.memory = &memory;
    f.camera = cmd->viewangles;
    f.realtime = memory.globalVars->realtime;
    f.curtime = memory.globalVars->currenttime;

    // ---- анти-брутфорс: триггеры и таймер ----
    if (aa.antiBrute && ((aa.antiBruteTriggers & AbEnemyShot) || aa.antiBruteMode == static_cast<int>(AntiBruteMode::Counter)))
        processImpacts(engineInterfaces, clientInterfaces, memory);
    else
        pendingImpacts.clear();
    if (aa.antiBrute && (aa.antiBruteTriggers & AbLocalShot) && shotThisTick)
        triggerAntiBrute(f.realtime);
    if (!aa.antiBrute || (aa.antiBruteReset > 0.0f && lastAntiBrute >= 0.0f && f.realtime - lastAntiBrute > aa.antiBruteReset))
        if (lastAntiBrute >= 0.0f || haveOriginal)
            resetAntiBrute();

    const auto moveType = localPlayer.get().moveType();
    if (moveType == MoveType::LADDER || moveType == MoveType::NOCLIP)
        return;

    // ---- легит AA на E: первый тик E уходит как есть (дверь, бомба), дальше E забираем себе ----
    if (cmd->buttons & csgo::UserCmd::IN_USE) {
        if (!aa.legitAa)
            return;
        if (!legitWait) {
            legitWait = true;
            return;
        }
        if (!legitAllowed(cmd, clientInterfaces))
            return;
        cmd->buttons &= ~csgo::UserCmd::IN_USE;
        f.legit = true;
    } else {
        legitWait = false;
    }

    if (const auto weapon = csgo::Entity::from(retSpoofGadgets->client, localPlayer.get().getActiveWeapon()); weapon.getPOD()) {
        // граната: пока зажат бросок, выдернута чека или идёт сам бросок (он происходит через
        // несколько тиков после отпускания кнопки, по m_fThrowTime) — углы не трогаем,
        // иначе граната улетает по антиаимному pitch себе под ноги
        if (weapon.isGrenade()) {
            if (cmd->buttons & (csgo::UserCmd::IN_ATTACK | csgo::UserCmd::IN_ATTACK2))
                return;
            if (Netvars::get(fnv::hash("CBaseCSGrenade->m_bPinPulled")) && weapon.pinPulled())
                return;
            if (Netvars::get(fnv::hash("CBaseCSGrenade->m_fThrowTime")) && weapon.throwTime() > 0.0f)
                return;
        }

        // ручной выстрел, когда оружие готово, — углы камеры не трогаем
        const float serverTime = memory.globalVars->serverTime();
        bool ready = weapon.nextPrimaryAttack() <= serverTime && localPlayer.get().nextAttack() <= serverTime;
        // револьвер с зажатым ЛКМ сначала взводится (и «Prepare revolver» держит ЛКМ почти всегда) —
        // выстрел только когда барабан готов, до этого AA работает
        if (ready && weapon.itemDefinitionIndex() == WeaponId::Revolver && Netvars::get(fnv::hash("CWeaponCSBase->m_flPostponeFireReadyTime"))) {
            const float fireReady = weapon.postponeFireReadyTime();
            ready = fireReady > 0.0f && fireReady <= serverTime;
        }
        if ((cmd->buttons & csgo::UserCmd::IN_ATTACK) && ready) {
            if (aa.antiBrute && (aa.antiBruteTriggers & AbLocalShot))
                triggerAntiBrute(f.realtime);
            return;
        }
    }

    // тик выстрела не трогаем — там углы рейджбота
    if (shotThisTick)
        return;

    // Цикл чока: тик после отправленного пакета начинает новый. Всё, что шагает по тикам,
    // шагает здесь — чокнутая и отправленная команды должны нести один и тот же угол билдера.
    f.sendPacket = tickbase::sendPacket();
    f.cycleStart = lastSentPacket || !cycleValid;
    lastSentPacket = f.sendPacket;

    // ---- угроза: ближайший по прицелу враг ----
    csgo::Vector threatEye{};
    int threatIndex = 0;
    f.toTarget = atTargetsYaw(cmd, engineInterfaces, clientInterfaces, memory, &threatEye, &f.haveThreat, false, &threatIndex);
    f.threatOrigin = threatEye;
    f.threatOrigin.z -= 64.0f;
    f.eye = localPlayer.get().getEyePosition();
    f.targetPitch = f.haveThreat ? (threatEye - f.eye).toAngle().x : f.camera.x;
    f.maxDesync = localMaxDesync();
    f.inverterSide = aa.inverter.isToggled() ? 1 : -1;
    // Дефенсив AA (скрытые углы, LC-триггеры, Force LC). Как в Neverlose, у него свой заряд: галка DT
    // не нужна. Нужен детур CL_Move (на нём держится заряд) и чтобы заряд не был отдан второй пуле DT —
    // в быстрой стрельбе рывки его бы съедали, и DT оставался бы пустым.
    f.defensiveOn = !f.legit && aa.defensiveAa && defensive::enabled();

    // ---- настройки билдера: один раз на цикл ----
    if (f.cycleStart) {
        activeState = detectState(cmd);
        AntiAimState st = blended(resolveState(activeState), f.realtime);
        // анти-брутфорс «Подстройка»: настройки чуть уводим в случайную сторону, его брутфорс промахивается
        if (antiBruteWeight != 0.0f) {
            st.yawOffset += static_cast<int>(std::lround(antiBruteWeight * 15.0f));
            st.degree = std::max(0, static_cast<int>(std::lround(st.degree * (1.0f + antiBruteWeight))));
            const float scale = 1.0f - std::abs(antiBruteWeight) * 0.5f;
            st.desyncLeft = static_cast<int>(std::lround(st.desyncLeft * scale));
            st.desyncRight = static_cast<int>(std::lround(st.desyncRight * scale));
        }
        // переключение фазы — по циклам, а не по пакетам: «задержка» в билдере считается в них же
        if (++packetsSinceSwitch >= switchWait) {
            packetsSinceSwitch = 0;
            switchWait = nextSwitchWait(st);
            ++phase;
            const float spread = static_cast<float>(std::clamp(st.randomize, 0, 90));
            switchRandom = spread > 0.0f ? std::uniform_real_distribution<float>{ -spread, spread }(rng) : 0.0f;
            const float half = static_cast<float>(st.degree) * 0.5f;
            randomYaw = half > 0.0f ? std::uniform_real_distribution<float>{ -half, half }(rng) : 0.0f;
            randomSide = std::uniform_int_distribution<int>{ 0, 1 }(rng) ? 1 : -1;
        }
        cycleState = st;
        // модификатор считаем тут же: спин и «вращение» двигают свою фазу, и делать это каждый
        // тик нельзя — чокнутая команда уехала бы от отправленной на шаг спина
        cycleModifier = modifierYaw(st, static_cast<float>(st.degree) + switchRandom);
        cycleValid = true;
        // контр-резольвер решает раз на цикл — по врагу, на которого смотрим
        if (aa.antiBrute && aa.antiBruteMode == static_cast<int>(AntiBruteMode::Counter) && f.haveThreat) {
            // все живые враги в пределах досягаемости; на кого смотрим — главный
            std::array<Threat, 16> threats{};
            int threatCount = 0;
            const auto origin = localPlayer.get().getAbsOrigin();
            const int maxClients = std::min(engineInterfaces.getEngine().getMaxClients(), 64);
            for (int i = 1; i <= maxClients && threatCount < static_cast<int>(threats.size()); ++i) {
                const auto entity = csgo::Entity::from(retSpoofGadgets->client, clientInterfaces.getEntityList().getEntity(i));
                if (!entity.getPOD() || entity.getPOD() == localPlayer.get().getPOD() || entity.getNetworkable().isDormant()
                    || !entity.isAlive() || !entity.isOtherEnemy(memory, localPlayer.get()))
                    continue;
                const bool main = i == threatIndex;
                if (!main && (entity.getAbsOrigin() - origin).length() > 3000.0f)
                    continue;
                threats[threatCount++] = { i, main ? 1.0f : 0.5f };
            }
            counterPlan = planAgainst(threats.data(), threatCount, f.realtime, f.inverterSide);
        } else {
            counterPlan = CounterPlan{};
        }
    }
    f.st = cycleState;

    // ---- 1) план ----
    Plan plan = buildPlan(f);

    // ---- 2) поза ----
    Pose pose;
    pose.roll = plan.roll;
    pose.pitch = buildPitch(f, plan);
    pose.fakeYaw = plan.yaw;
    // скрытый угол забрал yaw — дальше билдер к нему уже ничего не добавляет: ни смещения,
    // ни модификатора, ни добавки по стороне, и сторона десинка на этих тиках не джиттерится
    if (applyDefensiveSnap(f, plan, pose))
        plan.staticAngles = true;
    if (!plan.staticAngles) {
        pose.fakeYaw += static_cast<float>(f.st.yawOffset);
        pose.fakeYaw += cycleModifier;
    }
    const bool flick = applyFlicks(f, pose);
    buildDesync(f, plan, pose, flick);

    // ---- 3) применение ----
    cmd->viewangles.x = std::clamp(pose.pitch, -89.0f, 89.0f);
    // вот и весь десинк: отправленная команда несёт угол головы, чокнутые — угол, за которым
    // сервер тянет ноги
    cmd->viewangles.y = std::remainder(f.sendPacket ? pose.fakeYaw : pose.realYaw, 360.0f);
    currentRoll = pose.roll;
    tickDesync = pose.desync;
    if (pose.side != 0) {
        side = pose.side;           // индикаторы: без десинка сторону не меняем
        applyBodyMovement(f, plan.owner == Owner::Extended);
    }
    if (f.sendPacket)
        pushPose({ f.realtime, pose.fakeYaw, pose.desync, localPlayer.get().getAbsOrigin(), f.eye.z });
    // слайдвок — в movement::fixMovement, после того как кнопки выставлены под реальный ввод
}

void onHurtByEnemy(int attackerIndex, int hitgroup, float realtime) noexcept
{
    if (hitgroup == 1)
        observeHeadHit(attackerIndex, realtime);
}

const char* counterStyle() noexcept
{
    return counterPlan.active ? counterPlan.style : "";
}

ExtendedView extendedView() noexcept
{
    std::scoped_lock lock{ extendedMutex };
    return extendedShown;
}

void updateEventListener(const EngineInterfaces& engineInterfaces, const Memory& memory, bool forceRemove) noexcept
{
    // слушатель bullet_impact только пока он нужен
    const auto& aa = config.antiAim;
    const bool wanted = !forceRemove && aa.enabled && aa.antiBrute && (aa.antiBruteTriggers & AbEnemyShot);
    if (wanted && !impactListenerRegistered) {
        engineInterfaces.getGameEventManager(memory.getEventDescriptor).addListener(&impactListener, csgo::bullet_impact);
        impactListenerRegistered = true;
    } else if (!wanted && impactListenerRegistered) {
        engineInterfaces.getGameEventManager(memory.getEventDescriptor).removeListener(&impactListener);
        impactListenerRegistered = false;
        pendingImpacts.clear();
    }
}

void onRoundStart() noexcept
{
    resetAntiBrute();
}

}

#pragma once

// Контр-резольвер антиаима: разбор выстрелов врага по нам и выбор стороны/величины десинка.
// Чистая логика без обращений к игре — её гоняет офлайн-тест tests/counter_test.cpp.

#include <algorithm>
#include <array>
#include <cmath>
#include <random>

#include <CSGO/Vector.h>
#include <Helpers.h>

namespace hvh::counter
{

inline std::mt19937 counterRng{ std::random_device{}() };

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
inline std::array<PoseSample, 128> poseHistory{};
inline int poseNext = 0;

inline void pushPose(const PoseSample& sample) noexcept
{
    poseHistory[poseNext] = sample;
    poseNext = (poseNext + 1) % static_cast<int>(poseHistory.size());
}

[[nodiscard]] inline const PoseSample* poseAt(float time) noexcept
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
inline std::array<EnemyModel, 65> enemyModels{};

// Знак геометрии: при стороне +1 (ноги левее взгляда) голова смещается влево от оси взгляда, луч в неё
// даёт lateral < 0. Знак уточняется попаданиями в голову: тогда враг целился точно в нашу сторону.
inline float geometryVotes = -1.0f;
inline constexpr float centerBand = 2.5f;     // |lateral| меньше — выстрел «по центру»

[[nodiscard]] inline int guessedSide(float lateral) noexcept
{
    if (std::abs(lateral) < centerBand)
        return 0;
    return (lateral > 0.0f) == (geometryVotes > 0.0f) ? 1 : -1;
}

inline void learnServerStyle(int guess, int previousGuess, int ourSide) noexcept;

// Выстрел врага в нас: разобрать, в какую сторону он целился
inline void observeShot(int shooterIndex, int userId, const csgo::Vector& shooterEye, const csgo::Vector& impact, float realtime, float latency) noexcept
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
inline void observeHeadHit(int shooterIndex, float realtime) noexcept
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
inline CounterPlan counterPlan;

// Шанс, что выстрел в сторону g попадёт в голову при нашей стороне s и доле десинка m (0.45…1):
// полный десинк уводит голову от центра, но в неё попадает выстрел «в нашу сторону»; low delta — наоборот
[[nodiscard]] inline float headRisk(int g, int s, float m) noexcept
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
inline EnemyStyle serverStyle;

inline void learnServerStyle(int guess, int previousGuess, int ourSide) noexcept
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

[[nodiscard]] inline EnemyStyle styleOf(const EnemyModel& model, float realtime) noexcept
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
[[nodiscard]] inline float riskFor(const EnemyStyle& own, int s, float low) noexcept
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
inline CounterPlan planAgainst(const Threat* threats, int threatCount, float realtime, int fallbackSide) noexcept
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
    int pick = near[std::uniform_int_distribution<int>{ 0, std::max(nearCount - 1, 0) }(counterRng)];
    for (int k = 0; k < nearCount; ++k) {
        if (options[near[k]].side == fallbackSide && options[near[k]].risk <= bestRisk + 0.005f
            && std::uniform_int_distribution<int>{ 0, 1 }(counterRng))
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
    // «Следит» — резольвер видит нашу сторону, её выбор его не обманет, спасает величина (её уже выбрал риск).
    // Сторону тогда берём случайно. Раньше она менялась строго через цикл — и против «перебора» в том же
    // ритме это захватывало само себя: каждый его выстрел совпадал с нашей стороной, выглядел как «следит»,
    // и мы продолжали меняться в такт под его пули (тест: 46% попаданий вместо 6%). Случайная сторона
    // разрывает такт, и «перебор» за пару выстрелов отделяется от «следит».
    if (strongest >= 0.5f && strongest == shown.follow)
        plan.side = std::uniform_int_distribution<int>{ 0, 1 }(counterRng) ? 1 : -1;
    return plan;
}


}

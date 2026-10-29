#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>

#include <CSGO/AnimationLayer.h>

// Сравнение слоёв анимации: какой кандидат-угол ног дал слои, похожие на сетевые.
// Вынесено отдельно от Resolver.cpp, чтобы математику можно было проверить тестами без игры.
namespace hvh::resolver
{

// Разброс поля между кандидатами меньше шага квантования сетевого слоя — сигнала в нём нет.
// Квантование из DT_Animationlayer в server_client.so: m_flCycle — 15 бит с округлением вниз (шаг 1/32767),
// m_flWeight — 8 бит на [0, 1] (шаг 1/255), m_flPlaybackRate — SPROP_NOSCALE, целый float.
// Раньше пороги были 0.004 по циклу и 0.02 по скорости — в десятки раз грубее, чем разница между
// сторонами десинка, и почти каждый разбор выходил «слои молчат».
constexpr float weightEpsilon = 0.005f;
constexpr float cycleEpsilon = 0.0001f;
constexpr float rateEpsilon = 0.00001f;
// насколько поле должно разводить кандидатов (в долях их разброса), чтобы считаться сигналом
constexpr float fieldSignal = 0.2f;

struct ScoreResult {
    int best = 0;
    int second = -1;
    int signals = 0;        // сколько полей слоёв вообще отреагировали на угол ног
    float error = 0.0f;     // ошибка лучшего кандидата на один сигнал
    float margin = 0.0f;    // насколько второй кандидат хуже лучшего
    float confidence = 0.0f;
};

namespace detail
{

[[nodiscard]] inline float plainDiff(float a, float b) noexcept
{
    return std::abs(a - b);
}

// цикл анимации идёт по кругу [0, 1): 0.99 и 0.01 — соседи, а не противоположности
[[nodiscard]] inline float cycleDiff(float a, float b) noexcept
{
    const float diff = std::abs(a - b);
    return std::min(diff, 1.0f - diff);
}

}

// simulated — candidateCount * layerCount слоёв подряд (по кандидату на строку), net — сетевые слои.
// resultYaw нужен, чтобы «вторым» считать кандидата с заметно другим углом: иначе отрыв всегда нулевой.
[[nodiscard]] inline ScoreResult scoreCandidates(const csgo::AnimationLayer* simulated, int candidateCount,
    const csgo::AnimationLayer* net, int layerCount, const float* resultYaw, float* error) noexcept
{
    ScoreResult result;
    if (candidateCount <= 0 || layerCount <= 0 || candidateCount > 64)
        return result;

    for (int c = 0; c < candidateCount; ++c)
        error[c] = 0.0f;

    const auto row = [&](int candidate, int layer) -> const csgo::AnimationLayer& {
        return simulated[static_cast<std::size_t>(candidate) * layerCount + layer];
    };

    int signals = 0;
    // Поле идёт в счёт относительно: лучшему кандидату в нём 0, остальным — насколько хуже. Иначе поле, где
    // сетевое значение далеко от всех кандидатов (наша тень анимации разошлась с серверной), добавляло каждому
    // одинаковые 2.0: отрыв лучшего тонул в общей ошибке, и почти всё выходило «низкая уверенность».
    static float fieldError[64];
    const auto addField = [&](int count) {
        float low = fieldError[0], high = low;
        for (int c = 1; c < count; ++c) {
            low = std::min(low, fieldError[c]);
            high = std::max(high, fieldError[c]);
        }
        // поле не разделяет кандидатов — сигнала нет
        if (high - low < fieldSignal)
            return;
        ++signals;
        for (int c = 0; c < count; ++c)
            error[c] += fieldError[c] - low;
    };
    const auto scoreField = [&](float (*get)(const csgo::AnimationLayer&), float (*diff)(float, float), float epsilon, int layer) {
        float low = get(row(0, layer)), high = low;
        for (int c = 1; c < candidateCount; ++c) {
            const float value = get(row(c, layer));
            low = std::min(low, value);
            high = std::max(high, value);
        }
        const float spread = high - low;
        if (!(spread > epsilon))
            return;
        // делим на разброс, но с добавкой в один квант: поле, где кандидаты расходятся на величину кванта
        // сетевого слоя, не должно весить столько же, сколько поле с явной разницей. Было 4 кванта —
        // при разнице скорости слоя движения в 1e-4..1e-3 запас съедал всё, и выходило «слои молчат»
        const float scale = spread + epsilon;
        const float target = get(net[layer]);
        for (int c = 0; c < candidateCount; ++c)
            fieldError[c] = std::min(diff(get(row(c, layer)), target) / scale, 2.0f);
        addField(candidateCount);
    };

    for (int layer = 0; layer < layerCount; ++layer) {
        scoreField([](const csgo::AnimationLayer& l) { return l.weight; }, detail::plainDiff, weightEpsilon, layer);
        scoreField([](const csgo::AnimationLayer& l) { return l.cycle; }, detail::cycleDiff, cycleEpsilon, layer);
        scoreField([](const csgo::AnimationLayer& l) { return l.playbackRate; }, detail::plainDiff, rateEpsilon, layer);

        // Разные последовательности у кандидатов — это поворот ног влево/вправо: сильнейший признак стороны
        bool sequenceVaries = false;
        for (int c = 1; c < candidateCount; ++c)
            sequenceVaries = sequenceVaries || row(c, layer).sequence != row(0, layer).sequence;
        if (sequenceVaries) {
            for (int c = 0; c < candidateCount; ++c)
                fieldError[c] = row(c, layer).sequence == net[layer].sequence ? 0.0f : 1.0f;
            addField(candidateCount);
        }
    }

    if (signals == 0)
        return result;

    int best = 0;
    for (int c = 1; c < candidateCount; ++c) {
        if (error[c] < error[best])
            best = c;
    }
    int second = -1;
    for (int c = 0; c < candidateCount; ++c) {
        if (c == best || std::abs(std::remainder(resultYaw[c] - resultYaw[best], 360.0f)) < 2.0f)
            continue;
        if (second < 0 || error[c] < error[second])
            second = c;
    }

    result.best = best;
    result.second = second;
    result.signals = signals;
    result.error = error[best] / static_cast<float>(signals);
    result.margin = second >= 0 ? (error[second] - error[best]) / static_cast<float>(signals) : 0.0f;
    result.confidence = std::clamp(result.margin / (result.error + result.margin + 0.02f), 0.0f, 1.0f);
    return result;
}

}

#pragma once

#include <algorithm>
#include <array>
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
// sequence слоя (номер анимации) от угла ног почти не зависит, но если он всё-таки разошёлся —
// это прямое попадание в сторону поворота, поэтому такой сигнал весит больше поля с плавающим значением
constexpr float sequenceWeight = 1.5f;
constexpr int maxCandidates = 64;

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

// Поле слоя: 0 — вес, 1 — цикл, 2 — скорость, 3 — sequence
[[nodiscard]] inline float fieldValue(const csgo::AnimationLayer& layer, int kind) noexcept
{
    switch (kind) {
    case 0: return layer.weight;
    case 1: return layer.cycle;
    case 2: return layer.playbackRate;
    default: return 0.0f;
    }
}

[[nodiscard]] inline float fieldEpsilon(int kind) noexcept
{
    switch (kind) {
    case 0: return weightEpsilon;
    case 1: return cycleEpsilon;
    case 2: return rateEpsilon;
    default: return 0.0f;
    }
}

[[nodiscard]] inline float normalizedFieldError(int kind, const csgo::AnimationLayer& candidate,
    const csgo::AnimationLayer& net, float scale) noexcept
{
    if (kind == 3)
        return candidate.sequence == net.sequence ? 0.0f : 1.0f;
    const float a = fieldValue(candidate, kind);
    const float b = fieldValue(net, kind);
    const float diff = kind == 1 ? detail::cycleDiff(a, b) : detail::plainDiff(a, b);
    return std::min(diff / scale, 2.0f);
}

// Параметры нормализации, снятые с готового набора кандидатов (обычно — с грубой сетки углов).
// По ним можно оценить ЛЮБОЙ другой угол так, что его ошибку можно сравнивать с ошибками
// кандидатов грубой сетки: без этого узкий набор «полушагов» получал бы свою нормировку,
// где шум выглядит как сигнал, и уточнение побеждало бы по пустякам.
struct Calibration {
    struct Field {
        int layer = 0;
        int kind = 0;
        float scale = 1.0f;   // разброс поля между кандидатами + один квант
        float low = 0.0f;     // минимальная ошибка по этому полю (её вычитаем, как и scoreCandidates)
        float weight = 1.0f;  // вклад поля в общую ошибку
    };
    static constexpr int maxFields = maxCandidates * 4;
    std::array<Field, maxFields> fields{};
    int count = 0;

    [[nodiscard]] float searchStepHint = 0.0f;   // не используется расчётом, удобно для логов
};

// Снимает нормировку по каждому полю, которое реально разделяет кандидатов.
[[nodiscard]] inline int buildCalibration(const csgo::AnimationLayer* simulated, int candidateCount,
    const csgo::AnimationLayer* net, int layerCount, Calibration& cal) noexcept
{
    cal.count = 0;
    if (candidateCount <= 0 || layerCount <= 0 || candidateCount > maxCandidates)
        return 0;

    const auto row = [&](int candidate, int layer) -> const csgo::AnimationLayer& {
        return simulated[static_cast<std::size_t>(candidate) * layerCount + layer];
    };

    float errors[maxCandidates];
    for (int layer = 0; layer < layerCount; ++layer) {
        for (int kind = 0; kind < 4; ++kind) {
            float scale = 1.0f;
            if (kind != 3) {
                const float first = fieldValue(row(0, layer), kind);
                float low = first, high = first;
                for (int c = 1; c < candidateCount; ++c) {
                    const float value = fieldValue(row(c, layer), kind);
                    low = std::min(low, value);
                    high = std::max(high, value);
                }
                const float spread = high - low;
                const float epsilon = fieldEpsilon(kind);
                if (!(spread > epsilon))
                    continue;   // поле не двигается от угла ног — сигнала нет
                // делим на разброс, но с добавкой в один квант: поле, где кандидаты расходятся на величину
                // кванта сетевого слоя, не должно весить столько же, сколько поле с явной разницей
                scale = spread + epsilon;
            } else {
                bool varies = false;
                for (int c = 1; c < candidateCount; ++c)
                    varies = varies || row(c, layer).sequence != row(0, layer).sequence;
                if (!varies)
                    continue;
            }

            float low = 2.0f, high = 0.0f;
            for (int c = 0; c < candidateCount; ++c) {
                errors[c] = normalizedFieldError(kind, row(c, layer), net[layer], scale);
                low = std::min(low, errors[c]);
                high = std::max(high, errors[c]);
            }
            // поле не разделяет кандидатов — сигнала нет
            if (high - low < fieldSignal)
                continue;
            if (cal.count >= Calibration::maxFields)
                return cal.count;

            auto& field = cal.fields[cal.count++];
            field.layer = layer;
            field.kind = kind;
            field.scale = scale;
            field.low = low;
            field.weight = kind == 3 ? sequenceWeight : 1.0f;
        }
    }
    return cal.count;
}

// Ошибка произвольного угла по снятой нормировке — в тех же единицах, что ошибки грубой сетки.
[[nodiscard]] inline float scoreAgainst(const csgo::AnimationLayer* candidate, int layerCount,
    const csgo::AnimationLayer* net, const Calibration& cal) noexcept
{
    float error = 0.0f;
    for (int i = 0; i < cal.count; ++i) {
        const auto& field = cal.fields[i];
        const auto& netLayer = net[field.layer];
        const auto& candidateLayer = candidate[field.layer];
        const float value = normalizedFieldError(field.kind, candidateLayer, netLayer, field.scale);
        error += field.weight * (value - field.low);
    }
    return error;
}

// Какая половина сетки лучше описывает сетевые слои.
// Шаг грубой сетки — 5-6°, и соседние узлы дают почти одинаковую ошибку: «отрыв от второго узла»
// меряет точность угла, а рейджботу нужна прежде всего СТОРОНА. Поэтому берём лучший узел каждой
// половины и сравниваем половины между собой: в каждую входит по десятку узлов, и выиграть целой
// половиной случайно гораздо труднее, чем одним узлом у соседа.
struct SideResult {
    int plus = -1;              // лучший кандидат с ногами правее взгляда
    int minus = -1;             // ...и левее
    float plusError = 0.0f;
    float minusError = 0.0f;
    float margin = 0.0f;        // насколько проигравшая половина хуже выигравшей, на один сигнал
    float side = 0.0f;          // +1 / -1 — победившая половина, 0 — сравнивать не с чем
};

// deadzone — полоса вокруг взгляда, которая не принадлежит ни одной стороне (ноги «почти прямо»)
[[nodiscard]] inline SideResult scoreSides(const float* error, const float* resultYaw, int count,
    float eyeYaw, float deadzone, int signals) noexcept
{
    SideResult out;
    for (int c = 0; c < count; ++c) {
        const float delta = std::remainder(resultYaw[c] - eyeYaw, 360.0f);
        if (delta > deadzone) {
            if (out.plus < 0 || error[c] < out.plusError) {
                out.plus = c;
                out.plusError = error[c];
            }
        } else if (delta < -deadzone) {
            if (out.minus < 0 || error[c] < out.minusError) {
                out.minus = c;
                out.minusError = error[c];
            }
        }
    }
    if (out.plus < 0 || out.minus < 0 || signals <= 0)
        return out;
    out.side = out.plusError < out.minusError ? 1.0f : -1.0f;
    out.margin = std::abs(out.plusError - out.minusError) / static_cast<float>(signals);
    return out;
}

// simulated — candidateCount * layerCount слоёв подряд (по кандидату на строку), net — сетевые слои.
// resultYaw нужен, чтобы «вторым» считать кандидата с заметно другим углом: иначе отрыв всегда нулевой.
[[nodiscard]] inline ScoreResult scoreCandidates(const csgo::AnimationLayer* simulated, int candidateCount,
    const csgo::AnimationLayer* net, int layerCount, const float* resultYaw, float* error) noexcept
{
    ScoreResult result;
    if (candidateCount <= 0 || layerCount <= 0 || candidateCount > maxCandidates)
        return result;

    Calibration cal;
    result.signals = buildCalibration(simulated, candidateCount, net, layerCount, cal);
    if (result.signals == 0)
        return result;

    const auto row = [&](int candidate) -> const csgo::AnimationLayer* {
        return simulated + static_cast<std::size_t>(candidate) * layerCount;
    };
    for (int c = 0; c < candidateCount; ++c)
        error[c] = scoreAgainst(row(c), layerCount, net, cal);

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
    result.error = error[best] / static_cast<float>(result.signals);
    result.margin = second >= 0 ? (error[second] - error[best]) / static_cast<float>(result.signals) : 0.0f;
    result.confidence = std::clamp(result.margin / (result.error + result.margin + 0.02f), 0.0f, 1.0f);
    return result;
}

}

#include <array>
#include <vector>

#include <gtest/gtest.h>

#include <Hacks/HvH/ResolverScoring.h>

namespace
{

constexpr int layerCount = 3;

// слои одного кандидата: вес / цикл / скорость / последовательность по слоям
struct LayerValues {
    float weight;
    float cycle;
    float playbackRate;
    int sequence;
};

[[nodiscard]] std::vector<csgo::AnimationLayer> makeLayers(const std::vector<std::array<LayerValues, layerCount>>& candidates)
{
    std::vector<csgo::AnimationLayer> out(candidates.size() * layerCount);
    for (std::size_t c = 0; c < candidates.size(); ++c) {
        for (int layer = 0; layer < layerCount; ++layer) {
            auto& target = out[c * layerCount + layer];
            const auto& source = candidates[c][layer];
            target.weight = source.weight;
            target.cycle = source.cycle;
            target.playbackRate = source.playbackRate;
            target.sequence = source.sequence;
        }
    }
    return out;
}

[[nodiscard]] std::array<csgo::AnimationLayer, layerCount> makeNet(const std::array<LayerValues, layerCount>& values)
{
    std::array<csgo::AnimationLayer, layerCount> out{};
    for (int layer = 0; layer < layerCount; ++layer) {
        out[layer].weight = values[layer].weight;
        out[layer].cycle = values[layer].cycle;
        out[layer].playbackRate = values[layer].playbackRate;
        out[layer].sequence = values[layer].sequence;
    }
    return out;
}

TEST(ResolverScoring, NoSignalWhenCandidatesProduceSameLayers) {
    const std::array<LayerValues, layerCount> same{ { { 0.5f, 0.25f, 1.0f, 7 }, { 0.0f, 0.0f, 0.0f, 0 }, { 1.0f, 0.5f, 1.0f, 9 } } };
    const auto simulated = makeLayers({ same, same, same });
    const auto net = makeNet(same);
    const std::array<float, 3> yaw{ -58.0f, 0.0f, 58.0f };
    std::array<float, 3> error{};

    const auto scored = hvh::resolver::scoreCandidates(simulated.data(), 3, net.data(), layerCount, yaw.data(), error.data());
    EXPECT_EQ(scored.signals, 0);
    EXPECT_FLOAT_EQ(scored.confidence, 0.0f);
}

TEST(ResolverScoring, PicksCandidateWhoseWeightMatchesNetwork) {
    const std::array<LayerValues, layerCount> left{ { { 0.1f, 0.20f, 1.0f, 7 }, { 0.0f, 0.0f, 0.0f, 0 }, { 1.0f, 0.5f, 1.0f, 9 } } };
    const std::array<LayerValues, layerCount> middle{ { { 0.5f, 0.50f, 1.0f, 7 }, { 0.0f, 0.0f, 0.0f, 0 }, { 1.0f, 0.5f, 1.0f, 9 } } };
    const std::array<LayerValues, layerCount> right{ { { 0.9f, 0.80f, 1.0f, 7 }, { 0.0f, 0.0f, 0.0f, 0 }, { 1.0f, 0.5f, 1.0f, 9 } } };
    const auto simulated = makeLayers({ left, middle, right });
    const auto net = makeNet(right);
    const std::array<float, 3> yaw{ -58.0f, 0.0f, 58.0f };
    std::array<float, 3> error{};

    const auto scored = hvh::resolver::scoreCandidates(simulated.data(), 3, net.data(), layerCount, yaw.data(), error.data());
    EXPECT_EQ(scored.best, 2);
    EXPECT_GT(scored.signals, 0);
    EXPECT_GT(scored.margin, 0.0f);
    EXPECT_GT(scored.confidence, 0.3f);
}

TEST(ResolverScoring, SurvivesQuantizationNoiseInNetworkedLayers) {
    const std::array<LayerValues, layerCount> left{ { { 0.10f, 0.20f, 1.0f, 7 }, { 0.0f, 0.0f, 0.0f, 0 }, { 1.0f, 0.5f, 1.0f, 9 } } };
    const std::array<LayerValues, layerCount> right{ { { 0.90f, 0.80f, 1.0f, 7 }, { 0.0f, 0.0f, 0.0f, 0 }, { 1.0f, 0.5f, 1.0f, 9 } } };
    const auto simulated = makeLayers({ left, right });
    // сервер шлёт вес и цикл квантованными — точного совпадения не бывает
    auto net = makeNet(left);
    net[0].weight += 0.004f;
    net[0].cycle -= 0.0015f;
    const std::array<float, 2> yaw{ -58.0f, 58.0f };
    std::array<float, 2> error{};

    const auto scored = hvh::resolver::scoreCandidates(simulated.data(), 2, net.data(), layerCount, yaw.data(), error.data());
    EXPECT_EQ(scored.best, 0);
    EXPECT_GT(scored.confidence, 0.3f);
}

TEST(ResolverScoring, SequenceMismatchDecidesSideWhenNumbersAreClose) {
    // доворот ног влево и вправо играет разными последовательностями при почти равных весах
    const std::array<LayerValues, layerCount> turnLeft{ { { 0.50f, 0.30f, 1.0f, 980 }, { 0.0f, 0.0f, 0.0f, 0 }, { 1.0f, 0.5f, 1.0f, 9 } } };
    const std::array<LayerValues, layerCount> turnRight{ { { 0.505f, 0.302f, 1.0f, 981 }, { 0.0f, 0.0f, 0.0f, 0 }, { 1.0f, 0.5f, 1.0f, 9 } } };
    const auto simulated = makeLayers({ turnLeft, turnRight });
    const auto net = makeNet(turnRight);
    const std::array<float, 2> yaw{ -58.0f, 58.0f };
    std::array<float, 2> error{};

    const auto scored = hvh::resolver::scoreCandidates(simulated.data(), 2, net.data(), layerCount, yaw.data(), error.data());
    EXPECT_EQ(scored.best, 1);
    EXPECT_EQ(scored.second, 0);
    EXPECT_GT(scored.margin, 0.0f);
}

TEST(ResolverScoring, TinyPlaybackRateDifferenceIsASignal) {
    // скорость слоя движения у сторон десинка расходится на тысячные — сервер шлёт её без округления
    const std::array<LayerValues, layerCount> left{ { { 0.5f, 0.30f, 1.0000f, 7 }, { 0.0f, 0.0f, 0.0f, 0 }, { 1.0f, 0.5f, 1.0f, 9 } } };
    const std::array<LayerValues, layerCount> middle{ { { 0.5f, 0.30f, 1.0015f, 7 }, { 0.0f, 0.0f, 0.0f, 0 }, { 1.0f, 0.5f, 1.0f, 9 } } };
    const std::array<LayerValues, layerCount> right{ { { 0.5f, 0.30f, 1.0030f, 7 }, { 0.0f, 0.0f, 0.0f, 0 }, { 1.0f, 0.5f, 1.0f, 9 } } };
    const auto simulated = makeLayers({ left, middle, right });
    const auto net = makeNet(right);
    const std::array<float, 3> yaw{ -58.0f, 0.0f, 58.0f };
    std::array<float, 3> error{};

    const auto scored = hvh::resolver::scoreCandidates(simulated.data(), 3, net.data(), layerCount, yaw.data(), error.data());
    EXPECT_EQ(scored.signals, 1);
    EXPECT_EQ(scored.best, 2);
    EXPECT_GT(scored.confidence, 0.3f);
}

TEST(ResolverScoring, PlaybackRateDifferenceOfTenThousandthsIsASignal) {
    // по игре стороны десинка расходятся по скорости слоя движения на 1e-4..1e-3
    const std::array<LayerValues, layerCount> left{ { { 0.5f, 0.30f, 0.9810f, 7 }, { 0.0f, 0.0f, 0.0f, 0 }, { 1.0f, 0.5f, 1.0f, 9 } } };
    const std::array<LayerValues, layerCount> right{ { { 0.5f, 0.30f, 0.9813f, 7 }, { 0.0f, 0.0f, 0.0f, 0 }, { 1.0f, 0.5f, 1.0f, 9 } } };
    const auto simulated = makeLayers({ left, right });
    const auto net = makeNet(left);
    const std::array<float, 2> yaw{ -58.0f, 58.0f };
    std::array<float, 2> error{};

    const auto scored = hvh::resolver::scoreCandidates(simulated.data(), 2, net.data(), layerCount, yaw.data(), error.data());
    EXPECT_EQ(scored.signals, 1);
    EXPECT_EQ(scored.best, 0);
}

TEST(ResolverScoring, FieldFarFromAllCandidatesDoesNotDrownTheSignal) {
    // слой 0: скорость явно указывает на правого; слой 2: цикл у всех далеко от сетевого (тень разошлась)
    const std::array<LayerValues, layerCount> left{ { { 0.5f, 0.30f, 1.000f, 7 }, { 0.0f, 0.0f, 0.0f, 0 }, { 1.0f, 0.100f, 1.0f, 9 } } };
    const std::array<LayerValues, layerCount> right{ { { 0.5f, 0.30f, 1.003f, 7 }, { 0.0f, 0.0f, 0.0f, 0 }, { 1.0f, 0.101f, 1.0f, 9 } } };
    const auto simulated = makeLayers({ left, right });
    auto net = makeNet(right);
    net[2].cycle = 0.6f;
    const std::array<float, 2> yaw{ -58.0f, 58.0f };
    std::array<float, 2> error{};

    const auto scored = hvh::resolver::scoreCandidates(simulated.data(), 2, net.data(), layerCount, yaw.data(), error.data());
    EXPECT_EQ(scored.best, 1);
    EXPECT_EQ(scored.signals, 1);
    EXPECT_GT(scored.confidence, 0.3f);
}

TEST(ResolverScoring, CycleWrapsAroundOne) {
    const std::array<LayerValues, layerCount> early{ { { 0.5f, 0.99f, 1.0f, 7 }, { 0.0f, 0.0f, 0.0f, 0 }, { 1.0f, 0.5f, 1.0f, 9 } } };
    const std::array<LayerValues, layerCount> late{ { { 0.5f, 0.50f, 1.0f, 7 }, { 0.0f, 0.0f, 0.0f, 0 }, { 1.0f, 0.5f, 1.0f, 9 } } };
    const auto simulated = makeLayers({ early, late });
    // сетевой цикл перевалил через 1.0: 0.01 — это сосед 0.99, а не 0.50
    auto net = makeNet(early);
    net[0].cycle = 0.01f;
    const std::array<float, 2> yaw{ -58.0f, 58.0f };
    std::array<float, 2> error{};

    const auto scored = hvh::resolver::scoreCandidates(simulated.data(), 2, net.data(), layerCount, yaw.data(), error.data());
    EXPECT_EQ(scored.best, 0);
}

TEST(ResolverScoring, SecondBestIgnoresCandidatesWithTheSameAngle) {
    const std::array<LayerValues, layerCount> left{ { { 0.10f, 0.20f, 1.0f, 7 }, { 0.0f, 0.0f, 0.0f, 0 }, { 1.0f, 0.5f, 1.0f, 9 } } };
    const std::array<LayerValues, layerCount> right{ { { 0.90f, 0.80f, 1.0f, 7 }, { 0.0f, 0.0f, 0.0f, 0 }, { 1.0f, 0.5f, 1.0f, 9 } } };
    // два кандидата привели к одному и тому же углу ног — второй должен браться не из них
    const auto simulated = makeLayers({ left, left, right });
    const auto net = makeNet(left);
    const std::array<float, 3> yaw{ 30.0f, 30.4f, -58.0f };
    std::array<float, 3> error{};

    const auto scored = hvh::resolver::scoreCandidates(simulated.data(), 3, net.data(), layerCount, yaw.data(), error.data());
    EXPECT_EQ(scored.best, 0);
    EXPECT_EQ(scored.second, 2);
    EXPECT_GT(scored.confidence, 0.3f);
}

}

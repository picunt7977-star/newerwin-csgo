// Офлайн-тест контр-резольвера AA (Source/Hacks/HvH/CounterResolver.h): без игры.
// Сборка и запуск: ./tests/run_counter_test.sh
//
// Враг с известным стилем резольвера стреляет по нам 40 раз. Перед каждым выстрелом контр-резольвер
// выбирает сторону и величину десинка, враг целится по своему стилю, импакт идёт в разбор.
// Сравниваем ожидаемую долю попаданий в голову с «наивным» AA (случайная сторона, полный десинк).

#include <cstdio>

#include <Hacks/HvH/CounterResolver.h>

using namespace hvh::counter;

namespace
{

int failures = 0;

enum class Style { Static, Flip, Follow, Center, Switch, Coin };
constexpr const char* styleNames[]{ "статика", "перебор", "следит", "центр", "статика→перебор", "наугад" };

// куда целится враг этого стиля
std::mt19937 coin{ 11 };
int shotIndex = 0;

int enemyGuess(Style style, int& last, int ourSide)
{
    ++shotIndex;
    switch (style) {
    case Style::Switch: return last = shotIndex <= 20 ? 1 : -last;
    case Style::Coin: return last = coin() & 1 ? 1 : -1;
    case Style::Static: return last = 1;
    case Style::Flip: return last = -last;
    case Style::Follow: return last = ourSide;
    default: return last = 0;
    }
}

double simulate(Style style, bool counter)
{
    // чистое состояние
    poseHistory = {};
    poseNext = 0;
    enemyModels = {};
    serverStyle = {};
    counterPlan = {};
    geometryVotes = -1.0f;
    std::mt19937 naive{ 7 };

    const int enemy = 3;
    const csgo::Vector enemyEye{ -600.0f, 0.0f, 64.0f };
    int last = -1;
    shotIndex = 0;
    coin.seed(11);
    double hits = 0.0;
    const int shots = 40;
    for (int k = 0; k < shots; ++k) {
        const float now = 10.0f + k * 1.5f;
        int side;
        float magnitude;
        if (counter) {
            const Threat threat{ enemy, 1.0f };
            counterPlan = planAgainst(&threat, 1, now, 1);
            side = counterPlan.active ? counterPlan.side : (naive() & 1 ? 1 : -1);
            magnitude = counterPlan.active ? counterPlan.magnitude : 1.0f;
        } else {
            side = naive() & 1 ? 1 : -1;
            magnitude = 1.0f;
        }
        // мы смотрим вдоль +x, враг сзади; поза ушла чуть раньше выстрела
        pushPose({ now - 0.08f, 0.0f, side * 58.0f * magnitude, { 0.0f, 0.0f, 0.0f }, 64.0f });
        const int g = enemyGuess(style, last, side);
        hits += headRisk(g, side, magnitude);
        // луч в голову стороны g: сторона +1 — голова слева от оси взгляда (y > 0)
        const float y = g == 0 ? 0.0f : g > 0 ? 5.0f : -5.0f;
        const csgo::Vector impact{ 300.0f, y * 1.5f, 64.0f };   // пуля ушла дальше за нас
        observeShot(enemy, 42, enemyEye, impact, now, 0.0f);
        // попадание в голову уточняет знак геометрии
        if (g == side && g != 0)
            observeHeadHit(enemy, now);
    }
    return hits / shots;
}

}

int main()
{
    for (int s = 0; s < 6; ++s) {
        const auto style = static_cast<Style>(s);
        const double naive = simulate(style, false);
        const double smart = simulate(style, true);
        // против «центра» и «наугад» лучше наивного не сделать — нужно только не сделать хуже
        const bool ok = style == Style::Center || style == Style::Coin ? smart <= naive + 0.03 : smart < naive - 0.05;
        std::printf("%s  враг «%s»: попаданий в голову без контр-резольвера %.0f%%, с ним %.0f%%\n",
            ok ? "OK  " : "FAIL", styleNames[s], naive * 100.0, smart * 100.0);
        if (!ok)
            ++failures;
    }
    std::printf(failures ? "\nПРОВАЛЕНО: %d\n" : "\nвсё прошло\n", failures);
    return failures ? 1 : 0;
}

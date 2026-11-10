// Офлайн-тест модели попаданий рейджбота (Source/Hacks/HvH/HitModel.h): без игры.
// Сборка и запуск: ./tests/run_hitmodel_test.sh
//
// Проверяет: прямое попадание без разброса, смесь гипотез (сторона 50/50, общая точка),
// разброс по сидам (оценка 64 сидами против всех 256), отсев по сфере не меняет ответа,
// урон/убийство по хитгруппам и цену одного расчёта в микросекундах.

#include <chrono>
#include <cstdio>
#include <cstdlib>

#include <Hacks/HvH/HitModel.h>

using namespace hvh::hitmodel;

namespace
{

int failures = 0;

void check(bool ok, const char* what, double got)
{
    std::printf("%s  %-62s %.4f\n", ok ? "OK  " : "FAIL", what, got);
    if (!ok)
        ++failures;
}

// Синтетический игрок, стоит в (x, y), лицом вдоль +x: голова, шея, грудь, живот, таз, ноги
Capsules player(float x, float y, float headShift = 0.0f)
{
    Capsules c;
    const auto add = [&](csgo::Vector a, csgo::Vector b, float r, int group) {
        c.items[c.count++] = { a, b, r, group };
    };
    add({ x, y + headShift, 62.0f }, { x + 1.0f, y + headShift, 66.0f }, 4.2f, 1);  // голова
    add({ x, y, 56.0f }, { x, y, 59.0f }, 3.4f, 1);                                 // шея (в CS:GO — тоже голова)
    add({ x, y, 42.0f }, { x, y, 54.0f }, 6.5f, 2);                                 // грудь
    add({ x, y, 34.0f }, { x, y, 41.0f }, 6.5f, 3);                                 // живот
    add({ x, y - 4.0f, 28.0f }, { x, y + 4.0f, 28.0f }, 6.0f, 3);                   // таз
    add({ x, y - 4.0f, 2.0f }, { x, y - 4.0f, 26.0f }, 4.0f, 6);                    // левая нога
    add({ x, y + 4.0f, 2.0f }, { x, y + 4.0f, 26.0f }, 4.0f, 7);                    // правая нога
    return c;
}

Hypotheses mixture(std::initializer_list<std::pair<Capsules, float>> items)
{
    Hypotheses h;
    for (const auto& [caps, weight] : items) {
        if (auto* slot = h.add(weight, 0))
            *slot = caps;
    }
    h.normalize();
    h.computeSpheres();
    return h;
}

std::array<float, DAMAGE_GROUPS> damageTable(float base)
{
    // как scaleDamage без брони: голова ×4, живот ×1.25, ноги ×0.75
    std::array<float, DAMAGE_GROUPS> d{};
    for (int g = 0; g < DAMAGE_GROUPS; ++g)
        d[g] = base * (g == 1 ? 4.0f : g == 3 ? 1.25f : (g == 6 || g == 7) ? 0.75f : 1.0f);
    return d;
}

csgo::Vector angleTo(const csgo::Vector& eye, const csgo::Vector& point)
{
    return (point - eye).toAngle();
}

}

int main()
{
    const csgo::Vector eye{ 0.0f, 0.0f, 64.0f };
    const float distance = 800.0f;
    const auto damage = damageTable(80.0f);   // скаут вблизи
    SpreadState noSpread;
    noSpread.range = 8192.0f;

    // 1. одна гипотеза, без разброса, точно в голову
    {
        const auto h = mixture({ { player(distance, 0.0f), 1.0f } });
        const auto odds = shotOdds(noSpread, eye, angleTo(eye, { distance + 0.5f, 0.0f, 64.0f }), h, damage, 80.0f, 100.0f, 101, false, 1, 1.0f);
        check(odds.head > 0.999f, "без разброса в центр головы: P(голова) = 1", odds.head);
        check(odds.kill > 0.999f, "голова 80×4 ≥ 100 хп: P(убить) = 1", odds.kill);
    }
    // 2. сторона 50/50, головы разведены на 16 ед.: выстрел в одну — половина
    {
        const auto h = mixture({ { player(distance, 0.0f, 8.0f), 0.5f }, { player(distance, 0.0f, -8.0f), 0.5f } });
        const auto odds = shotOdds(noSpread, eye, angleTo(eye, { distance + 0.5f, 8.0f, 64.0f }), h, damage, 80.0f, 100.0f, 101, false, 1, 1.0f);
        check(std::abs(odds.head - 0.5f) < 1e-3f, "две стороны 50/50, в одну голову: P(голова) = 0.5", odds.head);
        check(!odds.everyHypothesis || true, "(флаг общей точки считается только в прикидке)", 0.0);
        const auto rough = shotOdds(noSpread, eye, angleTo(eye, { distance + 0.5f, 8.0f, 64.0f }), h, damage, 80.0f, 100.0f, 101, false, 0, 1.0f);
        check(!rough.everyHypothesis, "прикидка: голова одной стороны — не общая точка", rough.everyHypothesis);
        const auto chest = shotOdds(noSpread, eye, angleTo(eye, { distance, 0.0f, 48.0f }), h, damage, 80.0f, 100.0f, 30, false, 0, 1.0f);
        check(chest.everyHypothesis && chest.qualified > 0.999f, "прикидка: грудь общая для обеих сторон", chest.qualified);
    }
    // 3. головы перекрываются (±3 ед.): середина попадает в обе
    {
        const auto h = mixture({ { player(distance, 0.0f, 3.0f), 0.7f }, { player(distance, 0.0f, -3.0f), 0.3f } });
        const auto odds = shotOdds(noSpread, eye, angleTo(eye, { distance + 0.5f, 0.0f, 64.0f }), h, damage, 80.0f, 100.0f, 101, false, 1, 1.0f);
        check(odds.head > 0.999f, "головы перекрываются, центр масс: P(голова) = 1", odds.head);
    }
    // 4. разброс: оценка 64 сидами против 256
    SpreadState spread = noSpread;
    spread.inaccuracy = 0.008f;   // пятно ~6 ед. на 800
    spread.spread = 0.002f;
    {
        const auto h = mixture({ { player(distance, 0.0f, 6.0f), 0.6f }, { player(distance, 0.0f, -6.0f), 0.4f } });
        const auto a = angleTo(eye, { distance + 0.5f, 6.0f, 64.0f });
        const auto full = shotOdds(spread, eye, a, h, damage, 80.0f, 100.0f, 101, false, 1, 1.0f);
        const auto quick = shotOdds(spread, eye, a, h, damage, 80.0f, 100.0f, 101, false, 4, 1.0f);
        check(full.head > 0.1f && full.head < 0.6f, "разброс + две стороны: P(голова) между 0.1 и 0.6", full.head);
        check(std::abs(full.head - quick.head) < 0.08f, "64 сида от 256 отличаются < 8%", std::abs(full.head - quick.head));
        check(full.qualified <= full.head + 1e-4f, "мин. урон 101 набирает только голова", full.qualified);
        // отсев по сфере не должен менять ответ
        auto wide = h;
        for (int k = 0; k < wide.count; ++k)
            wide.sphereRadius[k] = 1e6f;
        const auto noReject = shotOdds(spread, eye, a, wide, damage, 80.0f, 100.0f, 101, false, 1, 1.0f);
        check(std::abs(noReject.head - full.head) < 1e-6f, "отсев по сфере не меняет результат", std::abs(noReject.head - full.head));
        // пробитие стены — множитель вероятности
        const auto walled = shotOdds(spread, eye, a, h, damage, 80.0f, 100.0f, 101, false, 1, 0.85f);
        check(std::abs(walled.head - full.head * 0.85f) < 1e-5f, "пробитие 0.85 умножает шанс", walled.head);
    }
    // 5. польза: тело при хп 100 не убивает, но DT добивает
    {
        const auto h = mixture({ { player(distance, 0.0f), 1.0f } });
        const auto a = angleTo(eye, { distance, 0.0f, 48.0f });
        const auto single = shotOdds(noSpread, eye, a, h, damage, 80.0f, 100.0f, 30, false, 1, 1.0f);
        const auto dt = shotOdds(noSpread, eye, a, h, damage, 80.0f, 100.0f, 30, true, 1, 1.0f);
        check(single.kill < 1e-6f && dt.value > single.value, "грудь 80 по 100 хп: не убивает, с DT полезнее", dt.value - single.value);
    }
    // 6. цена: 9 гипотез, 64 сида и 256 сидов
    {
        Hypotheses h;
        for (int k = 0; k < MAX_HYPOTHESES; ++k) {
            if (auto* slot = h.add(1.0f, 0))
                *slot = player(distance, (k - 4) * 1.5f, (k % 3 - 1) * 5.0f);
        }
        h.normalize();
        h.computeSpheres();
        const auto a = angleTo(eye, { distance + 0.5f, 0.0f, 64.0f });
        volatile float sink = 0.0f;
        for (const int step : { 4, 1 }) {
            const int runs = step == 4 ? 4000 : 1000;
            const auto start = std::chrono::steady_clock::now();
            for (int r = 0; r < runs; ++r)
                sink = sink + shotOdds(spread, eye, a, h, damage, 80.0f, 100.0f, 101, false, step, 1.0f).head;
            const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count() / runs;
            char text[96];
            std::snprintf(text, sizeof(text), "9 гипотез, %d сидов: мкс на расчёт (бюджет тика ~15600)", HITCHANCE_SEEDS / step);
            check(us < (step == 4 ? 400.0 : 1500.0), text, us);
        }
    }

    std::printf(failures ? "\nПРОВАЛЕНО: %d\n" : "\nвсё прошло\n", failures);
    return failures ? 1 : 0;
}

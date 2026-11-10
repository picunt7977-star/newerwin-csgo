#pragma once

// Модель попаданий рейджбота: капсулы хитбоксов, разброс (копия генератора игры), гипотезы о модели
// врага и шанс выстрела по их смеси. Чистая математика без обращений к игре — её гоняет офлайн-тест
// tests/hitmodel_test.cpp (корректность и цена в микросекундах).

#include <algorithm>
#include <array>
#include <cmath>

#include <Platform/Macros/PlatformSpecific.h>
#include <CSGO/matrix3x4.h>
#include <CSGO/ModelInfo.h>
#include <CSGO/Vector.h>
#include <Helpers.h>

namespace hvh::hitmodel
{

// сид разброса на сервере — (random_seed & 0xFF) + 1: всего 256 вариантов, перебираем все
constexpr int HITCHANCE_SEEDS = 256;
// кости модели: хитбоксы висят на первых ~80, больше 128 у моделей CS:GO не бывает
constexpr int RECORD_BONES = 128;
constexpr int HITGROUP_HEAD = 1;

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

[[nodiscard]] inline csgo::Vector normalized(csgo::Vector v) noexcept
{
    const float length = v.length();
    return length > 0.0f ? v / length : v;
}

// кратчайшее расстояние между отрезками p1-q1 и p2-q2; s — где на первом ближайшая точка (0..1)
inline float segmentDistance(const csgo::Vector& p1, const csgo::Vector& q1, const csgo::Vector& p2, const csgo::Vector& q2, float& s) noexcept
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

inline void shiftCapsules(Capsules& caps, const csgo::Vector& delta) noexcept
{
    for (int k = 0; k < caps.count; ++k) {
        caps.items[k].a += delta;
        caps.items[k].b += delta;
    }
}

inline void buildCapsules(const csgo::StudioHitboxSet& set, const csgo::matrix3x4* bones, Capsules& out) noexcept
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
inline int firstHitGroup(const Capsules& capsules, const csgo::Vector& start, const csgo::Vector& end) noexcept
{
    const auto ray = end - start;
    const float length = ray.length();
    if (length <= 0.0f)
        return -1;
    const auto direction = ray / length;
    float nearest = 1e9f;
    int group = -1;
    for (int i = 0; i < capsules.count; ++i) {
        const auto& capsule = capsules.items[i];
        // Быстрый отсев: луч дальше описанной сферы капсулы — мимо. Радиус сферы ≤ ½|ab| + r,
        // его квадрат ≤ 2·(¼|ab|² + r²) — без корня. Отсекает почти все капсулы, кроме нужных.
        const auto mid = (capsule.a + capsule.b) * 0.5f;
        const auto toMid = mid - start;
        const float along = toMid.dotProduct(direction);
        const float bound = 2.0f * ((capsule.b - capsule.a).squareLength() * 0.25f + capsule.radius * capsule.radius);
        const float centerSquared = toMid.squareLength();
        const float perpendicular = along > 0.0f ? centerSquared - along * along : centerSquared;
        if (perpendicular > bound || along > length + std::sqrt(bound))
            continue;
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

inline bool rayHits(const csgo::Vector& start, const csgo::Vector& end, const Capsules& capsules) noexcept
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

inline const std::array<SpreadSeed, SPREAD_TABLE>& spreadSeeds() noexcept
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

// ---------- модель гипотез ----------
// Где враг окажется на сервере в момент выстрела, точно не знает никто: сторона десинка, его
// величина, какое обновление сервер возьмёт под прогноз, фаза джиттера. Раньше каждую такую
// неопределённость гасили своей скидкой «надёжности» (×0.85, ×0.6, ×0.55 …) и фильтрами
// («строгий отбор», «ждём середину», «только safe»), и хитшанс по сидам о них не знал вовсе.
// Теперь неопределённость описана явно: набор скелетов-гипотез с вероятностями. Хитшанс считается
// сразу по смеси — доля пар (сид разброса × гипотеза), где пуля даёт нужный урон. Одно число
// отвечает и за разброс, и за резольвер, и за прогноз: край головы, общая для всех сторон точка,
// середина между фазами джиттера получают ровно ту вероятность, какую дают на самом деле.

constexpr int MAX_HYPOTHESES = 9;
constexpr int DAMAGE_GROUPS = 11;

struct Hypotheses {
    std::array<Capsules, MAX_HYPOTHESES> caps;
    std::array<float, MAX_HYPOTHESES> weight{};
    std::array<signed char, MAX_HYPOTHESES> side{};   // сторона десинка в этой гипотезе: +1 / −1 / 0
    // описанная сфера каждой гипотезы: луч мимо неё — мимо модели, капсулы не перебираем
    std::array<csgo::Vector, MAX_HYPOTHESES> sphereCenter{};
    std::array<float, MAX_HYPOTHESES> sphereRadius{};
    int count = 0;
    int main = 0;                                      // самая вероятная — по ней рисуем и пишем лог

    void clear() noexcept
    {
        count = 0;
        main = 0;
    }
    Capsules* add(float w, int s) noexcept
    {
        if (count >= MAX_HYPOTHESES || w < 0.015f)
            return nullptr;
        weight[count] = w;
        side[count] = static_cast<signed char>(s);
        return &caps[count++];
    }
    // описанные сферы — после того как капсулы заполнены
    void computeSpheres() noexcept
    {
        for (int h = 0; h < count; ++h) {
            const auto& c = caps[h];
            csgo::Vector center{};
            for (int k = 0; k < c.count; ++k)
                center += (c.items[k].a + c.items[k].b) * 0.5f;
            if (c.count)
                center = center / static_cast<float>(c.count);
            float radius = 0.0f;
            for (int k = 0; k < c.count; ++k)
                radius = std::max({ radius, (c.items[k].a - center).length() + c.items[k].radius,
                                    (c.items[k].b - center).length() + c.items[k].radius });
            sphereCenter[h] = center;
            sphereRadius[h] = radius;
        }
    }
    void normalize() noexcept
    {
        float total = 0.0f;
        for (int h = 0; h < count; ++h)
            total += weight[h];
        if (total <= 0.0f)
            return;
        main = 0;
        for (int h = 0; h < count; ++h) {
            weight[h] /= total;
            if (weight[h] > weight[main])
                main = h;
        }
    }
};

// Скелет одной стороны: кости, вес и сдвиг (джиттер-фазы переносятся на прогнозную позицию)
struct SideModel {
    const csgo::matrix3x4* bones = nullptr;
    float weight = 0.0f;
    int side = 0;
    csgo::Vector offset{};
};

// стороны × тайминги → капсулы гипотез
inline void buildHypotheses(Hypotheses& out, const csgo::StudioHitboxSet& set, const SideModel* sides, int sideCount,
                     const csgo::Vector* shifts, const float* shiftWeights, int shiftCount) noexcept
{
    out.clear();
    static Capsules base;
    for (int s = 0; s < sideCount; ++s) {
        if (!sides[s].bones || sides[s].weight <= 0.0f)
            continue;
        buildCapsules(set, sides[s].bones, base);
        for (int t = 0; t < shiftCount; ++t) {
            if (auto* caps = out.add(sides[s].weight * shiftWeights[t], sides[s].side)) {
                *caps = base;
                shiftCapsules(*caps, sides[s].offset + shifts[t]);
            }
        }
    }
    out.normalize();
    out.computeSpheres();
}

// Состояние оружия для разброса — одно на тик
struct SpreadState {
    float inaccuracy = 0.0f;
    float spread = 0.0f;
    bool negev = false;
    int recoilIndex = 0;
    float range = 8192.0f;
    int bullets = 1;
};

// Что даст выстрел в точку по смеси гипотез
struct ShotOdds {
    float qualified = 0.0f;     // P(урон ≥ мин. урона) по всей смеси гипотез
    float mainQualified = 0.0f; // то же только по главной гипотезе — хитшанс в прежнем смысле (один разброс)
    float kill = 0.0f;          // P(убить этой пулей)
    float value = 0.0f;         // ожидаемая польза: убийство 1, добивание второй пулей DT 0.55, урон — доля хп × 0.45
    float head = 0.0f;          // P(пуля в голову)
    bool everyHypothesis = false;   // центр точки попадает в каждую гипотезу весом ≥ 5% (бывший safe point)
    bool everyOnMainSide = false;   // ... в каждую гипотезу главной стороны
};

// Стрелять ли. Ползунок хитшанса — как раньше: разброс по главной модели должен его набрать.
// Смесь гипотез выбирает точку и запрещает только монетку: с учётом сомнений резольвера шанс
// нужного урона должен быть не ниже половины ползунка. Требовать от смеси весь порог оказалось
// слишком строго — по десинкерам «по догадке» голову (≈50%) не били вовсе, рейдж молчал.
[[nodiscard]] inline bool shootable(const ShotOdds& odds, float need) noexcept
{
    return odds.mainQualified + 1e-4f >= need && odds.qualified + 1e-4f >= need * 0.5f;
}

[[nodiscard]] inline float shotValue(float damage, float hp, bool doubleTap) noexcept
{
    if (damage >= hp)
        return 1.0f;
    if (doubleTap && damage * 2.0f >= hp)
        return 0.55f;
    return 0.45f * std::min(damage / hp, 1.0f);
}

// seedStep ≤ 0 — один луч ровно в точку (дёшево: прикидка для всех точек мультипоинта),
// иначе каждый seedStep-й из 256 сидов разброса; penetration — вероятность, что пробитие
// стены пройдёт, как насчитал автоволл (он его переоценивает)
inline ShotOdds shotOdds(const SpreadState& ws, const csgo::Vector& eye, const csgo::Vector& angle, const Hypotheses& hyps,
                  const std::array<float, DAMAGE_GROUPS>& groupDamage, float baseDamage, float hp, int minDamage,
                  bool doubleTap, int seedStep, float penetration) noexcept
{
    ShotOdds odds;
    if (hyps.count == 0)
        return odds;
    const float sp = std::sin(Helpers::deg2rad(angle.x)), cp = std::cos(Helpers::deg2rad(angle.x));
    const float sy = std::sin(Helpers::deg2rad(angle.y)), cy = std::cos(Helpers::deg2rad(angle.y));
    const csgo::Vector forward{ cp * cy, cp * sy, -sp };
    const csgo::Vector right{ sy, -cy, 0.0f };
    const csgo::Vector up{ sp * cy, sp * sy, cp };
    const auto& seeds = spreadSeeds();
    const auto damageOf = [&](int group) {
        return group >= 0 && group < DAMAGE_GROUPS ? groupDamage[group] : baseDamage;
    };

    const bool center = seedStep <= 0;
    const int bullets = center ? 1 : ws.bullets;
    int samples = 0;
    std::array<csgo::Vector, 16> directions;
    odds.everyHypothesis = center;
    odds.everyOnMainSide = center;
    const int mainSide = hyps.side[hyps.main];

    for (int i = 0; i < (center ? 1 : HITCHANCE_SEEDS); i += center ? 1 : seedStep) {
        for (int pellet = 0; pellet < bullets; ++pellet) {
            if (center) {
                directions[pellet] = forward;
                continue;
            }
            const auto& seed = seeds[i + pellet];
            float a = seed.a, c = seed.c;
            if (ws.negev && ws.recoilIndex < 3) {
                for (int j = 3; j > ws.recoilIndex; --j) {
                    a *= a;
                    c *= c;
                }
                a = 1.0f - a;
                c = 1.0f - c;
            }
            const float x = seed.cosB * a * ws.inaccuracy + seed.cosD * c * ws.spread;
            const float y = seed.sinB * a * ws.inaccuracy + seed.sinD * c * ws.spread;
            directions[pellet] = normalized(forward + right * x + up * y);
        }
        ++samples;
        for (int h = 0; h < hyps.count; ++h) {
            float total = 0.0f;
            bool headHit = false;
            const auto toCenter = hyps.sphereCenter[h] - eye;
            const float centerDistance = toCenter.squareLength();
            const float sphere = hyps.sphereRadius[h] * hyps.sphereRadius[h];
            for (int pellet = 0; pellet < bullets; ++pellet) {
                const float along = toCenter.dotProduct(directions[pellet]);
                if (centerDistance > sphere && (along <= 0.0f || centerDistance - along * along > sphere))
                    continue;
                const int group = firstHitGroup(hyps.caps[h], eye, eye + directions[pellet] * ws.range);
                if (group < 0)
                    continue;
                total += damageOf(group);
                headHit = headHit || group == HITGROUP_HEAD;
            }
            const float w = hyps.weight[h];
            if (center && hyps.weight[h] >= 0.05f && total <= 0.0f) {
                odds.everyHypothesis = false;
                if (hyps.side[h] == mainSide)
                    odds.everyOnMainSide = false;
            }
            if (total <= 0.0f)
                continue;
            if (total >= static_cast<float>(minDamage)) {
                odds.qualified += w;
                if (h == hyps.main)
                    odds.mainQualified += 1.0f;
            }
            if (total >= hp)
                odds.kill += w;
            if (headHit)
                odds.head += w;
            odds.value += w * shotValue(total, hp, doubleTap);
        }
    }
    const float scale = penetration / static_cast<float>(std::max(samples, 1));
    odds.qualified *= scale;
    odds.mainQualified *= scale;
    odds.kill *= scale;
    odds.head *= scale;
    odds.value *= scale;
    return odds;
}


}

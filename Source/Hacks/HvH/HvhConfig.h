#pragma once

#include <array>
#include <map>

#include <InputUtil.h>
#include <JsonForward.h>

namespace hvh
{

// Группы оружия для рейджбота: у каждой свои настройки, «Общие» — запасной вариант
enum class WeaponGroup {
    General,
    Pistol,
    Heavy,  // Desert Eagle / R8
    Scout,
    Awp,
    Auto,   // SCAR-20 / G3SG1
    Rifle,
    Smg,
    Count
};

inline constexpr std::array weaponGroupNames{ "Общие", "Пистолеты", "Дигл / R8", "Скаут", "AWP", "Автоснайпы", "Винтовки", "ПП" };

enum HitboxFlags {
    HB_Head    = 1 << 0,
    HB_Chest   = 1 << 1,
    HB_Stomach = 1 << 2,
    HB_Pelvis  = 1 << 3,
    HB_Arms    = 1 << 4,
    HB_Legs    = 1 << 5,
    HB_Feet    = 1 << 6
};

struct RageWeapon {
    bool override = false;          // свои настройки вместо «Общих»
    int hitboxes = HB_Head | HB_Chest | HB_Stomach | HB_Pelvis;
    int multipoint = HB_Head | HB_Chest | HB_Stomach | HB_Pelvis;
    bool autoScale = true;          // размер точек от разброса на дистанции (тогда headScale/bodyScale — потолок)
    float headScale = 0.7f;
    float bodyScale = 0.6f;
    int minDamage = 30;
    int minDamageOverride = 10;     // по клавише
    int hitchance = 60;
    int autoStop = 2;               // AutoStop
    bool autoStopEarly = false;     // тормозить и между выстрелами (пока перезаряжается)
    bool autoScope = true;
    bool preferBody = false;
    bool bodyIfLethal = true;
};

// Автостоп: «Полный» — встать как вкопанный, «Точность» — сбросить скорость
// до ~1/3 максимальной, где разброс уже минимальный, и не терять подвижность
enum class AutoStop { Off, Full, Accuracy };

struct RageConfig {
    bool enabled = false;
    bool silent = true;
    bool autoFire = true;
    bool autowall = true;
    // Форсшут: если заданный мин. урон на этой цели недостижим, порог опускается до лучшего
    // достижимого урона и выстрел всё равно происходит (иначе рейдж «фоткает»)
    bool forceShot = false;
    bool resolver = true;           // резольвер десинка включён
    bool layerResolver = true;      // сравнение слоёв анимации (иначе только геометрия + перебор)
    int resolverSteps = 9;          // сколько углов-кандидатов перебирать на обновление (5/9/17)
    int safePoint = 1;              // 0 — выкл, 1 — после промаха, 2 — всегда (по десинкерам)
    float fov = 180.0f;
    KeyBind minDamageOverrideKey;
    KeyBind forceBodyKey;
    std::array<RageWeapon, static_cast<int>(WeaponGroup::Count)> weapons{};
};

enum class Pitch { Off, Down, Up, Zero };
enum class YawBase { Off, Backward, AtTargets };
enum class YawModifier { Off, Jitter, CenterJitter, Spin, Random, FiveWay };
// Флики (дефенсив AA): на тиках сдвига тикбейса — или раз в N пакетов — yaw дёргается вбок
enum class FlickTrigger { Defensive, Always };
enum class FlickSide { Alternate, Random, Custom };

struct AntiAimConfig {
    bool enabled = false;
    int pitch = static_cast<int>(Pitch::Down);
    int yawBase = static_cast<int>(YawBase::AtTargets);
    int yawOffset = 0;
    int yawModifier = static_cast<int>(YawModifier::CenterJitter);
    int modifierRange = 30;
    int spinSpeed = 10;

    bool desync = true;
    int desyncLeft = 58;
    int desyncRight = 58;
    bool desyncJitter = false;
    bool antiBrute = true;          // промах врага рядом с головой — меняем сторону
    KeyBindToggle inverter;
    bool freestanding = false;      // голову — за ближайшее укрытие от врага
    KeyBind freestandKey;           // если задана — фристенд только пока клавиша зажата

    KeyBind manualLeft{ KeyBind::LEFT };
    KeyBind manualRight{ KeyBind::RIGHT };
    KeyBind manualBack{ KeyBind::DOWN };

    // Дефенсив AA: пока враг целится в нас — голова вниз, тело уведено вбок от его прицела,
    // сторона дёргается на каждом отправленном пакете, чтобы его резольвер не успевал привыкнуть
    bool defensive = false;
    int defensiveFov = 25;          // насколько точно враг должен смотреть на нас, градусы
    int defensiveYaw = 50;          // на сколько уводить yaw от направления на врага
    int defensivePitch = 89;        // pitch в дефенсиве, 89 — вниз, -89 — вверх
    bool defensiveJitter = true;    // дёргать сторону на каждом отправленном пакете

    bool flicks = false;
    int flickTrigger = static_cast<int>(FlickTrigger::Defensive);
    int flickSide = static_cast<int>(FlickSide::Alternate);
    int flickAngle = 90;
    int flickYaw = 120;             // «Свой yaw»: смещение от базы, знак — сторона
    bool flickPitchOverride = false; // на флике свой pitch, иначе как у AA
    int flickPitch = -89;            // градусы, -89 — вверх, 89 — вниз
    int flickEvery = 12;            // «Всегда»: раз в столько отправленных пакетов

    int fakeLag = 1;                // сколько тиков чокать (1..14)
    bool slideWalk = false;         // ноги не крутятся при ходьбе
};

struct ExploitConfig {
    bool doubleTap = false;
    KeyBindToggle doubleTapKey;
    int shiftTicks = 13;
    KeyBind teleportKey;            // нажать: мгновенно прогнать заряженные тики (рывок)
    bool defensive = false;         // враг только что увидел нас — авто-рывок, его лагкомп по нам ломается
};

struct ViewConfig {
    bool removeScopeZoom = true;    // прицел без приближения
    bool secondZoom = true;         // при «без зума»: второй уровень скопа всё же приближает
    float secondZoomFactor = 2.0f;  // во сколько раз
    bool showFakeAngles = true;     // в 3-м лице модель показывает то, что видит сервер
    bool grenadePrediction = true;  // траектория гранаты в руке
    float dormantTime = 6.0f;       // сколько секунд держать ESP игрока, пропавшего из PVS
    bool syncRender = true;         // mat_queue_mode 0: кадр и ESP из одного тика, без дёрганья
    bool screenLogs = true;         // попадания/промахи коротко в левом верхнем углу
};

// Автострейф: «По камере» — летим туда, куда смотрим (S — назад), «WASD» — направление клавишами
enum class AutoStrafe { Off, Camera, Keys };

struct MovementConfig {
    KeyBind autoPeekKey;            // держать: запомнить точку, после выстрела вернуться
    bool autoPeekOnRelease = true;  // возвращаться и когда отпустил WASD
    KeyBind fakeDuckKey;
    bool fastLadder = false;
    bool bunnyHop = false;
    int autoStrafe = 1;             // AutoStrafe
};

// Скинченджер без инвентаря игры: «запасные» поля оружия (m_nFallbackPaintKit…) — работает и без GC
struct SkinEntry {
    int paintKit = 0;
    float wear = 0.0001f;
    int seed = 0;
    int statTrak = -1;              // -1 — без счётчика
};

struct SkinsConfig {
    std::map<int, SkinEntry> weapons;  // WeaponId -> скин (ножи и перчатки тоже здесь)
    int knife = 0;                  // WeaponId ножа, 0 — стандартный
    int gloves = 0;                 // WeaponId перчаток, 0 — стандартные
    int agentT = 0;                 // WeaponId агента за каждую команду, 0 — стандартный
    int agentCT = 0;
};

// Автобай в начале раунда. Порядок покупки: основное, броня, пистолет, зевс, гранаты, кусачки —
// денег не хватило, и дешёвое просто не купится
enum AutoBuyExtra { BuyArmor = 1 << 0, BuyTaser = 1 << 1, BuyHe = 1 << 2, BuySmoke = 1 << 3, BuyMolotov = 1 << 4, BuyFlash = 1 << 5, BuyDefuser = 1 << 6 };

struct AutoBuyConfig {
    bool enabled = false;
    int primary = 1;                // 0 — ничего, 1 — скаут, 2 — AWP, 3 — автоснайпа
    int secondary = 1;              // 0 — ничего, 1 — дигл/R8 (что в лоадауте), 2 — дуалы, 3 — P250, 4 — Tec-9/Five-SeveN
    int extras = BuyArmor | BuyTaser | BuyHe | BuySmoke | BuyMolotov | BuyDefuser;
};

struct Config {
    ViewConfig view;
    RageConfig rage;
    AntiAimConfig antiAim;
    ExploitConfig exploits;
    MovementConfig movement;
    AutoBuyConfig autoBuy;
    SkinsConfig skins;
};

inline Config config;

[[nodiscard]] const RageWeapon& weaponSettings(WeaponGroup group) noexcept;

json toJson() noexcept;
void fromJson(const json& j) noexcept;
void resetConfig() noexcept;

}

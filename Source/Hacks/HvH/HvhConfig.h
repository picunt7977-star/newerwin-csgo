#pragma once

#include <array>
#include <map>
#include <vector>

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
    int minDamage = 30;             // другое значение на клавишу — бинд (ПКМ по строке)
    int hitchance = 60;
    int autoStop = 2;               // AutoStop
    bool autoStopEarly = false;     // тормозить и между выстрелами (пока перезаряжается)
    bool peekStop = true;           // тормозить и скопиться заранее, когда через пару тиков выйдем на врага
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
    // достижимого урона и выстрел всё равно происходит (иначе рейдж «фоткает»).
    // Клавиша — бинд на строку (ПКМ), как и у всего остального.
    bool forceShot = false;
    bool forceBody = false;         // только в тело (обычно на бинде «зажатие»)
    bool resolver = true;           // резольвер десинка включён
    bool layerResolver = true;      // сравнение слоёв анимации (иначе только геометрия + перебор)
    // Сторона десинка не доказана — бить точки, попадающие в обе стороны (обычно тело):
    // попадание почти гарантировано, а «край головы» при неверной стороне — промах
    bool resolverSafeWhenUnsure = true;
    int resolverSteps = 9;          // сколько углов-кандидатов перебирать на обновление (5/9/17)
    bool strictShots = true;        // строгий отбор: по десинкерам только безопасные точки, без прогноза по прыгающим/джиттерящим
    int safePoint = 1;              // 0 — выкл, 1 — после промаха, 2 — всегда (по десинкерам)
    // Нож: бьёт сам, когда враг в досягаемости (ЛКМ — 48 ед., ПКМ — 32 ед., в спину ×3).
    // 0 — только наповал, 1 — любой удар, если враг в досягаемости
    bool knifeBot = false;
    int knifeBotMode = 0;
    // Зевс: стреляет, только если убивает (тело, в пределах дальности), со своим хитшансом
    bool zeusBot = false;
    int zeusHitchance = 80;
    int zeusMargin = 15;        // запас урона сверх хп: на краю дальности зевс бьёт впритык и не убивает
    // Дормант-аимбот: враг вне PVS, но слышен (шаги/выстрел) или виден союзникам на радаре — стрелять
    // в живот по последней позиции, если пробиваем на мин. урон. dormantAccuracy — насколько свежей
    // должна быть позиция: 100% — только что услышали, 25% — до ~1.1 с назад
    bool dormantAim = false;
    int dormantAccuracy = 65;
    float fov = 180.0f;
    std::array<RageWeapon, static_cast<int>(WeaponGroup::Count)> weapons{};
};

enum class Pitch { Off, Down, Up, Zero };
enum class YawBase { Off, Backward, AtTargets };
// Модификатор yaw. Старые значения (0..5) сохранены — конфиги читаются как раньше, «5-way» стал «N-way».
//  Джиттер — 0 / +градус; Центр — ±градус; Спин — оборот; Рандом — случайно в ±градус;
//  N-way — N положений от −градуса до +градуса по кругу; Вращение — плавно туда-обратно в ±градус;
//  Скиттер — рваный порядок −,+,0,−,+,0,−,0,+,−,0,+ (сторону десинка в джиттере тянет за собой)
enum class YawModifier { Off, Jitter, CenterJitter, Spin, Random, FiveWay, Rotate, Skitter, Count };
// Десинк: Статика — одна сторона (инвертер/анти-брут); Джиттер — смена стороны с модификатором; Рандом — случайно
enum class DesyncMode { Off, Static, Jitter, Random, Count };
// Задержка переключения модификатора/джиттера: каждые N отправленных пакетов; «Плавающая» — 1, 2 … N по кругу;
// «Случайная» — от 1 до N; «Качели» — то N, то 1
enum class SwitchDelay { None, Static, Fluctuate, Random, Switch, Count };
// Флики (дефенсив AA): на тиках сдвига тикбейса — или раз в N пакетов — yaw дёргается вбок
enum class FlickTrigger { Defensive, Always };
enum class FlickSide { Alternate, Random, Custom };

// Состояние игрока для билдера AA. «Общее» — запасное: состояние без своих настроек берёт его
// («Воздух + присед» без своих — сначала «Воздух», «Присед в движении» — «Присед»).
// «Фейклаг» — когда ни DT, ни дефенсив не включены (чок длинный): главнее состояния движения.
enum class AaState { Default, Standing, Running, Walking, Air, AirCrouch, Crouching, Sneaking, Fakelag, Count };
inline constexpr std::array aaStateNames{ "Общее", "Стоя", "Бег", "Шаг (shift)", "Воздух", "Воздух + присед", "Присед", "Присед в движении", "Фейклаг" };

// Дефенсив AA (скрытые углы): пока тикбейс откатился (лагкомп по нам не пишется — враг видит эти углы,
// но ни бектрекнуть, ни разрезольвить не может), pitch и yaw подменяются своими.
enum class SnapState { Default, Air, AirCrouch, Crouching, Sneaking, Walking, EdgeBait, Count };
inline constexpr std::array snapStateNames{ "Общее", "Воздух", "Воздух + присед", "Присед", "Присед в движении", "Шаг (shift)", "У стены (фристенд)" };
enum class SnapMode { Default, Off, Custom };   // «Как общее» / выкл / своё
enum class SnapPitch { Off, Static, Jitter, Random, RandomStatic, Spin, SpinFull, Camera, AtTarget, Count };
enum class SnapYaw { Off, Static, Jitter, Random, RandomJitter, RandomStatic, Spin, SpinFull, SpinJitter, Way90, Sway180, Opposite, Count };

struct DefensiveSnap {
    int mode = static_cast<int>(SnapMode::Default);
    int pitch = static_cast<int>(SnapPitch::Off);
    int pitchAngle = -89;
    int pitchAngle2 = 89;
    int pitchSpeed = 20;            // десятые оборота в секунду (как в hysteria: 20 = 2 об/с)
    int yaw = static_cast<int>(SnapYaw::Off);
    int yawAngle = 180;
    int yawDelay = 0;               // пакетов между переключениями джиттера
    int yawSpeed = 20;
    int duration = 13;              // сколько тиков окна дефенсива занимать (13 — всё окно)
};

enum class SafeHead { Off, Basic, Smart };
// Контр-резольвер: по импактам пуль врага учит, как он нас резольвит, и выбирает сторону/величину десинка
enum class AntiBruteMode { Flip, Adjust, PresetRack, Counter };
enum AntiBruteTrigger { AbEnemyShot = 1 << 0, AbLocalShot = 1 << 1 };

struct AntiAimState {
    bool override = false;          // свои настройки (для «Общего» не используется)
    int yawOffset = 0;
    int addLeft = 0;                // добавка к yaw, пока десинк влево...
    int addRight = 0;               // ...и вправо — так джиттер-десинк ведёт голову за собой
    int modifier = static_cast<int>(YawModifier::CenterJitter);
    int degree = 30;                // размах модификатора
    int ways = 5;                   // для N-way
    int spinSpeed = 10;             // для спина и вращения
    int randomize = 0;              // ± случайно к размаху на каждом переключении, градусы
    int desyncMode = static_cast<int>(DesyncMode::Static);
    int desyncLeft = 58;
    int desyncRight = 58;
    bool desyncInvert = false;      // в «Статике» — другая сторона по умолчанию
    int delayMode = static_cast<int>(SwitchDelay::None);
    int delayTicks = 2;             // пакетов между переключениями
};

// Снимок билдера и дефенсива — для «стойки пресетов» анти-брутфорса
struct AntiAimPreset {
    std::array<AntiAimState, static_cast<int>(AaState::Count)> states{};
    std::array<DefensiveSnap, static_cast<int>(SnapState::Count)> snaps{};
};

struct AntiAimConfig {
    bool enabled = false;
    int pitch = static_cast<int>(Pitch::Down);
    int yawBase = static_cast<int>(YawBase::AtTargets);
    std::array<AntiAimState, static_cast<int>(AaState::Count)> states{};

    bool desync = true;             // общий выключатель десинка (фейклаг держит ≥1 тик ради него)
    bool antiBrute = true;          // враг стрелял в нас (или мы выстрелили) — меняем AA
    int antiBruteTriggers = AbEnemyShot;
    int antiBruteMode = static_cast<int>(AntiBruteMode::Flip);
    int antiBrutePower = 25;        // «Подстройка»: насколько сдвигать углы, %
    float antiBruteReset = 0.0f;    // через сколько секунд вернуть как было (0 — не возвращать)
    int antiBruteOrder = 0;         // стойка: 0 — случайно, 1 — по очереди
    std::vector<AntiAimPreset> presetRack;
    KeyBindToggle inverter;
    bool freestanding = false;      // голову — за ближайшее укрытие от врага
    KeyBind freestandKey;           // если задана — фристенд только пока клавиша зажата
    bool staticOnManual = true;     // на ручном AA — без модификатора и джиттера десинка, десинк на полную
    bool freestandStatic = true;    // то же на фристенде
    bool edgeYaw = false;           // стена вплотную — лицом в неё (голова в стене)
    KeyBind edgeKey;
    bool edgeStatic = true;
    int roll = 0;                   // наклон головы (roll), знак — сторона; в движении до ±40
    int extendedYaw = 0;            // extended yaw: стоя ломаем LBY флик-тиком на столько градусов в сторону десинка (0 — выкл)
    int safeHead = static_cast<int>(SafeHead::Off);
    bool legitAa = false;           // на E — смотреть куда смотришь, десинк остаётся
    bool naturality = false;        // плавный переход между состояниями билдера
    float naturalityTime = 0.2f;

    KeyBind manualLeft{ KeyBind::LEFT };
    KeyBind manualRight{ KeyBind::RIGHT };
    KeyBind manualBack{ KeyBind::DOWN };
    KeyBind manualForward;

    // Скрытые углы: что показывать на тиках окна дефенсива. Само окно — в DefensiveConfig ниже
    bool defensiveAa = false;
    std::array<DefensiveSnap, static_cast<int>(SnapState::Count)> snaps{};

    bool flicks = false;
    int flickTrigger = static_cast<int>(FlickTrigger::Defensive);
    int flickSide = static_cast<int>(FlickSide::Alternate);
    int flickAngle = 90;
    int flickYaw = 120;             // «Свой yaw»: смещение от базы, знак — сторона
    bool flickPitchOverride = false; // на флике свой pitch, иначе как у AA
    int flickPitch = -89;            // градусы, -89 — вверх, 89 — вниз
    int flickEvery = 12;            // «Всегда»: раз в столько отправленных пакетов

    // Фейклаг: сколько тиков держать команды в чоке. Чем длиннее чок, тем дальше модель на
    // сервере от настоящей позиции. Режимы — как в NL: ровно столько тиков, случайно, адаптивно
    // по скорости (держать смещение у порога телепорта сервера) и «ломать лагкомп» (перешагнуть
    // этот порог: сервер считает такой шаг телепортом и запись лагкомпа по нам не интерполирует).
    int fakeLag = 1;                // сколько тиков чокать (1..14)
    int fakeLagMode = 0;            // FakeLagMode
    int fakeLagAir = 0;             // свой лимит в воздухе (0 — как основной)
    int fakeLagStanding = 0;        // свой лимит, пока стоим (0 — как основной)
    bool slideWalk = false;         // ноги не крутятся при ходьбе

    // Аним брейкер (своя модель): ноги на земле / в воздухе — 0 выкл, 1 статика, 2 мунвок;
    // «Статика» на земле включает и слайд-волк (его видят враги), остальное — только у себя
    bool animBreaker = false;
    int breakerGround = 0;
    int breakerAir = 0;
    bool breakerLandPitch = false;  // при приземлении голова не кивает (pitch 0)
};

enum class AnimBreak { Off, Static, Moonwalk };

// Фейклаг. «Адаптивный» и «Ломать лагкомп» считают лимит от своей скорости: сервер считает
// телепортом шаг больше 64 юнитов по горизонтали (m_flTeleportDistanceSqr) — адаптивный держится
// под этим порогом, «ломать» старается его перешагнуть (на земле скорости обычно не хватает).
enum class FakeLagMode { Static, Random, Adaptive, Break, Count };
inline constexpr std::array fakeLagModeNames{ "Статика", "Случайно", "Адаптивный", "Ломать лагкомп" };

// Дефенсив: когда открывать окна. Биты триггеров
enum DefensiveTrigger {
    DefPeek    = 1 << 0,    // вот-вот выйдем врагу на глаза (или он только что нас увидел)
    DefVisible = 1 << 1,    // пока нас кто-то видит — держать окна подряд
    DefNear    = 1 << 2,    // живой враг рядом, даже если не видит
    DefKey     = 1 << 3,    // по клавише
    DefShot    = 1 << 4,    // сразу после своего выстрела (бектрек по тику выстрела не сработает)
};

// Как копить под окно. Разница в том, создаётся ли команда на «заряжаемом» тике.
enum class ChargeStyle {
    Choke,  // чоком: команда создаётся, но уходит позже — серверная модель отстаёт, мы едем ровно
    Skip,   // пропуском вызова CL_Move: окно приходит быстрее, но персонаж на этих тиках не идёт
    Count
};
inline constexpr std::array chargeStyleNames{ "Чоком (плавно)", "Пропуском тиков" };

struct DefensiveConfig {
    bool enabled = false;
    // «Чоком» — по умолчанию: на пропусках персонаж терял треть скорости (1 из 3 тиков без команды),
    // а рывок прогонял всю пачку за один тик, предсказание улетало вперёд и коррекция тянула назад
    int chargeStyle = static_cast<int>(ChargeStyle::Choke);
    int triggers = DefPeek | DefVisible;
    // В воздухе — окна всегда (в прыжке нас обычно ещё не видят, и «пока видят» не срабатывал,
    // а фейклаг с DT срезан до 1 тика — лагкомп в воздухе не ломался ничем). Отдельная галка, чтобы
    // включилась и в старых конфигах
    bool inAir = true;
    KeyBind key;
    int peekTicks = 8;      // за сколько тиков до «увидит» считать это пиком
    // Заряд под рывок. Сервер откатывает тикбейс, когда промах больше sv_clockcorrection_msecs
    // (~60 мс ≈ 4 тика) — полные 13 не нужны. Меньше заряд → чаще окна, но каждое короче
    int chargeTicks = 10;
    // Зарядка пропускает вызовы CL_Move: на пропущенном тике не создаётся команда и стоит персонаж.
    // Пропускаем 1 тик из N: один пропуск — 15 мс и не чувствуется, подряд (1) — рывками
    int chargeSpread = 3;
    // Минимум тиков между рывками (у NL это `command_number % mode_duration`, по умолчанию 8).
    // Не даёт жечь заряд очередью рывков, когда триггер висит всё время
    int interval = 8;
    // Рывок зарядом DT, когда враг нас только увидел (или вот-вот увидит). Нужен в связке «DT в режиме
    // дефенсив + окна чоком»: там заряд DT (до 14 тиков) копился и лежал без дела — чоковая пачка лагкомп
    // не ломает (за 12 тиков проходим < 64 ед.), а настоящего сдвига с откатом тикбейса не было вовсе.
    // Лог 13.11: из 79 попаданий по нам 54 пришлись на «заряд есть, окна нет».
    bool peekShift = false;
};

struct ExploitConfig {
    bool doubleTap = false;
    KeyBindToggle doubleTapKey;
    // 0 — быстрая стрельба (сдвиг на выстреле, вторая пуля); 1 — дефенсив DT: выстрел обычный, заряд уходит
    // на рывок при выглядывании (враг только что увидел) и LC-триггеры. У скаута/AWP всегда дефенсив:
    // второй пули у них нет, а на серверах с DT-фиксом сдвиг на выстреле ломает следующий выстрел.
    int doubleTapMode = 0;
    int shiftTicks = 13;
    // Сколько команд удалённый сервер обрабатывает за свой тик. sv_maxusrcmdprocessticks не реплицируется:
    // клиент видит своё 16, а сервер режет раньше. Логи 11.11 (боты, лаг-пик): пачка 2–6 — 24 из 30
    // попаданий, 7–8 — 2 из 10, 9+ — 1 из 19: хвост пачки сервер выкидывал, выстрел шёл из-за стены.
    // Чок, фейклаг, сдвиг DT и пачки дефенсива — не больше этого. На своём (локальном) сервере — cvar.
    // 0 — не ограничивать (как было до 11.11). Ставить, только если сервер правда режет: общий лимит
    // в 6 сломал фейкдак (ему нужен чок ~14) и дёргал модель — сервер такие пачки принимал
    int serverPackLimit = 0;
    // Latency spoof (фейк-пинг): серверу подтверждаем его пакеты с опозданием — он считает наш пинг больше
    // и откатывает врагов дальше (бектрек до ~0.4 с вместо 0.2). Свою картинку при этом видим свежей.
    bool fakeLatency = false;
    int fakeLatencyMs = 150;        // сколько добавить к пингу, мс (сервер всё равно режет откат по sv_maxunlag)
    // Нож в руках, в прыжке и включён DT — никаких лагов (фейклаг, лаг-пик, дефенсив): прыжки с ножом
    // идут ровно, без рывков модели
    bool knifeAirNoLag = true;
    KeyBind teleportKey;            // нажать: мгновенно прогнать заряженные тики (рывок)
    // Хайдшоты: команда выстрела всегда уходит в чоке, следующим пакетом — углы AA. Сервер обновляет
    // анимацию и лагкомп по последней команде пачки: угол выстрела (голова вверх, взгляд на врага) никто не видит
    bool hideShots = false;
    KeyBindToggle hideShotsKey;
    // Авто хайдшоты: DT включён, а мы стоим / идём на shift / сидим — вместо быстрой стрельбы хайдшоты
    // (стоячий DT резольвят легче всего). В воздухе и на автопике — снова DT
    bool autoHideShots = false;
    bool autoHsRifles = true;       // разрешать для автоматов, ПП и пистолетов (иначе там остаётся DT)
    bool autoHsDeagle = true;       // для дигла
    bool autoHsAuto = false;        // для автоснайперок (SCAR/G3SG1): по умолчанию у них DT, вторая пуля добивает
    // Лаг-пик: на выглядывании держим команды в чоке, сервер (и враг) ещё видит нас за углом.
    // Выстрел отпускает пачку: сервер получает выход и выстрел за один свой тик
    bool lagPeek = false;
    KeyBind lagPeekKey;             // назначена — работает только пока зажата
    int lagPeekTicks = 14;          // максимум чока на выглядывании
    int lagPeekPredict = 6;         // за сколько тиков до «увидит» начинать следить (чок — за 2 тика)
};

enum EspFlag {
    FlagArmor = 1 << 0, FlagMoney = 1 << 1, FlagKit = 1 << 2, FlagScoped = 1 << 3, FlagReload = 1 << 4,
    FlagFlashed = 1 << 5, FlagBomb = 1 << 6, FlagDoubleTap = 1 << 7, FlagLagComp = 1 << 8, FlagResolver = 1 << 9,
    FlagLethal = 1 << 10, FlagAll = (1 << 11) - 1
};

struct ViewConfig {
    bool removeScopeZoom = true;    // прицел без приближения
    bool secondZoom = true;         // при «без зума»: второй уровень скопа всё же приближает
    float secondZoomFactor = 2.0f;  // во сколько раз
    bool showFakeAngles = true;     // в 3-м лице модель показывает то, что видит сервер
    bool serverModel = true;        // в 3-м лице поверх — модель, как её видит сервер (десинк, roll)
    float serverModelColor[4]{ 0.55f, 0.7f, 1.0f, 0.4f };
    bool backtrackChams = false;    // на враге — призрак его прошлой позы, в которую рейдж ещё может попасть
    bool backtrackChamsHidden = true;   // и сквозь стены
    float backtrackChamsColor[4]{ 0.85f, 0.55f, 1.0f, 0.35f };
    bool serverBox = true;          // на чоке (лаг-пик, фейклаг) — хитбокс там, где модель стоит на сервере
    float serverBoxColor[4]{ 1.0f, 0.45f, 0.35f, 0.9f };
    bool grenadePrediction = true;  // траектория гранаты в руке
    float dormantTime = 6.0f;       // сколько секунд держать ESP игрока, пропавшего из PVS
    bool syncRender = true;         // mat_queue_mode 0: кадр и ESP из одного тика, без дёрганья
    bool screenLogs = true;         // худ «Notifications»: попадания/промахи/урон по нам/evaded
    float logsX = 0.5f;             // где худ уведомлений: x — центр (в долях экрана; левее трети — по левому краю)
    float logsY = 0.68f;            // ...y — верх ленты; таскается мышью при открытом меню
    bool watermark = true;          // ватермарка NewerWin: пилюля сверху по центру
    bool hotkeys = true;            // панель «Hotkeys»: активные бинды (DT, хайдшоты, фейкдак...)
    float hotkeysX = 0.012f;        // позиция панели в долях экрана (таскается мышью при открытом меню)
    float hotkeysY = 0.42f;
    // HvH-визуалы (HvhVisuals.cpp)
    int espFlags = FlagAll;         // EspFlag: что писать справа от ESP врага
    bool aimDot = true;             // точка, куда рейдж может попасть
    bool tabLoadout = true;         // по Tab — снаряжение врагов
    bool damageNumbers = true;      // всплывающие цифры урона
    bool spreadCircle = false;      // круг разброса вокруг прицела
    bool footsteps = true;          // кольца там, где враг шумит
    bool killFlash = true;          // вспышка краёв экрана при убийстве
    bool grenadeTimers = true;      // над дымом и огнём — сколько им осталось
    bool shotCapsules = true;       // хитбоксы цели в момент выстрела (зелёные — попал, красные — мимо)
    bool impactSparks = false;      // искры там, куда пришла своя пуля
    float sparkColor[4]{ 1.0f, 0.62f, 0.2f, 1.0f };
    int sparkCount = 18;            // искр на одно попадание пули
    float sparkSize = 1.0f;         // множитель скорости разлёта / длины хвостов
    // 13.11 — новое (HvhVisuals.cpp, стиль Hud.h)
    bool offscreenArrows = true;    // стрелки вокруг прицела на врагов, которых нет на экране
    int arrowRadius = 190;          // радиус кольца стрелок, пиксели
    bool hitMarker = true;          // хитмаркер у прицела при попадании (красный — в голову)
    bool bulletTracers = true;      // линия своей пули до точки попадания
    float tracerColor[4]{ 0.30f, 0.49f, 0.96f, 1.0f };
    bool localTrail = false;        // светящийся след за собой
    float trailColor[4]{ 0.30f, 0.49f, 0.96f, 1.0f };
    bool grenadeWarning = true;     // метка на летящей вражеской гранате: тип, дистанция, красная — заденет
    bool zeusRange = true;          // с зевсом в руках — круг его дальности на земле
    bool seenWarning = true;        // «SEEN» под прицелом, когда нас видит враг
    bool espSmoothHealth = true;    // полоска хп врага убывает плавно, снятый урон на миг остаётся белым
    bool espAmmoBar = true;         // под рамкой врага — полоска патронов (при перезарядке — жёлтая)
    bool espDistance = false;       // под рамкой — дистанция в метрах
    bool espGlowBox = true;         // мягкое свечение вокруг рамки врага её же цветом
    bool espSoftFill = true;        // рамка врага залита мягким градиентом её же цвета (снизу плотнее, сверху пусто)
    // из hysteria (Extras.cpp)
    bool aspectRatio = false;
    float aspectRatioValue = 1.5f;  // 1.25 — 5:4, 1.33 — 4:3, 1.5 — 3:2, 1.6 — 16:10, 1.78 — 16:9
    bool viewmodel = false;
    int viewmodelFov = 68;
    float viewmodelX = 0.0f, viewmodelY = 0.0f, viewmodelZ = 0.0f;
    bool customScope = false;       // свой прицел снайпера: градиентные линии вместо чёрных
    int scopeSize = 40;
    int scopeGap = 10;
    bool scopeT = false;            // без верхней линии
    bool scopeInvert = false;       // «инвертор»: градиент наоборот — у центра прозрачно, к концу ярко
    int scopeThickness = 4;         // не используется (был клин инвертора), оставлено ради старых конфигов
    bool damageIndicator = false;   // мин. урон у прицела
    bool shotMarkers = false;       // метки в мире: куда попал / где пуля прошла мимо
    bool missMarkers = false;
    float shotMarkerTime = 2.0f;
    bool slowdownWarning = false;   // после попадания по нам скорость режется — показать насколько
    bool keybindList = false;       // активные бинды
    bool lcIndicator = false;       // окно дефенсива (лагкомп по нам не пишется)
    bool trashTalk = false;         // сообщение в чат при килле/смерти
    int trashTalkWhen = 3;          // 1 — при килле, 2 — при смерти
};

// Автострейф: «По камере» — летим туда, куда смотрим (S — назад), «WASD» — направление клавишами
enum class AutoStrafe { Off, Camera, Keys };
enum class AutoPeekMode { Shot, ShotOrRelease };

struct MovementConfig {
    KeyBind autoPeekKey;            // держать: запомнить точку, после выстрела вернуться
    int autoPeekMode = 0;           // AutoPeekMode: только после выстрела / ещё и когда отпустил WASD
    int peekTeleportTicks = 14;     // заряд под телепорт назад (больше «Тиков сдвига» DT — телепорт дальше)
    KeyBind fakeDuckKey;
    bool fastLadder = false;
    bool superToss = false;         // граната летит по прицелу с учётом своей скорости (бег/прыжок)
    bool noFallDamage = false;      // jumpbug в тик приземления: урон от падения не засчитывается
    // AI peek (при зажатом автопике и без WASD): ищет вокруг точку в паре шагов, откуда пробиваем
    // ближайшего к прицелу врага на мин. урон, выходит туда сам, после выстрела автопик возвращает
    bool aiPeek = false;
    int aiPeekQuality = 50;         // 30..100: сколько направлений проверять (8..16)
    int aiPeekSafety = 4;           // столько проверок подряд точка должна держаться, прежде чем пойти
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

// Вид мира: цвет стен/пропов/неба, свой туман, подсветка. Применяется в RENDER_START
// (перебор материалов раз в полсекунды и при смене настроек), при выключении — всё обратно
struct WorldConfig {
    bool enabled = false;
    int preset = 0;                 // последний выбранный пресет (только для меню)
    float wallColor[3]{ 1.0f, 1.0f, 1.0f };
    float propColor[3]{ 1.0f, 1.0f, 1.0f };
    float propAlpha = 1.0f;         // прозрачность пропов (ящики, машины)
    bool skyColorOn = false;
    float skyColor[3]{ 1.0f, 1.0f, 1.0f };
    bool fog = false;               // свой туман (fog_override)
    float fogColor[3]{ 0.6f, 0.6f, 0.65f };
    float fogStart = 0.0f;
    float fogEnd = 3000.0f;
    float fogDensity = 0.5f;
    bool ambient = false;           // подсветка (mat_ambient_light_*)
    float ambientColor[3]{ 0.05f, 0.05f, 0.1f };

    // погода: частицы вокруг камеры (рисуются оверлеем, под открытым небом)
    int weather = 0;                // 0 выкл, 1 снег, 2 дождь, 3 светлячки, 4 пепел
    int particles = 260;
    float particleColor[4]{ 1.0f, 1.0f, 1.0f, 0.85f };
    float particleSize = 1.0f;
    float particleRadius = 750.0f;

    // свет
    bool tonemap = false;           // свой блум и экспозиция (env_tonemap_controller карты)
    float bloom = 2.0f;
    float exposure = 1.0f;
    bool modelBrightness = false;   // r_modelAmbientMin — модели не тонут в темноте
    float modelAmbient = 1.0f;
    bool sunOverride = false;       // своё положение солнца: куда падают тени (cl_csm_rot_*)
    float sunPitch = 50.0f;         // высота солнца, 90 — в зените
    float sunYaw = 0.0f;            // сторона света
};

struct Config {
    WorldConfig world;
    ViewConfig view;
    RageConfig rage;
    AntiAimConfig antiAim;
    DefensiveConfig defensive;
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

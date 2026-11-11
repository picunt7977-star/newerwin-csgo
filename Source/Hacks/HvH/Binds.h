#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <InputUtil.h>
#include <JsonForward.h>

// Бинды на любую настройку меню (как в NL): ПКМ по строке → клавиша, режим и значение,
// которое настройка принимает, пока бинд активен. Отпустили — возвращается прежнее.
// Работают только для полей hvh::config: адрес поля хранится смещением от начала config,
// в файл конфига пишется вместе с id строки меню — если структура конфига поменялась между
// сборками, смещение перепривязывается, когда строка впервые нарисуется в меню.
namespace hvh::binds
{

enum class Type : std::uint8_t { Bool, Int, Float, Flags };
enum class Mode : std::uint8_t { Hold, Toggle, HoldOff };

struct Bind {
    std::uint32_t id = 0;        // строка меню + поле (одна строка «Хитшанс» — разные поля у разного оружия)
    std::uint32_t row = 0;       // ImGuiID строки меню
    std::string label;           // подпись строки — для панели хоткеев и проверки при загрузке
    Type type = Type::Bool;
    std::uint32_t offset = 0;    // смещение поля в hvh::config
    bool resolved = false;       // смещение подтверждено (та же сборка или строка уже рисовалась)
    KeyBind key;
    Mode mode = Mode::Hold;
    float value = 1.0f;          // значение на бинде (bool 0/1, int/флаги — целое)
    float base = 0.0f;           // значение до включения
    bool active = false;
    bool toggled = false;
};

// поле лежит внутри hvh::config — на него можно повесить бинд
[[nodiscard]] bool bindable(const void* field) noexcept;
[[nodiscard]] Bind* find(std::uint32_t id) noexcept;
// виджет рисуется — подтверждаем адрес поля (и создаём бинд, если create); row — ImGuiID строки
Bind* attach(std::uint32_t row, const char* label, Type type, void* field, bool create) noexcept;
void remove(std::uint32_t id) noexcept;
[[nodiscard]] std::vector<Bind>& all() noexcept;

[[nodiscard]] float read(const Bind& bind) noexcept;
void write(const Bind& bind, float value) noexcept;

// каждый кадр: клавиши → значения полей
void update() noexcept;
// сохранение конфига: на время записи вернуть настоящие значения, потом снова значения биндов
void suspend() noexcept;
void resume() noexcept;
// загрузка конфига: всё неактивно, список из файла
void reset() noexcept;

// старые клавиши-оверрайды из конфига → бинд на поле (смещение в config); строка меню
// подхватит его, когда нарисуется. Уже есть бинд на это поле или клавиши нет — ничего
void migrate(void* field, Type type, const KeyBind& key, Mode mode, float value, const char* label) noexcept;

json toJson() noexcept;
void fromJson(const json& j) noexcept;

[[nodiscard]] const char* modeName(Mode mode) noexcept;

}

// Офлайн-тест окна отката (Source/Hacks/HvH/BacktrackWindow.h): без игры.
// Сборка и запуск: ./tests/run_backtrack_test.sh
// Проверяет: свежая запись годна, край 0.17 с по «правильному» времени, обрезка задержки по sv_maxunlag,
// целый порог удаления записей floor(arrival − sv_maxunlag).

#include <cstdio>

#include <Hacks/HvH/BacktrackWindow.h>

using hvh::backtrack::recordValid;

namespace
{

int failures = 0;

void check(bool got, bool want, const char* name)
{
    if (got != want) {
        std::printf("ПРОВАЛ: %s (получили %d, ждали %d)\n", name, got, want);
        ++failures;
    } else {
        std::printf("ок: %s\n", name);
    }
}

}

int main()
{
    constexpr float unlag = 0.2f;

    // пинг 50 мс + lerp 30 мс: сервер откатит ровно на 0.08 с — запись этого момента годна
    check(recordValid(100.0f - 0.08f, 100.0f, 0.05f, 0.03f, unlag, 100.03f), true, "свежая запись");

    // отклонение от «правильного» времени: 0.165 с ещё можно, 0.175 с — уже нет
    check(recordValid(100.0f - 0.08f - 0.165f, 100.0f, 0.05f, 0.03f, unlag, 100.03f), true, "край окна внутри");
    check(recordValid(100.0f - 0.08f - 0.175f, 100.0f, 0.05f, 0.03f, unlag, 100.03f), false, "край окна снаружи");
    check(recordValid(100.0f - 0.08f + 0.175f, 100.0f, 0.05f, 0.03f, unlag, 100.03f), false, "запись из будущего");

    // пинг 500 мс: задержка обрезается до sv_maxunlag, откат не глубже 0.2 с
    check(recordValid(100.0f - 0.2f, 100.0f, 0.5f, 0.0f, unlag, 100.0f), true, "обрезка по sv_maxunlag");
    check(recordValid(100.0f - 0.5f, 100.0f, 0.5f, 0.0f, unlag, 100.0f), false, "старше sv_maxunlag при большом пинге");

    // порог удаления целый: floor(100.25 − 0.2) = 100 — запись 99.95 сервер уже выкинул, 100.0 ещё держит
    check(recordValid(99.95f, 100.05f, 0.0f, 0.0f, unlag, 100.25f), false, "удалена по целому порогу");
    check(recordValid(100.0f, 100.05f, 0.0f, 0.0f, unlag, 100.25f), true, "живёт до целого порога");

    std::printf(failures ? "\nПровалов: %d\n" : "\nВсё прошло\n", failures);
    return failures ? 1 : 0;
}

#include "Entity.h"
#include "GlobalVars.h"
#include "LocalPlayer.h"
#include "UserCmd.h"

namespace csgo
{

float GlobalVars::serverTime(UserCmd* cmd) const noexcept
{
    static int tick;
    static int lastTickBase = -1;

    // После прогона предсказания m_nTickBase — тик следующей команды. Если с прошлой команды он не
    // сдвинулся (несколько CL_Move за кадр, сдвиг DT), эта команда идёт следующей по тику. Раньше
    // тут было «++ пока прошлая команда не предсказана», и счётчик убегал на секунды вперёд —
    // рейдж стрелял, когда оружие на сервере ещё не готово, и браковал живые записи бектрека.
    if (cmd && localPlayer) {
        const int tickBase = localPlayer.get().tickBase();
        if (tickBase == lastTickBase && tick + 1 - tickBase <= 16)
            tick++;
        else
            tick = tickBase;
        lastTickBase = tickBase;
    }
    return tick * intervalPerTick;
}

}


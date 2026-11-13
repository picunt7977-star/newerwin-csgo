#pragma once

// Окно отката сервера — чистая функция без игры, покрыта тестом tests/backtrack_test.cpp

#include <algorithm>
#include <cmath>

namespace hvh::backtrack
{

// Окно отката сервера: цель не дальше 0.2 с от «правильного» времени и не старше sv_maxunlag
// (serverTime — наш тикбейс, то есть время выполнения команды на сервере)
// margin — запас у края sv_maxunlag: логи 04–09.11, бектрек 12+ тиков — 4/13 (30%) против 84–100% у 9–11:
// пока выстрел едет (чок + пинг), сервер успевает выкинуть запись с края окна
//
// 13.11, сверено с дизассемблером server_client.so (0x97ba.. — откат, 0x97c7f0 — запись в историю):
//   correct = clamp(GetLatency(FLOW_OUTGOING) + m_fLerpTime, 0, sv_maxunlag)   — задержка сервера до нас, это полный
//             круг (наши исходящая + входящая), а не одна входящая;
//   target  = cmd->tick_count · interval − m_fLerpTime;   |target − curtime + correct| > 0.2 → откат по пингу;
//   записи удаляются при simtime < (int)(curtime − sv_maxunlag) — порог ЦЕЛЫЙ, запись живёт 0.2–1.2 с.
// Раньше тут стояло «запись не старше sv_maxunlag от нашего тикбейса минус запас»: при пинге 50 мс свежая запись
// врага с фейклагом годилась только первые ~5 тиков из 14, остальное время рейдж стрелял прогнозом «вперёд»
// (логи 07–11.11: прогноз — больше половины выстрелов, попаданий 27% против 65% по настоящим записям).
// latency — наши исходящая + входящая; arrival — время сервера, когда он примет команду (для порога удаления).
[[nodiscard]] inline bool recordValid(float simulationTime, float serverTime, float latency, float lerp, float maxUnlag, float arrival) noexcept
{
    const float correct = std::clamp(latency + lerp, 0.0f, maxUnlag);
    // запас 30 мс: задержку сервер меряет сам, наша оценка дрожит (логи 04–09.11: на самом краю окна — 30% попаданий)
    if (std::abs(correct - (serverTime - simulationTime)) >= 0.17f)
        return false;
    return simulationTime >= std::floor(arrival - maxUnlag);
}

}

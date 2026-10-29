#pragma once

// Грань, о которой договорились: чит работает только в игре, запущенной с -insecure.
// С этим флагом движок сам не пускает на VAC-серверы, поэтому чит физически
// доступен только на insecure-серверах (хвх) и локально. Обхода VAC нет и не будет.

#include <Platform/Macros/IsPlatform.h>

#if IS_LINUX()
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#elif IS_WIN32() || IS_WIN64()
#include <cstring>
#include <Windows.h>
#endif

namespace insecure_gate
{

#if IS_LINUX()

// /proc/self/cmdline — аргументы, разделённые '\0'
inline bool hasInsecureArg() noexcept
{
    const int fd = ::open("/proc/self/cmdline", O_RDONLY);
    if (fd < 0)
        return false;

    char buf[8192];
    const auto len = ::read(fd, buf, sizeof(buf) - 1);
    ::close(fd);
    if (len <= 0)
        return false;
    buf[len] = '\0';

    for (const char* arg = buf; arg < buf + len; arg += std::strlen(arg) + 1) {
        if (std::strcmp(arg, "-insecure") == 0)
            return true;
    }
    return false;
}

// LD_PRELOAD цепляется ко всей цепочке запуска (reaper, csgo.sh и т.д.) — работаем только в самой игре
inline bool isGameProcess() noexcept
{
    char exe[512];
    const auto len = ::readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (len <= 0)
        return false;
    exe[len] = '\0';
    const char* name = std::strrchr(exe, '/');
    return name && std::strcmp(name + 1, "csgo_linux64") == 0;
}

#elif IS_WIN32() || IS_WIN64()

inline bool hasInsecureArg() noexcept
{
    const char* cmd = GetCommandLineA();
    for (const char* p = std::strstr(cmd, "-insecure"); p; p = std::strstr(p + 1, "-insecure")) {
        const bool startOk = p == cmd || p[-1] == ' ' || p[-1] == '"';
        const char end = p[9];
        if (startOk && (end == '\0' || end == ' ' || end == '"'))
            return true;
    }
    return false;
}

inline bool isGameProcess() noexcept { return true; }

#endif

inline bool allowed() noexcept
{
    return hasInsecureArg();
}

}

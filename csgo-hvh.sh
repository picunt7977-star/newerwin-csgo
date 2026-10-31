#!/usr/bin/env bash
# Сборка и подсказка по запуску newerwin (CS:GO legacy, Linux, только -insecure)
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
LIB="$ROOT/build/Source/libOsiris.so"

case "${1:-build}" in
    build)
        cmake -D CMAKE_BUILD_TYPE=Release -D CMAKE_CXX_COMPILER=clang++ -D CMAKE_C_COMPILER=clang -S "$ROOT" -B "$ROOT/build" -G Ninja >/dev/null
        cmake --build "$ROOT/build" -j"$(nproc)"
        echo
        echo "Готово: $LIB"
        ;&
    launch-options)
        echo
        echo "Steam → CS:GO (legacy) → Свойства → Параметры запуска:"
        echo
        echo "  SDL_VIDEODRIVER=x11 LD_PRELOAD=\"\$LD_PRELOAD:$LIB\" %command% -insecure"
        echo
        echo "Без -insecure чит не загрузится (в консоли игры будет сообщение)."
        ;;
    *)
        echo "использование: $0 [build|launch-options]"
        exit 1
        ;;
esac

#!/bin/sh
# Офлайн-тест окна отката: собрать и запустить (игра не нужна)
set -e
cd "$(dirname "$0")/.."
mkdir -p build/tests
clang++ -std=gnu++20 -O3 -I Source -I Dependencies tests/backtrack_test.cpp -o build/tests/backtrack_test
./build/tests/backtrack_test

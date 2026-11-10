#!/bin/sh
# Офлайн-тест контр-резольвера AA: собрать и запустить (игра не нужна)
set -e
cd "$(dirname "$0")/.."
mkdir -p build/tests
clang++ -std=gnu++20 -O2 -I Source -I Dependencies tests/counter_test.cpp -o build/tests/counter_test 2>/dev/null
./build/tests/counter_test

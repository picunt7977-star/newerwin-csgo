#!/bin/sh
# Офлайн-тест модели попаданий: собрать и запустить (игра не нужна)
set -e
cd "$(dirname "$0")/.."
mkdir -p build/tests
clang++ -std=gnu++20 -O3 -I Source -I Dependencies tests/hitmodel_test.cpp -o build/tests/hitmodel_test
./build/tests/hitmodel_test

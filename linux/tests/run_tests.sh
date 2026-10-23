#!/bin/bash
# Сборка и запуск тестов ygg-watchdog (без сети и без Yggdrasil)
set -e
cd "$(dirname "$(readlink -f "$0")")"
g++ -std=c++17 -O1 -Wall -Wextra -I../src -o tests tests.cpp -lssl -lcrypto -pthread
./tests

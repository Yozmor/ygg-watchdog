#!/bin/bash
# Сборка ygg-watchdog для Linux. Нужны g++ и заголовки OpenSSL:
#   sudo apt install g++ libssl-dev
# Готовая программа - ./ygg-watchdog рядом с этим файлом.
set -e
cd "$(dirname "$(readlink -f "$0")")"
if ! command -v g++ >/dev/null; then echo "Нет g++: sudo apt install g++ libssl-dev"; exit 1; fi
if [ ! -f /usr/include/openssl/ssl.h ]; then echo "Нет заголовков OpenSSL: sudo apt install libssl-dev"; exit 1; fi
echo "Собираю ygg-watchdog..."
g++ -std=c++17 -O2 -Wall -Wextra -Isrc -o ygg-watchdog.build src/watchdog/main.cpp \
    -static-libstdc++ -static-libgcc -lssl -lcrypto -pthread
mv -f ygg-watchdog.build ygg-watchdog
echo "Готово: $(pwd)/ygg-watchdog ($(./ygg-watchdog --selftest))"

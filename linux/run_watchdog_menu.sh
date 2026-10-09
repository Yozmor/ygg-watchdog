#!/bin/bash
# run_watchdog_menu.sh
#
# Запускает интерактивное меню watchdog. Сам находит свою папку через
# dirname, поэтому работает независимо от того, куда его перенесли -
# не завязан на конкретный путь вроде ~/ygg-watchdog.

cd "$(dirname "$(readlink -f "$0")")" || exit 1

sudo python3 watchdog_daemon.py menu

echo
read -p "Нажми Enter, чтобы закрыть окно..."

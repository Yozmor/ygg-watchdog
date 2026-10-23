#!/bin/bash
# Установка ygg-watchdog на Linux:
#   sudo ./install.sh            - включить автопроверку (systemd-таймер раз в 3 минуты) и команду ygg-watchdog
#   sudo ./install.sh --remove   - выключить (файлы в папке остаются)
# Программа работает прямо из этой папки. Всё остальное делает она сама
# (ygg-watchdog install / uninstall) - этот файл только проверяет, что она на месте.
set -e
DIR="$(cd "$(dirname "$(readlink -f "$0")")" && pwd)"
if [ "$(id -u)" -ne 0 ]; then echo "Нужны права root: sudo $0 $*"; exit 1; fi
[ -x "$DIR/ygg-watchdog" ] || chmod +x "$DIR/ygg-watchdog" 2>/dev/null || true
if [ ! -x "$DIR/ygg-watchdog" ] || ! "$DIR/ygg-watchdog" --selftest >/dev/null 2>&1; then
    echo "ygg-watchdog не запускается. Собери его: ./build.sh (нужны g++ и libssl-dev)"
    exit 1
fi
command -v yggdrasilctl >/dev/null || echo "Внимание: не найден yggdrasilctl - сначала установи Yggdrasil."
if [ "$1" = "--remove" ]; then exec "$DIR/ygg-watchdog" uninstall; fi
# остатки Python-версии (старые файлы рядом не мешают, но больше не нужны)
for f in watchdog_daemon.py watchdog_core.py peer_database.py geoip.py device_monitor.py battery.py vpn_hook.py \
         auto_update.py selftest.py yggdrasil_menu.py run_watchdog_menu.sh launch_in_terminal.sh \
         ygg-watchdog-tick.service ygg-watchdog-tick.timer; do
    [ -f "$DIR/$f" ] && rm -f "$DIR/$f" && echo "убран старый файл Python-версии: $f"
done
rm -rf "$DIR/__pycache__"
"$DIR/ygg-watchdog" install
echo
echo "Готово. Окно: ygg-watchdog   Лог: tail -f $DIR/watchdog.log"
[ -f "$DIR/notify.conf" ] || echo "Telegram: ygg-watchdog → «Управление» → [n] (необязательно)"

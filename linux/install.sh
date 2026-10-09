#!/bin/bash
# Установка ygg-watchdog на Linux.
#   sudo ./install.sh            - установить или обновить
#   sudo ./install.sh --remove   - удалить (таймер и файлы программы)
#
# Программа копируется в /opt/ygg-watchdog, systemd-таймер настраивается
# на этот путь и включается. При обновлении notify.conf, state.json и
# логи сохраняются.
set -e

DEST="/opt/ygg-watchdog"
SRC="$(cd "$(dirname "$0")" && pwd)"
UNIT_DIR="/etc/systemd/system"

if [ "$(id -u)" -ne 0 ]; then
    echo "Нужны права root: sudo $0 $*"
    exit 1
fi

if [ "$1" = "--remove" ]; then
    systemctl disable --now ygg-watchdog-tick.timer 2>/dev/null || true
    rm -f "$UNIT_DIR/ygg-watchdog-tick.service" "$UNIT_DIR/ygg-watchdog-tick.timer"
    systemctl daemon-reload
    rm -rf "$DEST"
    echo "ygg-watchdog удалён."
    exit 0
fi

command -v python3 >/dev/null || { echo "Не найден python3. Установи его: sudo apt install python3"; exit 1; }
command -v yggdrasilctl >/dev/null || echo "Внимание: не найден yggdrasilctl. Сначала установи Yggdrasil."

mkdir -p "$DEST"
for f in peer_database.py watchdog_core.py watchdog_daemon.py yggdrasil_menu.py selftest.py \
         run_watchdog_menu.sh launch_in_terminal.sh cities.dat notify.conf.example README_LINUX.md; do
    cp "$SRC/$f" "$DEST/"
done
chmod +x "$DEST"/*.py "$DEST"/*.sh

# systemd-юниты с правильным путём
sed "s#/opt/ygg-watchdog#$DEST#g" "$SRC/ygg-watchdog-tick.service" > "$UNIT_DIR/ygg-watchdog-tick.service"
cp "$SRC/ygg-watchdog-tick.timer" "$UNIT_DIR/ygg-watchdog-tick.timer"
systemctl daemon-reload
systemctl enable --now ygg-watchdog-tick.timer

# короткая команда для меню
ln -sf "$DEST/run_watchdog_menu.sh" /usr/local/bin/ygg-watchdog

echo
echo "Готово. Программа установлена в $DEST, проверка идёт каждые 3 минуты."
echo "  Меню:          ygg-watchdog"
echo "  Самопроверка:  sudo python3 $DEST/selftest.py"
echo "  Лог:           tail -f $DEST/watchdog.log"
if [ ! -f "$DEST/notify.conf" ]; then
    echo "  Telegram:      скопируй $DEST/notify.conf.example в notify.conf и впиши токен (необязательно)"
fi

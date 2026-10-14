#!/bin/bash
# launch_in_terminal.sh
#
# Сам находит доступный терминальный эмулятор и запускает в нём
# run_watchdog_menu.sh - в обход автоопределения самого GNOME (которое,
# похоже, не настроено на этой системе - Terminal=true в .desktop
# просто ничего не находит и окно мгновенно закрывается).

SCRIPT_DIR="$(dirname "$(readlink -f "$0")")"
TARGET="$SCRIPT_DIR/run_watchdog_menu.sh"

if [ ! -x "$TARGET" ]; then
    echo "Не найден или не исполняемый: $TARGET" >&2
    exit 1
fi

for term in gnome-terminal konsole xfce4-terminal mate-terminal x-terminal-emulator xterm; do
    if command -v "$term" >/dev/null 2>&1; then
        case "$term" in
            gnome-terminal|mate-terminal)
                exec "$term" -- "$TARGET"
                ;;
            konsole|xfce4-terminal)
                exec "$term" -e "$TARGET"
                ;;
            x-terminal-emulator|xterm)
                exec "$term" -e "$TARGET"
                ;;
        esac
    fi
done

# Ни один терминал не найден - показываем это хотя бы через zenity/notify,
# если они есть, иначе просто пишем в файл, раз показать пользователю
# уже нечем (окно .desktop и так закроется мгновенно без терминала).
echo "Не найден ни один терминальный эмулятор (gnome-terminal, konsole, xterm...)" >&2
if command -v zenity >/dev/null 2>&1; then
    zenity --error --text="Не найден ни один терминальный эмулятор.\nУстанови gnome-terminal или xterm."
fi
exit 1

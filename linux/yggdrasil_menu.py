#!/usr/bin/env python3
# -*- coding: utf-8 -*-
#
# Консольная утилита-меню для управления Yggdrasil на Linux.
# Не требует компиляции - запускается напрямую python3 (или chmod +x
# и запуск как обычный исполняемый скрипт).
#
# Умеет: перехватывать вывод yggdrasilctl, разбирать его таблицы
# (peers/sessions/paths) и перерисовывать их компактно с нумерацией
# строк - в том числе для удобного удаления пира по номеру.

import os
import re
import shutil
import subprocess
import time
import sys

# ------------------------------------------------------------------
# НАСТРОЙКИ - поправь под свою систему при необходимости
# ------------------------------------------------------------------
YGGCTL_CANDIDATES = ["yggdrasilctl", "/usr/bin/yggdrasilctl", "/usr/local/bin/yggdrasilctl"]
CONFIG_PATH = "/etc/yggdrasil/yggdrasil.conf"
SERVICE_NAME = "yggdrasil"

# Какие столбцы показывать в удобных таблицах и какой у них максимум
# ширины (более длинные значения обрезаются с многоточием). Названия
# должны совпадать (без учёта регистра) с заголовками, которые печатает
# сам yggdrasilctl - если в новой версии Yggdrasil что-то переименуют,
# соответствующий столбец просто тихо пропадёт из вывода, не сломав
# программу (есть fallback на сырой вывод).
PEERS_COLUMNS = [
    ("URI", 42),
    ("State", 5),
    ("Dir", 4),
    ("IP Address", 40),
    ("Uptime", 7),
    ("RTT", 8),
    ("RX", 7),
    ("TX", 7),
    ("Cost", 5),
    ("Last Error", 60),
]

SESSIONS_COLUMNS = [
    ("Public Key", 28),
    ("IP Address", 40),
    ("Uptime", 7),
    ("RX", 7),
    ("TX", 7),
]

PATHS_COLUMNS = [
    ("Public Key", 28),
    ("IP Address", 40),
    ("Path", 30),
    ("Coords", 30),
    ("Sequence", 10),
    ("Cost", 5),
]


def find_yggctl():
    for candidate in YGGCTL_CANDIDATES:
        found = shutil.which(candidate) or (candidate if os.path.isfile(candidate) else None)
        if found:
            return found
    return None


YGGCTL = find_yggctl()


def ensure_root():
    if os.geteuid() != 0:
        print("Требуются права администратора, перезапуск через sudo...")
        try:
            os.execvp("sudo", ["sudo", sys.executable] + sys.argv)
        except FileNotFoundError:
            print("Не найдена команда sudo. Запусти скрипт от root вручную:")
            print(f"  sudo python3 {os.path.abspath(sys.argv[0])}")
            sys.exit(1)


def clear():
    os.system("clear")


def pause():
    print()
    input("Нажми Enter, чтобы продолжить...")


def run_passthrough(args):
    """Запуск команды с выводом напрямую в этот же терминал."""
    try:
        subprocess.run(args)
    except FileNotFoundError:
        print(f"Не удалось запустить: {args[0]}")


def run_ygg(args):
    if not YGGCTL:
        print("yggdrasilctl не найден. Проверь установку Yggdrasil.")
        return
    run_passthrough([YGGCTL] + args)


def run_and_capture(args):
    """Запуск команды с перехватом вывода (для разбора таблиц)."""
    try:
        result = subprocess.run(
            args, capture_output=True, text=True, encoding="utf-8", errors="replace"
        )
        return (result.stdout or "") + (result.stderr or "")
    except FileNotFoundError:
        return ""


def capture_ygg(args):
    if not YGGCTL:
        return ""
    return run_and_capture([YGGCTL] + args)


# ------------------------------------------------------------------
# Разбор таблиц. yggdrasilctl в разных версиях печатает таблицы
# по-разному: либо символами рамки (│ ─ ┌ ...), либо простыми
# колонками с выравниванием пробелами. Поддерживаем оба варианта,
# при неудаче безопасно откатываемся на показ сырого текста.
# ------------------------------------------------------------------
BORDER_CHARS = set("─┌┐└┘├┤┬┴┼ ")


def is_border_line(line):
    if not line:
        return False
    return all(c in BORDER_CHARS for c in line)


def parse_box_table(text):
    headers = []
    rows = []
    for raw_line in text.splitlines():
        line = raw_line.strip()
        if not line or is_border_line(line):
            continue
        if "│" not in line:
            continue
        cells = [c.strip() for c in line.split("│")]
        if cells and cells[0] == "":
            cells = cells[1:]
        if cells and cells[-1] == "":
            cells = cells[:-1]
        if not headers:
            headers = cells
        elif len(cells) == len(headers):
            rows.append(cells)
    return headers, rows


def parse_plain_table(text):
    """Резервный разбор для старого формата с колонками через пробелы.
    Делим по двум и более пробелам подряд - обычно этого достаточно,
    чтобы не разрезать значения вроде 'no such host' на части."""
    lines = [l for l in text.splitlines() if l.strip()]
    if not lines:
        return [], []

    header_line = lines[0]
    headers = [h for h in re.split(r"\s{2,}", header_line.strip()) if h]
    if len(headers) < 2:
        return [], []

    rows = []
    for line in lines[1:]:
        if is_border_line(line.strip()):
            continue
        cells = [c for c in re.split(r"\s{2,}", line.strip()) if c]
        if len(cells) == len(headers):
            rows.append(cells)
    return headers, rows


def parse_ygg_table(text):
    if "│" in text:
        headers, rows = parse_box_table(text)
        if headers and rows:
            return headers, rows
    return parse_plain_table(text)


def find_header_index(headers, name):
    name_lower = name.lower()
    for i, h in enumerate(headers):
        if h.lower() == name_lower:
            return i
    return -1


def truncate_cell(s, max_len):
    if len(s) <= max_len:
        return s
    if max_len <= 3:
        return s[:max_len]
    return s[: max_len - 3] + "..."


def print_friendly_table(headers, rows, columns_spec):
    idx = []
    disp_headers = []
    max_widths = []

    for name, width in columns_spec:
        i = find_header_index(headers, name)
        if i >= 0:
            idx.append(i)
            disp_headers.append(name)
            max_widths.append(width)

    if not idx:
        return False

    num_len = max(1, len(str(len(rows))))

    widths = []
    for c, i in enumerate(idx):
        w = len(disp_headers[c])
        for row in rows:
            cell = truncate_cell(row[i], max_widths[c])
            w = max(w, len(cell))
        widths.append(w)

    def sep():
        parts = ["+" + "-" * (num_len + 2)]
        for w in widths:
            parts.append("-" * (w + 2) + "+")
        print("+" + "-" * (num_len + 2) + "+" + "+".join("-" * (w + 2) for w in widths) + "+")

    def pad(s, w):
        return s + " " * (w - len(s))

    sep()
    print("| " + pad("#", num_len) + " |" + "".join(
        " " + pad(disp_headers[c], widths[c]) + " |" for c in range(len(idx))
    ))
    sep()
    for r, row in enumerate(rows):
        line = "| " + pad(str(r + 1), num_len) + " |"
        for c, i in enumerate(idx):
            cell = truncate_cell(row[i], max_widths[c])
            line += " " + pad(cell, widths[c]) + " |"
        print(line)
    sep()
    return True


def show_friendly_table_or_raw(args, columns_spec):
    output = capture_ygg(args)
    if not output.strip():
        print("Не удалось получить ответ от yggdrasilctl.")
        return

    headers, rows = parse_ygg_table(output)

    if not headers:
        print(output, end="")
        return
    if not rows:
        print("Список пуст.")
        return

    if not print_friendly_table(headers, rows, columns_spec):
        print(output, end="")


# ------------------------------------------------------------------
# Меню
# ------------------------------------------------------------------
def show_menu():
    clear()
    print("================================================")
    print("           YGGDRASIL - ПАНЕЛЬ УПРАВЛЕНИЯ")
    print("================================================")
    print()
    print("  1  - Статус узла: getSelf")
    print("  2  - Список пиров: getPeers")
    print("  3  - Дерево маршрутов: getTree")
    print("  4  - Активные сессии: getSessions")
    print("  5  - Таблица путей: getPaths")
    print("  6  - Добавить пира вручную: addPeer")
    print("  7  - Удалить пира по номеру: removePeer")
    print()
    print("  8  - Перезапустить службу Yggdrasil")
    print("  9  - Остановить службу Yggdrasil")
    print("  10 - Запустить службу Yggdrasil")
    print("  11 - Статус службы")
    print()
    print("  12 - Посмотреть лог Yggdrasil (journalctl)")
    print("  13 - Посмотреть / отредактировать конфиг")
    print()
    print("  14 - Список всех доступных команд: list")
    print("  15 - Сырой JSON-ответ на произвольную команду (для продвинутых)")
    print("  16 - Отложенный перезапуск службы (если подключён только по Yggdrasil)")
    print()
    print("  0  - Выход")
    print()
    print("================================================")


def do_remove_peer():
    clear()
    print("=== Удалить пира ===\n")

    output = capture_ygg(["getPeers"])
    headers, rows = parse_ygg_table(output)
    uri_idx = find_header_index(headers, "URI")
    state_idx = find_header_index(headers, "State")

    if not headers or uri_idx < 0 or not rows:
        print("Не удалось получить список пиров для выбора.")
        if output.strip():
            print(output, end="")
        pause()
        return

    for i, row in enumerate(rows):
        state = row[state_idx] if state_idx >= 0 else "?"
        print(f"  {i + 1}) [{state}] {row[uri_idx]}")

    print()
    choice = input("Номер пира для удаления (0 - отмена): ").strip()
    try:
        num = int(choice)
    except ValueError:
        num = 0

    if 1 <= num <= len(rows):
        uri = rows[num - 1][uri_idx]
        print(f"Удаляю: {uri}")
        run_ygg(["removePeer", f"uri={uri}"])
    else:
        print("Отмена.")
    pause()


def do_view_log():
    clear()
    print("=== Лог Yggdrasil (journalctl) ===\n")
    run_passthrough(["journalctl", "-u", SERVICE_NAME, "-n", "100", "--no-pager"])
    print()
    print("Полный лог с прокруткой: journalctl -u yggdrasil -f")
    pause()


def do_view_config():
    clear()
    print("=== Конфиг Yggdrasil ===\n")
    print(f"Путь: {CONFIG_PATH}\n")

    if not os.path.isfile(CONFIG_PATH):
        print("Файл не найден по этому пути.")
        pause()
        return

    print("1 - Посмотреть содержимое (cat)")
    print("2 - Открыть в редакторе (nano)")
    print("0 - Назад")
    choice = input("Выбор: ").strip()

    if choice == "1":
        run_passthrough(["cat", CONFIG_PATH])
        pause()
    elif choice == "2":
        editor = os.environ.get("EDITOR", "nano")
        run_passthrough([editor, CONFIG_PATH])


def main():
    ensure_root()

    if not YGGCTL:
        print("Не найден yggdrasilctl в системе.")
        print("Проверь, что Yggdrasil установлен, и поправь YGGCTL_CANDIDATES")
        print("в начале скрипта, если он лежит в нестандартном месте.")
        pause()
        sys.exit(1)

    while True:
        show_menu()
        choice = input("Выбери пункт меню и нажми Enter: ").strip()

        if choice == "0":
            break

        elif choice == "1":
            clear()
            print("=== Статус узла ===\n")
            run_ygg(["getSelf"])
            pause()

        elif choice == "2":
            clear()
            print("=== Список пиров ===\n")
            show_friendly_table_or_raw(["getPeers"], PEERS_COLUMNS)
            pause()

        elif choice == "3":
            clear()
            print("=== Дерево маршрутов ===\n")
            run_ygg(["getTree"])
            print()
            print("Если команда не распознана - в этой версии Yggdrasil её")
            print("могли снова переименовать. Смотри пункт 14 (list).")
            pause()

        elif choice == "4":
            clear()
            print("=== Активные сессии ===\n")
            show_friendly_table_or_raw(["getSessions"], SESSIONS_COLUMNS)
            pause()

        elif choice == "5":
            clear()
            print("=== Таблица путей ===\n")
            show_friendly_table_or_raw(["getPaths"], PATHS_COLUMNS)
            pause()

        elif choice == "6":
            clear()
            print("=== Добавить пира ===\n")
            print("Введи URI пира, например tcp://1.2.3.4:12345")
            uri = input("URI: ").strip()
            if uri:
                run_ygg(["addPeer", f"uri={uri}"])
            pause()

        elif choice == "7":
            do_remove_peer()

        elif choice == "8":
            clear()
            print("=== Перезапуск службы Yggdrasil ===\n")
            print("Внимание: если ты подключён СЕЙЧАС именно через Yggdrasil")
            print("(а не через локальную сеть) - соединение на секунду прервётся.")
            print("Если это твой единственный канал до сервера - используй")
            print("пункт 16 (отложенный перезапуск) вместо этого.\n")
            run_passthrough(["systemctl", "restart", SERVICE_NAME])
            print("\nПроверяю, что служба реально поднялась:\n")
            run_passthrough(["systemctl", "status", SERVICE_NAME, "--no-pager", "-l"])
            pause()

        elif choice == "9":
            clear()
            print("=== Остановка службы Yggdrasil ===\n")
            run_passthrough(["systemctl", "stop", SERVICE_NAME])
            pause()

        elif choice == "10":
            clear()
            print("=== Запуск службы Yggdrasil ===\n")
            run_passthrough(["systemctl", "start", SERVICE_NAME])
            pause()

        elif choice == "11":
            clear()
            print("=== Статус службы ===\n")
            run_passthrough(["systemctl", "status", SERVICE_NAME, "--no-pager"])
            pause()

        elif choice == "12":
            do_view_log()

        elif choice == "13":
            do_view_config()

        elif choice == "14":
            clear()
            print("=== Список всех доступных команд ===\n")
            run_ygg(["list"])
            pause()

        elif choice == "15":
            clear()
            print("=== Сырой JSON-ответ на произвольную команду ===\n")
            print("Эта опция для продвинутых пользователей: она отправляет")
            print("yggdrasilctl произвольную команду с флагом -json и печатает")
            print("ответ в точности так, как его отдаёт сама программа - без")
            print("какого-либо форматирования и упрощений, как в остальных")
            print("пунктах меню. В обычной работе это не нужно.")
            print()
            print("Название команды бери из пункта 14 (список всех команд).")
            print()
            cmd = input("Команда (например getSelf): ").strip()
            if cmd:
                run_ygg(["-json", cmd])
            pause()

        elif choice == "16":
            clear()
            print("=== Отложенный перезапуск службы Yggdrasil ===\n")
            print("Используй это, если сейчас подключён к серверу ТОЛЬКО через")
            print("Yggdrasil (например ноут за NAT) - обычный перезапуск оборвёт")
            print("твоё же соединение прямо в процессе выполнения команды.\n")
            print("Команда планируется через systemd-run и выполнится через")
            print("заданную задержку - у тебя будет время на то, чтобы команда")
            print("успела уйти по ещё живому каналу, а сам перезапуск произойдёт")
            print("уже после этого. Просто подожди указанное время и переподключись.\n")

            delay_str = input("Задержка в секундах (по умолчанию 10): ").strip()
            delay = int(delay_str) if delay_str.isdigit() else 10

            unit_name = f"ygg-watchdog-delayed-restart-{int(time.time())}"
            run_passthrough([
                "systemd-run",
                f"--unit={unit_name}",
                f"--on-active={delay}s",
                "systemctl", "restart", SERVICE_NAME,
            ])
            print(f"\nЗапланировано. Служба перезапустится через {delay} секунд.")
            print("Если соединение сейчас идёт через Yggdrasil - оно скоро оборвётся")
            print("и восстановится через несколько секунд после перезапуска.")
            pause()

        else:
            print("Неверный пункт меню.")
            pause()


if __name__ == "__main__":
    main()

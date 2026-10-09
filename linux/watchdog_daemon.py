#!/usr/bin/env python3
"""
watchdog_daemon.py

CLI-обвязка над watchdog_core: подкоманды для планировщика (tick) и для
ручного управления региональными пирами (add-region/remove-region), плюс
запись событий в файл-очередь для последующей доставки любым каналом
уведомлений (телефон, email, что угодно - этот файл не знает и не должен
знать, как события кому-то доставляются).

Использование:
    sudo python3 watchdog_daemon.py tick
    sudo python3 watchdog_daemon.py add-region nepal
    sudo python3 watchdog_daemon.py add-region nepal india   (несколько сразу)
    sudo python3 watchdog_daemon.py remove-region
    sudo python3 watchdog_daemon.py status
"""

import argparse
import json
import logging
import logging.handlers
import os
import socket
import subprocess
import sys
import time
import traceback
import urllib.error
import urllib.parse
import urllib.request
from datetime import datetime, timezone
from pathlib import Path
from typing import Optional

import peer_database as pd
import watchdog_core as wc

logger = logging.getLogger("ygg_watchdog.daemon")

_USER_QUEUE_PATH = Path.home() / ".cache" / "ygg-watchdog" / "events_queue.jsonl"
_resolved_queue_path = None


def _get_queue_path() -> Path:
    global _resolved_queue_path
    if _resolved_queue_path is not None:
        return _resolved_queue_path
    candidates = (wc._project_dir() / "events_queue.jsonl", wc._default_system_dir() / "events_queue.jsonl", _USER_QUEUE_PATH)
    for candidate in candidates:
        try:
            candidate.parent.mkdir(parents=True, exist_ok=True)
            test_path = candidate.parent / ".write_test"
            test_path.write_text("")
            test_path.unlink()
            _resolved_queue_path = candidate
            return candidate
        except OSError:
            continue
    fallback = Path("/tmp/ygg-watchdog/events_queue.jsonl") if sys.platform != "win32" else Path(os.environ.get("TEMP", r"C:\Temp")) / "ygg-watchdog" / "events_queue.jsonl"
    fallback.parent.mkdir(parents=True, exist_ok=True)
    _resolved_queue_path = fallback
    return fallback


# ------------------------------------------------------------------
# Уведомления в Telegram - настраиваются через notify.conf рядом со
# скриптом (BOT_TOKEN=...\nCHAT_ID=...). Если файла нет или он неполный -
# уведомления просто молча выключены, ничего не ломается. Имя устройства
# в каждом сообщении - то же самое socket.gethostname(), что уже
# используется для записей в очередь событий (единая логика).
# ------------------------------------------------------------------
_NOTIFY_CONFIG_CACHE: Optional[dict] = None

# Пуш в Telegram отправляется только для этих трёх типов событий - все
# остальные (обычные "без изменений", региональные и т.д.) остаются
# только в логе/очереди, чтобы не спамить.
_NOTIFY_KINDS = {"backup_added", "backup_removed", "internet_restored"}


def _load_notify_config() -> Optional[dict]:
    global _NOTIFY_CONFIG_CACHE
    if _NOTIFY_CONFIG_CACHE is not None:
        return _NOTIFY_CONFIG_CACHE or None

    path = Path(__file__).resolve().parent / "notify.conf"
    if not path.exists():
        _NOTIFY_CONFIG_CACHE = {}
        return None

    config: dict = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, _, value = line.partition("=")
        config[key.strip()] = value.strip()

    if "BOT_TOKEN" not in config or "CHAT_ID" not in config:
        logger.debug("notify.conf найден, но нет BOT_TOKEN/CHAT_ID - уведомления выключены")
        _NOTIFY_CONFIG_CACHE = {}
        return None

    _NOTIFY_CONFIG_CACHE = config
    return config


def send_telegram_notification(message: str) -> bool:
    """Отправляет сообщение в Telegram, если notify.conf настроен.
    Молча ничего не делает, если конфига нет - это штатное отключённое
    состояние, не ошибка."""
    config = _load_notify_config()
    if config is None:
        return False

    device = socket.gethostname()
    full_message = f"[{device}] {message}"

    url = f"https://api.telegram.org/bot{config['BOT_TOKEN']}/sendMessage"
    data = urllib.parse.urlencode({
        "chat_id": config["CHAT_ID"],
        "text": full_message,
    }).encode("utf-8")

    try:
        req = urllib.request.Request(url, data=data, method="POST")

        proxy = config.get("PROXY")
        if proxy:
            # Явный прокси из notify.conf - в обход системных настроек
            # окружения (актуально, если сервис запускается в контексте,
            # где переменные http_proxy/https_proxy текущего пользователя
            # не видны - например через systemd без их явной передачи).
            proxy_handler = urllib.request.ProxyHandler({"https": proxy, "http": proxy})
            opener = urllib.request.build_opener(proxy_handler)
            resp = opener.open(req, timeout=10)
        else:
            resp = urllib.request.urlopen(req, timeout=10)

        with resp:
            return resp.status == 200
    except urllib.error.HTTPError as e:
        # Telegram обычно присылает понятное текстовое описание причины
        # (например "Unauthorized", "chat not found") прямо в теле ответа -
        # это и есть самая полезная диагностика при неверном токене/chat_id.
        body = e.read().decode("utf-8", errors="replace")
        logger.warning("Не удалось отправить уведомление в Telegram: %s %s - %s", e.code, e.reason, body)
        return False
    except Exception as e:
        logger.warning("Не удалось отправить уведомление в Telegram: %s", e)
        return False


def notify_relevant_events(events: list[wc.Event]) -> None:
    for event in events:
        if event.kind in _NOTIFY_KINDS:
            send_telegram_notification(event.message)


def append_events(events: list[wc.Event]) -> None:
    """Дописывает события в файл-очередь построчным JSON, и заодно
    отправляет пуш в Telegram для тех событий, которые того стоят (см.
    notify_relevant_events) - один центральный пункт для обоих действий,
    чтобы не дублировать вызов в каждой команде отдельно."""
    if not events:
        return

    notify_relevant_events(events)

    queue_path = _get_queue_path()
    device_name = socket.gethostname()
    now = datetime.now(timezone.utc).astimezone()  # локальное время с таймзоной

    with queue_path.open("a", encoding="utf-8") as f:
        for event in events:
            record = {
                "timestamp": now.isoformat(),
                "device": device_name,
                "kind": event.kind,
                "message": event.message,
            }
            f.write(json.dumps(record, ensure_ascii=False) + "\n")
            logger.info("Событие записано в очередь: %s - %s", event.kind, event.message)


def cmd_tick(args: argparse.Namespace) -> int:
    config_path = wc.find_config_path()
    if config_path is None:
        logger.error("Не найден yggdrasil.conf ни по одному из известных путей.")
        print("Не найден yggdrasil.conf ни по одному из известных путей.", file=sys.stderr)
        return 1

    state = wc.load_state()
    events = wc.tick(config_path, state)
    wc.save_state(state)
    append_events(events)

    if events:
        for e in events:
            print(f"[{e.kind}] {e.message}")
    else:
        logger.info("Проверка выполнена: без изменений (режим=%s)", state.mode)
        print("Без изменений.")
    return 0


def cmd_add_region_worker(args: argparse.Namespace) -> int:
    """Внутренняя команда - реальная работа (поиск + настоящее рукопожатие +
    добавление). Запускается в ОТДЕЛЬНОМ процессе, отсоединённом от
    родителя (start_new_session=True) - поэтому интерактивный вызов
    add-region не блокируется на минуты, пока идёт проверка кандидатов."""
    state = wc.load_state()

    country_slug = _resolve_country_input(args.country)
    if country_slug is None:
        logger.error("Не удалось определить страну по вводу '%s'", args.country)
        return 1

    spec = country_slug
    if args.city:
        iso = pd.slug_to_iso(country_slug)
        if iso:
            city_obj = _resolve_city_input(args.city, iso)
            if city_obj is None:
                logger.error("Не удалось определить город по вводу '%s'", args.city)
                return 1
            spec = f"{country_slug}/{city_obj.name}"
        else:
            spec = f"{country_slug}/{args.city}"

    events = wc.cmd_add_region(state, [spec])
    wc.save_state(state)
    append_events(events)
    for e in events:
        logger.info("[%s] %s", e.kind, e.message)
    return 0


def _spawn_region_worker(country: str, city: str) -> None:
    script_path = Path(__file__).resolve()
    cmd = [sys.executable, str(script_path), "add-region-worker", country, city or ""]
    subprocess.Popen(cmd, start_new_session=True,
                      stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def cmd_add_region(args: argparse.Namespace) -> int:
    if args.country is not None:
        # Прямой неинтерактивный вызов - страна (и, возможно, город) уже
        # указаны текстом или номером из ранее показанного списка.
        _spawn_region_worker(args.country, args.city or "")
        spec_display = f"{args.country}/{args.city}" if args.city else args.country
        print(f"Поиск пиров для региона '{spec_display}' запущен в фоне.")
        print("Прогресс смотри в watchdog.log, обычно занимает от нескольких")
        print("секунд до пары минут в зависимости от числа кандидатов.")
        return 0

    # Интерактивный режим - без аргументов, спрашиваем прямо здесь. Все
    # шаги в ОДНОМ процессе - номера разрешаются сразу же, в памяти, без
    # риска рассинхронизации между отдельными запусками (как могло бы
    # быть, если бы список показывался одной командой, а выбор - другой).
    print("Загружаю список стран из GitHub...")
    try:
        entries = pd.get_sorted_country_list()
    except Exception as e:
        print(f"Не удалось получить список: {e}", file=sys.stderr)
        return 1

    for i, e in enumerate(entries, 1):
        suffix = "" if e.supported else " (координаты пока не поддерживаются)"
        print(f"  {i}) {e.ru_name}{suffix}")

    country_input = input("Номер страны: ").strip()
    country_slug = _resolve_country_input(country_input)
    if country_slug is None:
        print("Неверный номер страны.", file=sys.stderr)
        return 1

    iso = pd.slug_to_iso(country_slug)
    city_name = ""
    if iso:
        db = pd.load_city_database()
        cities = pd.get_cities_for_country(db, iso)
        print(f"\nГорода для '{country_slug}' ({iso}), всего {len(cities)}:")
        for i, c in enumerate(cities, 1):
            print(f"  {i}) {c.display_label()} (население: {c.population})")
        print("\nЕсли нужен пир из ЛЮБОГО города страны - просто нажми Enter, ничего не вводя.")
        city_input = input("Номер города (необязательно): ").strip()
        if city_input:
            city_obj = _resolve_city_input(city_input, iso)
            if city_obj is None:
                print("Неверный номер города.", file=sys.stderr)
                return 1
            city_name = city_obj.name

    _spawn_region_worker(country_slug, city_name)
    spec_display = f"{country_slug}/{city_name}" if city_name else country_slug
    print(f"\nПоиск пиров для региона '{spec_display}' запущен в фоне.")
    print("Прогресс смотри в watchdog.log, обычно занимает от нескольких")
    print("секунд до пары минут в зависимости от числа кандидатов.")
    return 0


def cmd_remove_region(args: argparse.Namespace) -> int:
    state = wc.load_state()
    events = wc.cmd_remove_region(state)
    wc.save_state(state)
    append_events(events)

    for e in events:
        print(f"[{e.kind}] {e.message}")
    return 0


def cmd_find_best_peers(args: argparse.Namespace) -> int:
    count = int(args.count) if args.count else 3
    per_country = int(args.per_country) if args.per_country else 1

    print(f"Полный перебор по всем странам (по {per_country} кандидату на страну) - "
          f"это займёт время, возможно десятки минут...")
    results = pd.find_best_global_peers(count=count, per_country=per_country)

    if not results:
        print("Ни один кандидат не поднялся ни в одной стране.", file=sys.stderr)
        return 1

    print(f"\nЛучшие {len(results)} пиров по реальной задержке:")
    for peer, rtt in results:
        rtt_text = f"{rtt:.0f} мс" if rtt != float("inf") else "не измерено"
        print(f"  {peer.uri} ({peer.country_slug}/{peer.city}) - {rtt_text}")

    if args.add:
        config_path = wc.find_config_path()
        if config_path is None:
            print("\nНе найден yggdrasil.conf - не могу добавить автоматически.", file=sys.stderr)
            return 1
        print("\nДобавляю найденных в конфиг как основные пиры...")
        for peer, _ in results:
            wc.add_main_peer(config_path, peer.uri)
        print("Добавлено. Не забудь перезапустить службу Yggdrasil, чтобы изменения вступили в силу.")

    return 0


def cmd_list_main_peers(args: argparse.Namespace) -> int:
    config_path = wc.find_config_path()
    if config_path is None:
        print("Не найден yggdrasil.conf ни по одному из известных путей.", file=sys.stderr)
        return 1
    peers = wc.read_main_peers(config_path)
    print(f"Основные пиры ({len(peers)}):")
    for i, p in enumerate(peers, 1):
        print(f"  {i}) {p}")
    return 0


def cmd_add_main_peer(args: argparse.Namespace) -> int:
    config_path = wc.find_config_path()
    if config_path is None:
        print("Не найден yggdrasil.conf ни по одному из известных путей.", file=sys.stderr)
        return 1
    ok = wc.add_main_peer(config_path, args.uri)
    return 0 if ok else 1


def cmd_remove_main_peer(args: argparse.Namespace) -> int:
    config_path = wc.find_config_path()
    if config_path is None:
        print("Не найден yggdrasil.conf ни по одному из известных путей.", file=sys.stderr)
        return 1
    try:
        index = int(args.index)
    except ValueError:
        print("Нужен номер пира из списка (команда list-main-peers)", file=sys.stderr)
        return 1
    ok = wc.remove_main_peer(config_path, index)
    return 0 if ok else 1


def cmd_restart_yggdrasil(args: argparse.Namespace) -> int:
    wc.restart_yggdrasil_now()
    return 0


def cmd_restart_yggdrasil_delayed(args: argparse.Namespace) -> int:
    delay = 10
    if args.delay:
        try:
            delay = int(args.delay)
        except ValueError:
            pass
    wc.restart_yggdrasil_delayed(delay)
    return 0


def cmd_list_cities(args: argparse.Namespace) -> int:
    country_slug = _resolve_country_input(args.country)
    if country_slug is None:
        print(f"Неизвестная страна: '{args.country}'. Список стран - команда list-countries.", file=sys.stderr)
        return 1

    iso = pd.slug_to_iso(country_slug)
    if iso is None:
        print(f"Для страны '{country_slug}' нет соответствия ISO-коду - координаты не поддерживаются.", file=sys.stderr)
        return 1

    db = pd.load_city_database()
    cities = pd.get_cities_for_country(db, iso)
    if not cities:
        print(f"Городов для страны '{country_slug}' ({iso}) не найдено в cities.dat.")
        return 1

    print(f"Города для '{country_slug}' ({iso}), всего {len(cities)}:")
    for i, c in enumerate(cities, 1):
        print(f"  {i}) {c.display_label()} (население: {c.population})")
    print()
    print("Вводи НОМЕР города из списка выше (не название).")
    return 0


def cmd_list_countries(args: argparse.Namespace) -> int:
    print("Загружаю список стран из репозитория GitHub...")
    try:
        entries = pd.get_sorted_country_list()
    except Exception as e:
        print(f"Не удалось получить список: {e}", file=sys.stderr)
        return 1

    if not entries:
        print("Список пуст - что-то пошло не так при обращении к GitHub.")
        return 1

    print(f"Доступные страны ({len(entries)}):")
    for i, e in enumerate(entries, 1):
        suffix = "" if e.supported else " (координаты пока не поддерживаются)"
        print(f"  {i}) {e.ru_name}{suffix}")
    print()
    print("Вводи НОМЕР страны из списка выше (не название).")
    return 0


def _resolve_country_input(country_input: str) -> Optional[str]:
    """country_input может быть номером из списка list-countries или
    сразу slug'ом (прямой вызов из командной строки в обход меню)."""
    try:
        idx = int(country_input)
        entries = pd.get_sorted_country_list()
        if 1 <= idx <= len(entries):
            return entries[idx - 1].slug
        return None
    except ValueError:
        return country_input  # не число - считаем, что это уже slug


def _resolve_city_input(city_input: str, iso: str) -> Optional["pd.City"]:
    """city_input может быть номером из списка list-cities или сразу
    точным названием города (латиницей)."""
    db = pd.load_city_database()
    try:
        idx = int(city_input)
        cities = pd.get_cities_for_country(db, iso)
        if 1 <= idx <= len(cities):
            return cities[idx - 1]
        return None
    except ValueError:
        return pd.find_city(db, iso, city_input)

    return 0


def cmd_status(args: argparse.Namespace) -> int:
    state = wc.load_state()
    config_path = wc.find_config_path()

    print(f"Конфиг: {config_path if config_path else '(не найден)'}")
    print(f"Режим: {state.mode}")

    if config_path:
        main_peers = wc.read_main_peers(config_path)
        print(f"\nОсновные пиры ({len(main_peers)}):")
        for uri in main_peers:
            print(f"  {uri}")

    print(f"\nРезервные пиры ({len(state.backup_peers)}, регион: {state.backup_region}):")
    for uri in state.backup_peers:
        print(f"  {uri}")

    print(f"\nРегиональные пиры ({len(state.regional_peers)}, страны: {state.regional_countries}):")
    for uri in state.regional_peers:
        print(f"  {uri}")

    queue_path = _get_queue_path()
    if queue_path.exists():
        lines = queue_path.read_text(encoding="utf-8").splitlines()
        print(f"\nСобытий в очереди уведомлений: {len(lines)} (файл: {queue_path})")
    else:
        print(f"\nОчередь уведомлений пока пуста ({queue_path})")

    return 0


_LOG_PATH_CANDIDATES = [
    wc._project_dir() / "watchdog.log",
    wc._default_system_dir() / "watchdog.log",
    Path.home() / ".cache" / "ygg-watchdog" / "watchdog.log",
]


def _setup_logging() -> Path:
    """Настраивает логирование в консоль (как раньше) плюс в файл с
    ротацией (до 1 МБ на файл, 3 файла в запасе - этого с запасом
    хватит на историю ошибок, не разрастаясь бесконечно). Если ни один
    из системных путей не доступен для записи - логи всё равно идут
    в консоль, просто без файла, а не падают с ошибкой."""
    fmt = logging.Formatter("%(asctime)s %(levelname)s %(name)s: %(message)s")

    root = logging.getLogger()
    root.setLevel(logging.INFO)

    console = logging.StreamHandler()
    console.setFormatter(fmt)
    root.addHandler(console)

    for candidate in _LOG_PATH_CANDIDATES:
        try:
            candidate.parent.mkdir(parents=True, exist_ok=True)
            file_handler = logging.handlers.RotatingFileHandler(
                candidate, maxBytes=1_000_000, backupCount=3, encoding="utf-8",
            )
            file_handler.setFormatter(fmt)
            root.addHandler(file_handler)
            return candidate
        except OSError:
            continue

    logger.warning("Не удалось открыть файл лога ни по одному из путей - логи только в консоль")
    return None


def cmd_menu(args: argparse.Namespace) -> int:
    """Интерактивное меню - тот же набор возможностей, что и отдельные
    команды, но с удобной навигацией вместо запоминания синтаксиса."""
    while True:
        print()
        print("=" * 48)
        print("        YGG WATCHDOG")
        print("=" * 48)
        print()
        print("  1 - Проверить сейчас (tick)")
        print("  2 - Показать статус")
        print("  3 - Добавить регион (страна + город)")
        print("  4 - Убрать региональный режим")
        print("  5 - Показать основные пиры")
        print("  6 - Добавить основной пир")
        print("  7 - Удалить основной пир")
        print("  8 - Перезапустить Yggdrasil СЕЙЧАС (обрывает сессии по Yggdrasil!)")
        print("  9 - Перезапустить Yggdrasil С ЗАДЕРЖКОЙ (безопасно при SSH по Yggdrasil)")
        print("  0 - Выход")
        print()
        choice = input("Выбор: ").strip()

        if choice == "1":
            cmd_tick(argparse.Namespace())
        elif choice == "2":
            cmd_status(argparse.Namespace())
        elif choice == "3":
            cmd_add_region(argparse.Namespace(country=None, city=None))
        elif choice == "4":
            cmd_remove_region(argparse.Namespace())
        elif choice == "5":
            cmd_list_main_peers(argparse.Namespace())
        elif choice == "6":
            uri = input("URI пира (например tls://host:port): ").strip()
            if uri:
                cmd_add_main_peer(argparse.Namespace(uri=uri))
                print("Не забудь перезапустить службу - изменения вступят в силу только после этого.")
        elif choice == "7":
            cmd_list_main_peers(argparse.Namespace())
            index = input("Номер пира для удаления: ").strip()
            if index:
                cmd_remove_main_peer(argparse.Namespace(index=index))
                print("Не забудь перезапустить службу - изменения вступят в силу только после этого.")
        elif choice == "8":
            cmd_restart_yggdrasil(argparse.Namespace())
        elif choice == "9":
            delay = input("Задержка в секундах (по умолчанию 10): ").strip()
            cmd_restart_yggdrasil_delayed(argparse.Namespace(delay=delay or "10"))
        elif choice == "0":
            return 0
        else:
            print("Неверный пункт меню.")


def main() -> int:
    log_path = _setup_logging()

    parser = argparse.ArgumentParser(description="Yggdrasil watchdog - CLI")
    parser.add_argument(
        "-v", "--verbose", action="store_true",
        help="Подробный лог (DEBUG) - показывает каждый шаг рассуждений, а не только итоговые события",
    )
    subparsers = parser.add_subparsers(dest="command", required=False)

    p_menu = subparsers.add_parser("menu", help="Интерактивное меню (по умолчанию, если команда не указана)")
    p_menu.set_defaults(func=cmd_menu)

    p_tick = subparsers.add_parser("tick", help="Один цикл проверки (для планировщика)")
    p_tick.set_defaults(func=cmd_tick)

    p_add = subparsers.add_parser(
        "add-region",
        help="Включить региональный режим (без аргументов - интерактивно, спросит страну и город)",
    )
    p_add.add_argument("country", nargs="?", default=None,
                        help="Страна: номер из списка (list-countries) или slug напрямую")
    p_add.add_argument("city", nargs="?", default=None,
                        help="Город: номер из списка (list-cities) или точное название - необязательно")
    p_add.set_defaults(func=cmd_add_region)

    p_worker = subparsers.add_parser("add-region-worker", help="Внутренняя команда - реальный поиск (используется автоматически)")
    p_worker.add_argument("country")
    p_worker.add_argument("city", nargs="?", default="")
    p_worker.set_defaults(func=cmd_add_region_worker)

    p_list = subparsers.add_parser("list-countries", help="Показать список доступных стран для регионального режима")
    p_list.set_defaults(func=cmd_list_countries)

    p_cities = subparsers.add_parser("list-cities", help="Показать список городов внутри конкретной страны")
    p_cities.add_argument("country", help="Страна: номер из списка (list-countries) или slug напрямую")
    p_cities.set_defaults(func=cmd_list_cities)

    p_remove = subparsers.add_parser("remove-region", help="Снять региональные пиры, вернуться в normal")
    p_remove.set_defaults(func=cmd_remove_region)

    p_best = subparsers.add_parser(
        "find-best-peers",
        help="Полный перебор всех стран, поиск пиров с наименьшей реальной задержкой (долго)",
    )
    p_best.add_argument("--count", default="3", help="Сколько лучших вернуть (по умолчанию 3)")
    p_best.add_argument("--per-country", default="1", help="Сколько кандидатов пробовать на страну (по умолчанию 1)")
    p_best.add_argument("--add", action="store_true", help="Сразу добавить найденных как основные пиры в конфиг")
    p_best.set_defaults(func=cmd_find_best_peers)

    p_list_main = subparsers.add_parser("list-main-peers", help="Показать основные пиры из yggdrasil.conf")
    p_list_main.set_defaults(func=cmd_list_main_peers)

    p_add_main = subparsers.add_parser("add-main-peer", help="Добавить основного пира в конфиг (нужен перезапуск службы)")
    p_add_main.add_argument("uri", help="URI пира, например tls://host:port")
    p_add_main.set_defaults(func=cmd_add_main_peer)

    p_remove_main = subparsers.add_parser("remove-main-peer", help="Удалить основного пира из конфига по номеру")
    p_remove_main.add_argument("index", help="Номер пира из списка (list-main-peers)")
    p_remove_main.set_defaults(func=cmd_remove_main_peer)

    p_restart = subparsers.add_parser("restart-yggdrasil", help="Перезапустить службу Yggdrasil немедленно")
    p_restart.set_defaults(func=cmd_restart_yggdrasil)

    p_restart_delayed = subparsers.add_parser(
        "restart-yggdrasil-delayed",
        help="Перезапустить службу с задержкой (безопасно, если подключён по Yggdrasil)",
    )
    p_restart_delayed.add_argument("delay", nargs="?", default="10", help="Задержка в секундах (по умолчанию 10)")
    p_restart_delayed.set_defaults(func=cmd_restart_yggdrasil_delayed)

    p_status = subparsers.add_parser("status", help="Показать текущее состояние")
    p_status.set_defaults(func=cmd_status)

    args = parser.parse_args()

    if args.command is None:
        args.func = cmd_menu

    if args.verbose:
        logging.getLogger().setLevel(logging.DEBUG)
    if log_path:
        logger.debug("Лог пишется в: %s", log_path)

    try:
        return args.func(args)
    except Exception:
        # Любая необработанная ошибка обязательно попадает в лог с полным
        # traceback - без этого при работе через планировщик (без
        # интерактивного терминала) причина сбоя была бы просто потеряна.
        logger.error("Необработанная ошибка в команде '%s':\n%s", args.command, traceback.format_exc())
        return 1


if __name__ == "__main__":
    sys.exit(main())

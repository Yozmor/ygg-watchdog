#!/usr/bin/env python3
"""
selftest.py

Скрипт самопроверки проекта ygg-watchdog. Запускать после установки
(или после переноса на новый сервер) - проверяет, что все компоненты
на месте и реально работают, а не просто лежат рядом.

Часть проверок безопасны и ничего не трогают (синтаксис, чтение
конфига, живой запрос к GitHub без реального addPeer). Часть требует
root (доступ к yggdrasilctl) - без root они не проваливаются, а
помечаются как "пропущено" с понятной причиной.

Использование:
    python3 selftest.py        # без root - пропустит проверки yggdrasilctl
    sudo python3 selftest.py   # полная проверка, включая живой Yggdrasil
"""

import os
import subprocess
import sys
import time
import traceback
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(SCRIPT_DIR))

_COMMON_PYTHON_FILES = [
    "peer_database.py",
    "watchdog_core.py",
    "watchdog_daemon.py",
]
_COMMON_REQUIRED_FILES = _COMMON_PYTHON_FILES + ["cities.dat"]

if sys.platform == "win32":
    REQUIRED_FILES = _COMMON_REQUIRED_FILES + [
        "yggdrasil_menu.cpp",
        "run_yggdrasil_menu.bat",
        "install_scheduled_task.bat",
    ]
else:
    REQUIRED_FILES = _COMMON_REQUIRED_FILES + [
        "yggdrasil_menu.py",
        "ygg-watchdog-tick.service",
        "ygg-watchdog-tick.timer",
    ]

PASSED = []
FAILED = []
SKIPPED = []


def check(name: str):
    """Декоратор-обёртка: ловит исключения, красиво репортит результат."""
    def wrapper(fn):
        print(f"  {name}...", end=" ", flush=True)
        try:
            result = fn()
            if result == "skip":
                print("ПРОПУЩЕНО")
                SKIPPED.append(name)
            else:
                print("OK" + (f" ({result})" if result else ""))
                PASSED.append(name)
        except Exception as e:
            print(f"ПРОВАЛ: {e}")
            FAILED.append((name, str(e), traceback.format_exc()))
        return fn
    return wrapper


def is_root() -> bool:
    return hasattr(os, "geteuid") and os.geteuid() == 0


# ------------------------------------------------------------------
def section(title: str):
    print()
    print(f"=== {title} ===")


def main() -> int:
    print("Самопроверка ygg-watchdog")
    print(f"Папка проекта: {SCRIPT_DIR}")
    print(f"Запущено от root: {'да' if is_root() else 'нет (часть проверок будет пропущена)'}")

    # ------------------------------------------------------------------
    section("1. Наличие файлов проекта")

    @check("все необходимые файлы на месте")
    def _():
        missing = [f for f in REQUIRED_FILES if not (SCRIPT_DIR / f).exists()]
        if missing:
            raise RuntimeError(f"отсутствуют: {', '.join(missing)}")
        return f"{len(REQUIRED_FILES)} файлов"

    # ------------------------------------------------------------------
    section("2. Синтаксис Python-файлов")

    _py_files_to_check = _COMMON_PYTHON_FILES if sys.platform == "win32" else _COMMON_PYTHON_FILES + ["yggdrasil_menu.py"]
    for pyfile in _py_files_to_check:
        @check(f"py_compile {pyfile}")
        def _(pyfile=pyfile):
            result = subprocess.run(
                [sys.executable, "-m", "py_compile", str(SCRIPT_DIR / pyfile)],
                capture_output=True, text=True,
            )
            if result.returncode != 0:
                raise RuntimeError(result.stderr.strip())
            return None

    # ------------------------------------------------------------------
    section("3. Импорт модулей и наличие ключевых функций")

    @check("import peer_database")
    def _():
        import peer_database as pd
        required = ["pick_replacement_peers", "find_country_of_host",
                    "add_peer", "remove_peer", "get_live_peers",
                    "verify_peer_handshake", "list_available_countries",
                    "load_city_database", "find_city", "get_cities_for_country",
                    "find_nearest_working_peer", "slug_to_iso",
                    "slug_to_russian_name", "get_sorted_country_list",
                    "haversine_distance_km"]
        missing = [f for f in required if not hasattr(pd, f)]
        if missing:
            raise RuntimeError(f"нет функций: {missing}")
        return f"{len(required)} функций найдено"

    @check("import watchdog_core")
    def _():
        import watchdog_core as wc
        required = ["tick", "cmd_add_region", "cmd_remove_region",
                    "read_main_peers", "find_config_path",
                    "has_internet_connectivity", "check_internet",
                    "WatchdogState", "load_state", "save_state",
                    "modify_main_peers", "add_main_peer", "remove_main_peer",
                    "restart_yggdrasil_now", "restart_yggdrasil_delayed"]
        missing = [f for f in required if not hasattr(wc, f)]
        if missing:
            raise RuntimeError(f"нет функций: {missing}")
        return f"{len(required)} функций найдено"

    @check("import watchdog_daemon")
    def _():
        import watchdog_daemon as wd
        required = ["cmd_tick", "cmd_add_region", "cmd_remove_region", "cmd_status", "append_events",
                    "cmd_add_region_worker", "cmd_list_main_peers", "cmd_add_main_peer",
                    "cmd_remove_main_peer", "cmd_restart_yggdrasil", "cmd_restart_yggdrasil_delayed",
                    "cmd_menu"]
        missing = [f for f in required if not hasattr(wd, f)]
        if missing:
            raise RuntimeError(f"нет функций: {missing}")
        return None

    # ------------------------------------------------------------------
    section("4. Конфиг Yggdrasil (только чтение, ничего не трогает)")

    import watchdog_core as wc

    config_path_holder = {}

    @check("поиск yggdrasil.conf")
    def _():
        path = wc.find_config_path()
        if path is None:
            raise RuntimeError("не найден ни по одному известному пути")
        config_path_holder["path"] = path
        return str(path)

    @check("чтение основных пиров из конфига")
    def _():
        path = config_path_holder.get("path")
        if path is None:
            return "skip"
        peers = wc.read_main_peers(path)
        if not peers:
            raise RuntimeError("список пуст - это подозрительно, если пиры реально настроены")
        return f"{len(peers)} пиров"

    # ------------------------------------------------------------------
    section("5. Проверка интернета (реальная, независимо от Yggdrasil)")

    @check("has_internet_connectivity")
    def _():
        ok = wc.has_internet_connectivity()
        if not ok:
            raise RuntimeError("интернета нет - если это неожиданно, проверь сеть сервера")
        return "интернет есть"

    # ------------------------------------------------------------------
    section("6. GitHub - живой запрос базы пиров (сеть, но без addPeer)")

    import peer_database as pd

    @check("list_available_countries (живой запрос к GitHub)")
    def _():
        countries = pd.list_available_countries()
        if len(countries) < 10:
            raise RuntimeError(f"подозрительно мало стран: {len(countries)}")
        return f"{len(countries)} стран"

    @check("pick_replacement_peers без проверки рукопожатия (безопасно)")
    def _():
        peers = pd.pick_replacement_peers("russia", exclude_hosts=set(), count=3, verify_handshake=False)
        if not peers:
            raise RuntimeError("не нашлось ни одного пира для russia - странно")
        return f"{len(peers)} кандидатов"

    # ------------------------------------------------------------------
    section("6.5 База городов (cities.dat - координаты для регионального поиска)")

    @check("cities.dat найден и загружается")
    def _():
        db = pd.load_city_database()
        if len(db) < 1000:
            raise RuntimeError(f"подозрительно мало городов: {len(db)}")
        return f"{len(db)} городов"

    @check("реальный расчёт расстояния (Москва-Владивосток, ожидаем ~6400 км)")
    def _():
        db = pd.load_city_database()
        moscow = pd.find_city(db, "RU", "Moscow")
        vladivostok = pd.find_city(db, "RU", "Vladivostok")
        if not moscow or not vladivostok:
            raise RuntimeError("Москва или Владивосток не найдены в базе")
        dist = pd.haversine_distance_km(moscow.lat, moscow.lon, vladivostok.lat, vladivostok.lon)
        if not (6000 < dist < 6800):
            raise RuntimeError(f"расстояние вне ожидаемого диапазона: {dist:.0f} км")
        return f"{dist:.0f} км"

    @check("соответствие стран slug->ISO (russia->RU)")
    def _():
        iso = pd.slug_to_iso("russia")
        if iso != "RU":
            raise RuntimeError(f"ожидался RU, получено {iso}")
        return None

    # ------------------------------------------------------------------
    section("7. yggdrasilctl (требует root)")

    @check("get_live_peers (нужен root)")
    def _():
        if not is_root():
            return "skip"
        live = pd.get_live_peers()
        return f"{len(live)} пиров в живом выводе"

    @check("настоящее рукопожатие с реальным кандидатом (нужен root, займёт время)")
    def _():
        if not is_root():
            return "skip"
        # Берём кандидата, которого точно нет среди уже настроенных -
        # проверяем полный цикл addPeer -> ждём -> removePeer
        config_path = config_path_holder.get("path")
        exclude = set()
        if config_path:
            main_peers = wc.read_main_peers(config_path)
            for uri in main_peers:
                parsed = pd.parse_uri(uri)
                if parsed:
                    exclude.add(parsed[1])  # (protocol, host, port) - берём host
        candidates = pd.pick_replacement_peers("russia", exclude_hosts=exclude, count=1, verify_handshake=True)
        if not candidates:
            raise RuntimeError("ни один кандидат не прошёл рукопожатие (может быть временно - не обязательно баг)")
        return f"{candidates[0].uri} поднялся и был корректно убран"

    # ------------------------------------------------------------------
    section("8. Полный цикл watchdog_daemon tick (нужен root)")

    @check("watchdog_daemon.py tick (реальный, но безопасный при живых пирах)")
    def _():
        if not is_root():
            return "skip"
        result = subprocess.run(
            [sys.executable, str(SCRIPT_DIR / "watchdog_daemon.py"), "tick"],
            capture_output=True, text=True, timeout=60,
        )
        if result.returncode != 0:
            raise RuntimeError(f"код возврата {result.returncode}: {result.stderr.strip()}")
        return result.stdout.strip().splitlines()[0] if result.stdout.strip() else "(пусто)"

    # ------------------------------------------------------------------
    section("9. Планировщик (systemd-таймер на Linux / Scheduled Task на Windows)")

    @check("планировщик установлен и активен")
    def _():
        if sys.platform == "win32":
            result = subprocess.run(
                ["schtasks", "/Query", "/TN", "YggWatchdogTick"],
                capture_output=True, text=True,
            )
            if result.returncode != 0:
                raise RuntimeError("задача YggWatchdogTick не найдена - запусти install_scheduled_task.bat")
            return "задача YggWatchdogTick найдена"
        else:
            result = subprocess.run(
                ["systemctl", "is-active", "ygg-watchdog-tick.timer"],
                capture_output=True, text=True,
            )
            status = result.stdout.strip()
            if status != "active":
                raise RuntimeError(f"статус: {status} (ожидался 'active')")
        return "active"

    # ------------------------------------------------------------------
    section("9.5 Управление основными пирами (только чтение - ничего не меняет)")

    @check("wc.read_main_peers на реальном конфиге")
    def _():
        path = config_path_holder.get("path")
        if path is None:
            return "skip"
        peers = wc.read_main_peers(path)
        return f"{len(peers)} пиров (то же, что и в п.4)"

    # ------------------------------------------------------------------
    section("10. Панель управления (симлинк на Linux / скомпилированный .exe на Windows)")

    @check("команда yggdrasil доступна и указывает в проект")
    def _():
        if sys.platform == "win32":
            exe_path = SCRIPT_DIR / "yggdrasil_menu.exe"
            if not exe_path.exists():
                raise RuntimeError(
                    "yggdrasil_menu.exe не найден - запусти run_yggdrasil_menu.bat "
                    "хотя бы раз, он сам скомпилирует .exe из .cpp"
                )
            return str(exe_path)

        link_path = Path("/usr/local/bin/yggdrasil")
        if not link_path.is_symlink():
            raise RuntimeError("это не симлинк - возможно, снова разъехалось на копию")
        target = link_path.resolve()
        if target != (SCRIPT_DIR / "yggdrasil_menu.py").resolve():
            raise RuntimeError(f"указывает не туда: {target}")
        return str(target)

    # ------------------------------------------------------------------
    print()
    print("=" * 60)
    print(f"ИТОГ: {len(PASSED)} пройдено, {len(SKIPPED)} пропущено, {len(FAILED)} провалено")

    if SKIPPED and not is_root():
        print()
        print("Часть проверок пропущена, потому что скрипт запущен без root.")
        print("Для полной проверки: sudo python3 selftest.py")

    if FAILED:
        print()
        print("ПРОВАЛЕННЫЕ ПРОВЕРКИ:")
        for name, err, tb in FAILED:
            print(f"  - {name}: {err}")
        return 1

    print()
    print("Всё в порядке." if not SKIPPED else "Базовые проверки пройдены (часть пропущена без root).")
    return 0


if __name__ == "__main__":
    sys.exit(main())

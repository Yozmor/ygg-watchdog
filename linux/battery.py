"""
battery.py

Батарейный аварийный режим - OS-generic по смыслу (логика одинаковая
для Windows и Linux, только способ прочитать железо разный - это не
подстройка под конкретные ноуты пользователя, а под операционку вообще).
На Linux смотрим /sys/class/power_supply/BAT*/{capacity,status}; если
такого пути вообще нет (стационарный сервер без батареи) - молча считаем,
что батареи нет, аварийный режим на такой машине никогда не включается.
"""

import logging
from pathlib import Path
from typing import Optional

logger = logging.getLogger("ygg_watchdog.battery")

CRITICAL_PERCENT = 5


def read_battery_status() -> Optional[tuple[int, bool]]:
    """Возвращает (процент_заряда, идёт_ли_зарядка), или None, если на
    этой машине вообще нет батареи (ни одного /sys/class/power_supply/BAT*) -
    например, стационарный сервер."""
    power_supply = Path("/sys/class/power_supply")
    if not power_supply.exists():
        return None

    try:
        entries = sorted(power_supply.iterdir())
    except OSError:
        return None

    for entry in entries:
        if not entry.name.startswith("BAT"):
            continue
        try:
            capacity = int((entry / "capacity").read_text().strip())
            status = (entry / "status").read_text().strip().lower()
        except (OSError, ValueError):
            continue
        is_charging = status in ("charging", "full")
        return capacity, is_charging

    return None

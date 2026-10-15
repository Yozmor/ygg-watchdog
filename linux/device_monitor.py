"""
device_monitor.py

Level 2 диагностики - мониторинг доступности ДРУГИХ твоих устройств по
Yggdrasil-адресу (два ноута, телефон осознанно не мониторится). Вызывается
из watchdog_core.tick() ТОЛЬКО когда основная связность (главные/резервные/
региональные пиры) уже подтверждена здоровой - не повторяет то, что
Level 1 и так проверил (маршруты/DNS/VPN), только то, что он не покрывает.

Дебаунс - быстрая серия пингов ВНУТРИ одного вызова (не растягивается на
несколько тиков, это плохо сочетается с 3-минутным интервалом тика):
одиночная заминка DHT-маршрутизации после смены топологии не считается
обрывом, только устойчивый сбой всех попыток подряд.

На подтверждённый обрыв - однократная эскалация (с кулдауном на повтор,
чтобы не спамить и не дёргать службу на каждом тике, если устройство
просто надолго офлайн): снимок факта "мешает ли нагрузка самому
Yggdrasil" (не системе вообще - это разные вещи, см. обсуждение) +
перезапуск своей локальной службы.
"""

import logging
import subprocess
import time
from typing import Optional

import peer_database as pd

logger = logging.getLogger("ygg_watchdog.device_monitor")

PING_ATTEMPTS = 3
PING_ATTEMPT_TIMEOUT_SECONDS = 2
PING_RETRY_PAUSE_SECONDS = 3

ESCALATION_COOLDOWN_SECONDS = 3600  # не повторять диагностику+рестарт чаще раза в час на одно устройство
YGGCTL_RESPONSIVENESS_THRESHOLD_SECONDS = 2.0


def ping_device(address: str) -> bool:
    """Быстрая серия попыток - если хоть одна прошла, устройство живо.
    Это и есть дебаунс: берём несколько секунд внутри тика вместо
    растягивания подтверждения на несколько тиков."""
    for attempt in range(PING_ATTEMPTS):
        try:
            result = subprocess.run(
                ["ping", "-6", "-c", "1", "-W", str(PING_ATTEMPT_TIMEOUT_SECONDS), address],
                capture_output=True, timeout=PING_ATTEMPT_TIMEOUT_SECONDS + 1,
            )
            if result.returncode == 0:
                return True
        except (FileNotFoundError, subprocess.TimeoutExpired):
            pass
        if attempt < PING_ATTEMPTS - 1:
            time.sleep(PING_RETRY_PAUSE_SECONDS)
    return False


def check_yggdrasil_responsiveness() -> tuple[bool, float]:
    """Отзывается ли control-сокет самого Yggdrasil вовремя - прямой
    признак того, мешает ли нагрузка именно ЕМУ прямо сейчас, а не
    системе в целом (общая загрузка CPU может быть низкой, а конкретно
    Yggdrasil всё равно может не успевать - и наоборот)."""
    start = time.monotonic()
    try:
        pd.run_yggctl(["getSelf"])
        ok = True
    except pd.PeerDatabaseError:
        ok = False
    elapsed = time.monotonic() - start
    responsive = ok and elapsed < YGGCTL_RESPONSIVENESS_THRESHOLD_SECONDS
    return responsive, elapsed


def _cpu_busy_percent(sample_seconds: float = 1.0) -> Optional[float]:
    """% занятости CPU ПО ПРОЦЕССУ Yggdrasil было бы точнее, но не всегда
    тривиально найти PID переносимо; берём системную загрузку за короткое
    окно (а не моментальный снимок - тот слишком шумный) как более простой,
    хоть и менее точный индикатор-дополнение к check_yggdrasil_responsiveness,
    который и есть главный, прямой признак."""
    def _read() -> Optional[tuple[int, int]]:
        try:
            with open("/proc/stat") as f:
                for line in f:
                    if line.startswith("cpu "):
                        parts = [int(x) for x in line.split()[1:]]
                        idle = parts[3] + (parts[4] if len(parts) > 4 else 0)
                        return idle, sum(parts)
        except OSError:
            return None
        return None

    first = _read()
    if first is None:
        return None
    time.sleep(sample_seconds)
    second = _read()
    if second is None:
        return None

    idle_delta = second[0] - first[0]
    total_delta = second[1] - first[1]
    if total_delta <= 0:
        return None
    return 100.0 * (1 - idle_delta / total_delta)


def _mem_available_percent() -> Optional[float]:
    total = available = None
    try:
        with open("/proc/meminfo") as f:
            for line in f:
                if line.startswith("MemTotal:"):
                    total = int(line.split()[1])
                elif line.startswith("MemAvailable:"):
                    available = int(line.split()[1])
    except OSError:
        return None
    if not total or available is None:
        return None
    return 100.0 * available / total


def describe_local_load() -> str:
    """Факт для эскалации: мешает ли нагрузка самому Yggdrasil сейчас -
    не просто загруженность системы вообще. check_yggdrasil_responsiveness
    - прямой и решающий признак; CPU/память - вспомогательные цифры
    в подтверждение, не самостоятельный вердикт."""
    responsive, elapsed = check_yggdrasil_responsiveness()
    cpu = _cpu_busy_percent()
    mem_avail = _mem_available_percent()

    cpu_text = f"{cpu:.0f}%" if cpu is not None else "н/д"
    mem_text = f"{mem_avail:.0f}%" if mem_avail is not None else "н/д"

    if not responsive:
        verdict = "ПЕРЕГРУЖЕНА (control-сокет Yggdrasil не ответил вовремя)"
    elif cpu is not None and cpu > 90:
        verdict = "ПЕРЕГРУЖЕНА (CPU)"
    elif mem_avail is not None and mem_avail < 10:
        verdict = "ПЕРЕГРУЖЕНА (память)"
    else:
        verdict = "в норме"

    return (f"Нагрузка: {verdict} (CPU {cpu_text}, свободно ОЗУ {mem_text}, "
            f"отклик Yggdrasil {elapsed:.2f} с)")


def restart_own_yggdrasil() -> None:
    try:
        subprocess.run(["systemctl", "restart", "yggdrasil"], capture_output=True, timeout=15)
        logger.info("Перезапустил локальную службу Yggdrasil из-за недоступности устройства")
    except (FileNotFoundError, subprocess.TimeoutExpired) as e:
        logger.warning("Не удалось перезапустить Yggdrasil: %s", e)


def check_devices(
    devices: dict[str, str],
    down_since: dict[str, float],
    escalated_until: dict[str, float],
) -> list[tuple[str, str]]:
    """Мутирует down_since/escalated_until НА МЕСТЕ (вызывающий код должен
    сохранить state после вызова). Возвращает события как (kind, message) -
    без зависимости от Event из watchdog_core, чтобы не заводить
    циклический импорт."""
    events: list[tuple[str, str]] = []
    now = time.time()

    for name, address in devices.items():
        if ping_device(address):
            if name in down_since:
                downtime = now - down_since.pop(name)
                escalated_until.pop(name, None)
                minutes, seconds = divmod(int(downtime), 60)
                events.append((
                    "device_recovered",
                    f"Связь с устройством '{name}' восстановлена (недоступность {minutes} мин {seconds} сек).",
                ))
            continue

        if name not in down_since:
            down_since[name] = now

        cooldown_until = escalated_until.get(name)
        if cooldown_until is not None and now < cooldown_until:
            logger.debug("Устройство '%s' всё ещё недоступно, эскалация в кулдауне", name)
            continue

        load_desc = describe_local_load()
        events.append((
            "device_unreachable",
            f"Устройство '{name}' ({address}) не отвечает, хотя связь с пирами в порядке. "
            f"Либо оно само офлайн (выключено/закрыта крышка), либо реальный разрыв пути - "
            f"отличить со своей стороны не можем. {load_desc}",
        ))
        restart_own_yggdrasil()
        escalated_until[name] = now + ESCALATION_COOLDOWN_SECONDS

    return events

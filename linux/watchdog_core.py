"""
watchdog_core.py

Логика принятия решений watchdog: три категории пиров (основные,
резервные, региональные) и правила перехода между состояниями.

Ключевой технический факт, на котором строится разделение категорий:
addPeer/removePeer через yggdrasilctl - runtime-команды, они НЕ пишутся
в yggdrasil.conf и пропадают при перезапуске службы. Поэтому:
- "Основные" пиры - это то, что реально лежит в Peers: [] конфига
  (источник истины - сам файл, watchdog никогда его не трогает)
- "Резервные"/"Региональные" - существуют только в памяти запущенного
  yggdrasil и в нашем собственном файле состояния watchdog

Правила (заданы пользователем):
1. Все основные + все резервные лежат -> искать замену того же региона,
   что основные, добавлять как резервные
2. Хотя бы один ОСНОВНОЙ поднялся -> немедленно убрать ВСЕ резервные
3. Явная команда "добавить регион" -> переключение в региональный режим
4. Пока региональный режим активен - состояние основных не проверяется
   вообще (ожидаемо, что они могут лежать - это не повод для действий);
   вместо этого watchdog следит за региональными пирами и точно так же
   ищет им замену при полном отвале
5. Региональные пиры убираются ТОЛЬКО по явной команде, никогда
   автоматически - даже если основные внезапно ожили
6. Основные пиры - отдельный ручной инструмент (панель управления),
   этот модуль их не добавляет и не удаляет никогда
"""

import json
import logging
import os
import re
import socket
import subprocess
import sys
import time
from dataclasses import dataclass, field
from datetime import datetime
from pathlib import Path
from typing import Optional

import auto_update as au
import battery as bat
import device_monitor as dm
import geoip
import peer_database as pd
import vpn_hook as vh

logger = logging.getLogger("ygg_watchdog.core")

DEFAULT_CONFIG_PATHS = [
    Path("/etc/yggdrasil/yggdrasil.conf"),
    Path("C:/ProgramData/Yggdrasil/yggdrasil.conf"),
    Path("C:/Program Files/Yggdrasil/yggdrasil.conf"),
]

def _project_dir() -> Path:
    """Папка, где лежит сам watchdog_core.py - и, по договорённости, все
    остальные файлы проекта (батники, другие .py). Файлы состояния/лога/
    очереди кладутся именно сюда по умолчанию - пользователь явно просил
    не разбрасывать их по системным папкам вроде ProgramData, раз и так
    держит весь проект в одной папке."""
    return Path(__file__).resolve().parent


def _default_system_dir() -> Path:
    """/var/lib на Linux, %ProgramData% на Windows - используется только
    как ЗАПАСНОЙ вариант, если папка проекта почему-то недоступна для
    записи (например, лежит в Program Files с ограничениями)."""
    if sys.platform == "win32":
        base = os.environ.get("ProgramData", r"C:\ProgramData")
        return Path(base) / "ygg-watchdog"
    return Path("/var/lib/ygg-watchdog")


_USER_STATE_PATH = Path.home() / ".cache" / "ygg-watchdog" / "state.json"
_resolved_state_path: Optional[Path] = None


# ------------------------------------------------------------------
# Чтение основных пиров прямо из конфига (источник истины)
# ------------------------------------------------------------------
_PEERS_BLOCK_RE = re.compile(r"Peers\s*:\s*\[(.*?)\]", re.DOTALL)
_QUOTED_URI_RE = re.compile(r'"([^"]+)"')


def read_main_peers(config_path: Path) -> list[str]:
    """Достаёт список основных пиров прямо из Peers: [...] в конфиге.
    Не пытается полностью распарсить весь HJSON-конфиг - только этот
    один блок, регэкспом, что достаточно надёжно для формата, в котором
    Yggdrasil реально пишет свои конфиги (список строк в кавычках)."""
    text = config_path.read_text(encoding="utf-8")
    block_match = _PEERS_BLOCK_RE.search(text)
    if not block_match:
        return []
    block = block_match.group(1)
    return _QUOTED_URI_RE.findall(block)


def modify_main_peers(config_path: Path, mutator) -> bool:
    """Изменяет блок Peers: [...] в конфиге, вызывая mutator(peers_list)
    и записывая результат обратно. Весь остальной конфиг (PrivateKey,
    MulticastInterfaces и т.д.) остаётся байт-в-байт нетронутым - меняется
    только содержимое самого блока Peers."""
    text = config_path.read_text(encoding="utf-8")
    block_match = _PEERS_BLOCK_RE.search(text)
    if not block_match:
        logger.error("Не найден блок Peers: [...] в конфиге")
        return False

    peers = _QUOTED_URI_RE.findall(block_match.group(1))
    mutator(peers)

    new_block_content = "\n" + "".join(f'  "{p}",\n' for p in peers)
    new_text = (
        text[:block_match.start(1)] + new_block_content + text[block_match.end(1):]
    )

    try:
        config_path.write_text(new_text, encoding="utf-8")
    except OSError as e:
        logger.error("Не удалось записать конфиг (нет прав?): %s", e)
        return False
    return True


def add_main_peer(config_path: Path, uri: str) -> bool:
    ok = modify_main_peers(config_path, lambda peers: peers.append(uri))
    if ok:
        logger.info("Пир добавлен в конфиг: %s", uri)
        logger.info("Изменения вступят в силу ТОЛЬКО после перезапуска службы Yggdrasil.")
    return ok


def remove_main_peer(config_path: Path, index: int) -> bool:
    current = read_main_peers(config_path)
    if index < 1 or index > len(current):
        logger.error("Нет пира с номером %d", index)
        return False
    to_remove = current[index - 1]

    def mutator(peers: list[str]) -> None:
        if to_remove in peers:
            peers.remove(to_remove)

    ok = modify_main_peers(config_path, mutator)
    if ok:
        logger.info("Пир удалён из конфига: %s", to_remove)
        logger.info("Изменения вступят в силу ТОЛЬКО после перезапуска службы Yggdrasil.")
    return ok


# ------------------------------------------------------------------
# Перезапуск службы Yggdrasil - нужен после правки основных пиров
# (конфиг читается только при старте). ВАЖНО: если подключён к серверу
# ЧЕРЕЗ САМ Yggdrasil (например SSH по Yggdrasil-адресу) - обычный
# перезапуск оборвёт твою же сессию. Для этого случая - отложенный
# вариант через systemd-run, команда успевает уйти по ещё живому каналу.
# ------------------------------------------------------------------
def restart_yggdrasil_now() -> None:
    logger.info("Перезапускаю службу Yggdrasil немедленно...")
    logger.info(
        "Внимание: если подключён к этому серверу ЧЕРЕЗ Yggdrasil - "
        "соединение сейчас оборвётся. Если это единственный канал - "
        "используй отложенный перезапуск вместо этого."
    )
    subprocess.run(["systemctl", "restart", "yggdrasil"])
    logger.info("Перезапуск выполнен.")


def restart_yggdrasil_delayed(delay_seconds: int = 10) -> None:
    unit_name = f"ygg-watchdog-delayed-restart-{int(time.time())}"
    subprocess.run([
        "systemd-run",
        f"--unit={unit_name}",
        f"--on-active={delay_seconds}s",
        "systemctl", "restart", "yggdrasil",
    ])
    logger.info(
        "Перезапуск службы запланирован через %d секунд. "
        "Успей отключиться/переключиться, если сидишь по Yggdrasil.",
        delay_seconds,
    )


def stop_watchdog_timer() -> bool:
    """Останавливает и отключает systemd-таймер ygg-watchdog-tick.timer -
    после этого автоматические проверки (каждые 3 минуты) прекращаются,
    пока таймер не включат обратно через start_watchdog_timer(). Аналог
    uninstall-task в Windows-версии."""
    logger.info("Останавливаю таймер ygg-watchdog-tick (автоматические проверки)...")
    result = subprocess.run(
        ["systemctl", "disable", "--now", "ygg-watchdog-tick.timer"],
        capture_output=True, text=True,
    )
    if result.returncode == 0:
        logger.info("Таймер остановлен и отключён. Автоматические проверки не идут, "
                     "пока не включишь таймер обратно.")
        return True
    logger.error(
        "Не удалось остановить таймер (код %d): %s",
        result.returncode, result.stderr.strip() if result.stderr else "",
    )
    return False


def start_watchdog_timer() -> bool:
    """Включает и запускает systemd-таймер обратно - после долгой паузы
    или если его когда-то остановили через stop_watchdog_timer()."""
    logger.info("Включаю таймер ygg-watchdog-tick обратно...")
    result = subprocess.run(
        ["systemctl", "enable", "--now", "ygg-watchdog-tick.timer"],
        capture_output=True, text=True,
    )
    if result.returncode == 0:
        logger.info("Таймер включён - проверки снова будут идти каждые 3 минуты.")
        return True
    logger.error(
        "Не удалось включить таймер (код %d): %s",
        result.returncode, result.stderr.strip() if result.stderr else "",
    )
    return False


def find_config_path() -> Optional[Path]:
    for candidate in DEFAULT_CONFIG_PATHS:
        if candidate.exists():
            return candidate
    return None


def _host_from_uri(uri: str) -> Optional[str]:
    parsed = pd.parse_uri(uri)
    return parsed[1] if parsed else None


# ------------------------------------------------------------------
# ------------------------------------------------------------------
# Файл состояния watchdog
# ------------------------------------------------------------------
@dataclass
class WatchdogState:
    mode: str = "normal"  # "normal" | "regional"
    backup_peers: list[str] = field(default_factory=list)
    backup_region: Optional[str] = None
    regional_peers: list[str] = field(default_factory=list)
    regional_countries: list[str] = field(default_factory=list)
    internet_down_since: Optional[float] = None  # unix timestamp, локальные часы (не зависят от интернета)
    consecutive_region_fetch_failures: int = 0  # подряд неудач обращения к GitHub, хотя интернет вроде жив
    home_region: Optional[str] = None  # slug репозитория, задаётся руками (set-home-region), всегда запасной вариант
    egress_region: Optional[str] = None  # slug - где СЕЙЧАС реально выходит трафик (кэш, см. get_backup_search_region)
    egress_region_checked_at: Optional[float] = None  # когда egress_region последний раз пересчитывался (успешно или нет)
    # SOS-перебор (крайняя мера): снимок списка стран на текущий проход + курсор
    # (индекс страны, индекс пира внутри неё), резюмируемый между тиками/перезапусками.
    sos_country_slugs: list[str] = field(default_factory=list)  # пусто = проход не идёт
    sos_country_index: int = 0
    sos_peer_index: int = 0
    sos_last_full_scan_completed_at: Optional[float] = None  # unix timestamp - для 12-часового кулдауна
    # Мониторинг устройств (Level 2) - только свои два ноута, телефон не мониторится.
    devices: dict[str, str] = field(default_factory=dict)  # {имя: ygg-адрес}
    device_down_since: dict[str, float] = field(default_factory=dict)
    device_escalated_until: dict[str, float] = field(default_factory=dict)
    # Батарейный аварийный режим
    battery_critical_mode: bool = False
    battery_critical_entered_percent: Optional[int] = None
    # Автообновление - только уведомление, скачивание/применение по команде пользователя
    update_last_checked_at: Optional[float] = None
    update_notified_tag: Optional[str] = None

    def to_dict(self) -> dict:
        return {
            "mode": self.mode,
            "backup_peers": self.backup_peers,
            "backup_region": self.backup_region,
            "regional_peers": self.regional_peers,
            "regional_countries": self.regional_countries,
            "internet_down_since": self.internet_down_since,
            "consecutive_region_fetch_failures": self.consecutive_region_fetch_failures,
            "home_region": self.home_region,
            "egress_region": self.egress_region,
            "egress_region_checked_at": self.egress_region_checked_at,
            "sos_country_slugs": self.sos_country_slugs,
            "sos_country_index": self.sos_country_index,
            "sos_peer_index": self.sos_peer_index,
            "sos_last_full_scan_completed_at": self.sos_last_full_scan_completed_at,
            "devices": self.devices,
            "device_down_since": self.device_down_since,
            "device_escalated_until": self.device_escalated_until,
            "battery_critical_mode": self.battery_critical_mode,
            "battery_critical_entered_percent": self.battery_critical_entered_percent,
            "update_last_checked_at": self.update_last_checked_at,
            "update_notified_tag": self.update_notified_tag,
        }

    @classmethod
    def from_dict(cls, d: dict) -> "WatchdogState":
        return cls(
            mode=d.get("mode", "normal"),
            backup_peers=list(d.get("backup_peers", [])),
            backup_region=d.get("backup_region"),
            regional_peers=list(d.get("regional_peers", [])),
            regional_countries=list(d.get("regional_countries", [])),
            internet_down_since=d.get("internet_down_since"),
            consecutive_region_fetch_failures=d.get("consecutive_region_fetch_failures", 0),
            home_region=d.get("home_region"),
            egress_region=d.get("egress_region"),
            egress_region_checked_at=d.get("egress_region_checked_at"),
            sos_country_slugs=list(d.get("sos_country_slugs", [])),
            sos_country_index=d.get("sos_country_index", 0),
            sos_peer_index=d.get("sos_peer_index", 0),
            sos_last_full_scan_completed_at=d.get("sos_last_full_scan_completed_at"),
            devices=dict(d.get("devices", {})),
            device_down_since=dict(d.get("device_down_since", {})),
            device_escalated_until=dict(d.get("device_escalated_until", {})),
            battery_critical_mode=d.get("battery_critical_mode", False),
            battery_critical_entered_percent=d.get("battery_critical_entered_percent"),
            update_last_checked_at=d.get("update_last_checked_at"),
            update_notified_tag=d.get("update_notified_tag"),
        )


def _get_state_path() -> Path:
    global _resolved_state_path
    if _resolved_state_path is not None:
        return _resolved_state_path
    candidates = (_project_dir() / "state.json", _default_system_dir() / "state.json", _USER_STATE_PATH)
    for candidate in candidates:
        try:
            candidate.parent.mkdir(parents=True, exist_ok=True)
            # Проверяем реальную возможность записи, не только существование
            # папки - mkdir мог сработать, а вот сам файл может быть
            # недоступен для записи (например, папка проекта в Program Files)
            test_path = candidate.parent / ".write_test"
            test_path.write_text("")
            test_path.unlink()
            _resolved_state_path = candidate
            return candidate
        except OSError:
            continue
    fallback = Path("/tmp/ygg-watchdog/state.json") if sys.platform != "win32" else Path(os.environ.get("TEMP", r"C:\Temp")) / "ygg-watchdog" / "state.json"
    fallback.parent.mkdir(parents=True, exist_ok=True)
    _resolved_state_path = fallback
    return fallback


def load_state() -> WatchdogState:
    path = _get_state_path()
    if not path.exists():
        return WatchdogState()
    return WatchdogState.from_dict(json.loads(path.read_text(encoding="utf-8")))


def save_state(state: WatchdogState) -> None:
    path = _get_state_path()
    try:
        path.write_text(json.dumps(state.to_dict(), ensure_ascii=False, indent=2), encoding="utf-8")
    except OSError as e:
        logger.warning("Не удалось сохранить состояние watchdog (%s)", e)


# ------------------------------------------------------------------
# Регион для поиска резервных пиров - ПО EGRESS (где сейчас реально
# выходит трафик), а не по географии основных пиров.
#
# Старая логика (определить регион ПО ГЕОГРАФИИ ОСНОВНЫХ ПИРОВ) была
# ошибочной именно в сценарии, для которого и существует резервный
# режим: если основные пиры недоступны из-за того, что VPN перехватил
# трафик (или наоборот, DPI блокирует прямые соединения и включён VPN) -
# искать замену в регионе, откуда трафик больше не выходит, бессмысленно.
# Правильный критерий - где СЕЙЧАС реально оказывается трафик после
# всей текущей маршрутизации (включая VPN, если он активен).
# ------------------------------------------------------------------
EGRESS_REGION_REFRESH_SECONDS = 3600  # раз в час - без отдельного детектора смены VPN
                                       # (это самостоятельная задача на будущее) берём
                                       # периодический пересчёт как достаточное приближение


def _refresh_egress_region(state: "WatchdogState") -> None:
    """Пересчитывает egress-регион, если прошло достаточно времени с
    прошлой попытки. Отмечает момент попытки НЕЗАВИСИМО от результата -
    иначе при недоступности всех GeoIP-провайдеров мы долбили бы их
    на каждом тике без всякого толку. При неудаче старое кэшированное
    значение (если было) просто продолжает использоваться."""
    now = time.time()
    if (state.egress_region_checked_at is not None
            and now - state.egress_region_checked_at < EGRESS_REGION_REFRESH_SECONDS):
        return

    state.egress_region_checked_at = now
    iso = geoip.detect_egress_country_iso()
    if iso is None:
        return

    slug = pd.iso_to_slug(iso)
    if slug is None:
        logger.debug("Egress-страна '%s' не представлена в репозитории public-peers", iso)
        return

    if slug != state.egress_region:
        logger.info("Egress-регион изменился: %s -> %s", state.egress_region, slug)
    state.egress_region = slug


def get_backup_search_region(state: "WatchdogState") -> Optional[str]:
    """Регион, по которому ищем резервных пиров. Приоритет:
    1. Текущий egress-регион (кэш, пересчитывается не чаще раза в час).
    2. Домашний регион, заданный руками через set-home-region - всегда
       доступный запасной вариант, даже если GeoIP недоступен целиком.
    None только если не удалось получить ни то, ни другое."""
    _refresh_egress_region(state)
    if state.egress_region:
        return state.egress_region
    if state.home_region:
        logger.debug("Egress-регион не определён - использую домашний регион '%s'", state.home_region)
        return state.home_region
    return None


# ------------------------------------------------------------------
# Событие для очереди уведомлений
# ------------------------------------------------------------------
@dataclass
class Event:
    kind: str
    message: str


# Диагностика: пропал интернет вообще, или именно Yggdrasil-пиры
# ------------------------------------------------------------------
_INTERNET_CHECK_TARGETS = [("1.1.1.1", 443), ("8.8.8.8", 443), ("9.9.9.9", 443)]


def _try_flush_dns_cache() -> None:
    """Сырые IP отвечают, а DNS - нет: частая причина - завис
    отрицательный кэш systemd-resolved после сетевого сбоя, который НЕ
    сбрасывается перезапуском самого Yggdrasil (это разные, несвязанные
    службы) - раньше помогала только полная перезагрузка. Пробуем сами
    сбросить кэш - может решить проблему без неё."""
    try:
        result = subprocess.run(
            ["resolvectl", "flush-caches"],
            capture_output=True, timeout=5,
        )
        if result.returncode == 0:
            logger.info("DNS отвечает сырыми IP, но не резолвит имена - сбросил кэш systemd-resolved")
        else:
            logger.debug("Попытка сбросить DNS-кэш не удалась: %s",
                         result.stderr.decode(errors="replace"))
    except (FileNotFoundError, subprocess.TimeoutExpired) as e:
        logger.debug("Не удалось выполнить resolvectl flush-caches: %s", e)


def has_internet_connectivity(timeout: float = 5.0) -> bool:
    """Проверка в два этапа - раньше проверяли только сырые IP (не требует
    DNS вообще), из-за чего программа считала интернет живым, пока DNS
    ещё не восстановился после сбоя, и потом минутами безуспешно пыталась
    достучаться до GitHub/пиров по именам. Теперь требуем ОБА условия:
    сырую IP-связность И работающий DNS - иначе для наших целей (поиск
    региона на GitHub, подключение к пирам по имени) толку от "интернета"
    всё равно нет."""
    raw_ip_ok = False
    for host, port in _INTERNET_CHECK_TARGETS:
        try:
            with socket.create_connection((host, port), timeout=timeout):
                raw_ip_ok = True
                break
        except OSError:
            continue

    if not raw_ip_ok:
        return False

    try:
        socket.getaddrinfo("api.github.com", 443, proto=socket.IPPROTO_TCP)
        return True
    except socket.gaierror:
        _try_flush_dns_cache()
        return False


def _try_restart_local_vpn() -> None:
    """Отдельный шанс на восстановление: если VPN-туннель на этой машине
    "залип" после сетевого сбоя - сырая связность и DNS уже могут
    работать, а собственно проксируемые TLS-соединения через сам туннель -
    всё ещё нет (наблюдалось на практике: GitHub не открывался часами,
    хотя интернет был явно жив).

    Сначала пробуем ПРОДВИНУТЫЙ хук (vpn_hook.py) - если пользователь
    одобрил свою команду (свой vpn-on/vpn-off и т.п.), дёргаем именно
    её. Если хук не настроен/не прошёл проверку целостности - откатываемся
    на БАЗОВЫЙ уровень: угадывание общеизвестных служб (xray/v2ver/
    sing-box), без вреда, если такой службы на машине просто нет."""
    if vh.run_approved_vpn_command(["restart"]):
        return

    for service in ("xray", "v2ray", "sing-box"):
        try:
            result = subprocess.run(
                ["systemctl", "restart", service],
                capture_output=True, timeout=10,
            )
            if result.returncode == 0:
                logger.info("GitHub недоступен несколько проверок подряд, хотя интернет вроде жив - "
                            "перезапустил службу '%s' на всякий случай", service)
                return
        except (FileNotFoundError, subprocess.TimeoutExpired):
            continue


def _handle_region_fetch_failure(state: "WatchdogState") -> None:
    state.consecutive_region_fetch_failures += 1
    if state.consecutive_region_fetch_failures >= 2:
        _try_restart_local_vpn()
        state.consecutive_region_fetch_failures = 0


def check_internet(state: WatchdogState) -> tuple[bool, Optional[Event]]:
    """Возвращает (можно_ли_продолжать_поиск_замены, событие_или_None).
    Ведёт учёт, когда именно пропал интернет (по локальным часам, они
    тикают независимо от наличия сети). При восстановлении формирует
    ОДНО событие с обоими точными временами (не просто длительность) -
    это то самое требование "в такое-то время пропал, в такое-то
    появился" в одном уведомлении, а не два отдельных."""
    ok = has_internet_connectivity()

    if not ok:
        if state.internet_down_since is None:
            state.internet_down_since = time.time()
            return False, Event(
                "internet_down",
                "Похоже, пропал интернет целиком (не только Yggdrasil-пиры). "
                "Поиск замены пирам не имеет смысла, пока нет сети вообще - жду восстановления.",
            )
        return False, None  # уже сообщили в прошлый раз, не спамим на каждый тик

    if state.internet_down_since is not None:
        down_dt = datetime.fromtimestamp(state.internet_down_since).astimezone()
        up_dt = datetime.now().astimezone()
        downtime = up_dt.timestamp() - down_dt.timestamp()
        state.internet_down_since = None
        minutes, seconds = divmod(int(downtime), 60)
        return True, Event(
            "internet_restored",
            f"Интернет пропадал с {down_dt.strftime('%H:%M:%S')} "
            f"до {up_dt.strftime('%H:%M:%S')} (простой {minutes} мин {seconds} сек).",
        )

    return True, None


# ------------------------------------------------------------------
# SOS - аварийный перебор ВСЕХ известных пиров (крайняя мера).
#
# Срабатывает только когда и основные, и региональные/egress-пиры
# недоступны, а остальная диагностика (интернет, DNS) выглядит здоровой -
# то есть похоже, что дело именно в достижимости конкретных пиров
# (DPI/блокировка), а не в отсутствии сети вообще.
#
# Ограничен ПО ВРЕМЕНИ за один тик (не по счётчику пиров) - проверка
# рукопожатия каждого кандидата может занимать до HANDSHAKE_TIMEOUT_SECONDS,
# полный перебор всех стран займёт намного больше одного тика. Курсор
# (индекс страны + индекс пира внутри неё) сохраняется в состоянии и
# переживает перезапуск программы.
#
# Снимок списка стран (sos_country_slugs) берётся ОДИН РАЗ в начале
# прохода - если база public-peers обновится посередине, текущий проход
# доканчивается по старому списку; новый список подхватится только на
# следующем проходе, после 12-часового кулдауна.
# ------------------------------------------------------------------
SOS_COOLDOWN_SECONDS = 12 * 3600
SOS_TICK_TIME_BUDGET_SECONDS = 90  # сколько максимум тратим на SOS-перебор за один тик


def _sos_cooldown_active(state: "WatchdogState") -> bool:
    if state.sos_last_full_scan_completed_at is None:
        return False
    return time.time() - state.sos_last_full_scan_completed_at < SOS_COOLDOWN_SECONDS


def sos_scan_step(state: "WatchdogState", exclude_hosts: set[str]) -> list[Event]:
    """Один шаг аварийного перебора. Возвращает события только для
    начала прохода, успешной находки и завершения прохода без успеха -
    промежуточный прогресс НЕ генерирует событие на каждый тик (иначе
    был бы спам в Telegram на протяжении, возможно, многих часов),
    только отметку в логе."""
    events: list[Event] = []

    if _sos_cooldown_active(state):
        logger.debug("SOS: кулдаун после последнего полного прохода ещё не истёк")
        return events

    if not state.sos_country_slugs:
        state.sos_country_slugs = pd.list_available_countries()
        state.sos_country_index = 0
        state.sos_peer_index = 0
        if not state.sos_country_slugs:
            logger.warning("SOS: не удалось получить список стран с GitHub - откладываю попытку")
            return events
        events.append(Event(
            "sos_started",
            f"Основные и региональные пиры недоступны - запускаю аварийный перебор "
            f"всех известных пиров ({len(state.sos_country_slugs)} стран). Это может занять часы.",
        ))

    deadline = time.monotonic() + SOS_TICK_TIME_BUDGET_SECONDS

    while time.monotonic() < deadline:
        if state.sos_country_index >= len(state.sos_country_slugs):
            state.sos_last_full_scan_completed_at = time.time()
            state.sos_country_slugs = []
            state.sos_country_index = 0
            state.sos_peer_index = 0
            events.append(Event(
                "sos_pass_exhausted",
                "Аварийный перебор прошёл ВСЕ известные пиры, рабочего не нашлось. "
                "Если интернет при этом явно есть - возможно, антивирус/файрвол "
                "блокирует исходящие соединения именно этой программы (проверь "
                "исключение для неё). Программа НЕ трогает файрвол сама - только "
                "диагностирует и сообщает, остальное - руками. "
                "Следующая попытка - не раньше чем через 12 часов.",
            ))
            return events

        country = state.sos_country_slugs[state.sos_country_index]
        try:
            peers = pd.fetch_country_peers(country)
        except pd.PeerDatabaseError as e:
            logger.debug("SOS: не удалось получить пиров страны '%s': %s", country, e)
            state.sos_country_index += 1
            state.sos_peer_index = 0
            continue

        candidates = [p for p in peers if p.host.lower() not in exclude_hosts]

        if state.sos_peer_index >= len(candidates):
            state.sos_country_index += 1
            state.sos_peer_index = 0
            continue

        peer = candidates[state.sos_peer_index]
        state.sos_peer_index += 1

        logger.info("SOS: проверяю %s (%s/%s)", peer.uri, peer.country_slug, peer.city)
        if pd.verify_peer_handshake(peer.uri):
            pd.add_peer(peer.uri)
            state.backup_peers.append(peer.uri)
            state.backup_region = peer.country_slug
            state.sos_country_slugs = []
            state.sos_country_index = 0
            state.sos_peer_index = 0
            events.append(Event(
                "sos_found",
                f"Аварийный перебор нашёл рабочего пира: {peer.uri} ({peer.country_slug}/{peer.city}).",
            ))
            return events

    logger.info(
        "SOS: бюджет времени на этот тик истёк, остановился на стране '%s' (пир №%d), продолжу в следующий раз",
        state.sos_country_slugs[state.sos_country_index], state.sos_peer_index,
    )
    return events


# ------------------------------------------------------------------
# Батарейный аварийный режим - проверяется ПЕРВЫМ, может оборвать тик
# сразу. OS-generic по смыслу (Windows и Linux - своя функция чтения
# статуса батареи, одна и та же логика входа/выхода).
# ------------------------------------------------------------------
def check_battery(state: "WatchdogState") -> tuple[bool, list[Event]]:
    """Возвращает (можно_ли_продолжать_тик, события). Вход - заряд <= 5%
    и НЕ на зарядке: выключает все остальные проверки до восстановления,
    одно уведомление. Выход - автоматический, по факту начала зарядки
    ИЛИ роста процента относительно момента входа, с симметричным
    уведомлением. Пока активен - не спамим повторными уведомлениями,
    просто молча ничего не делаем (кроме самой проверки заряда - она
    копеечная и нужна, чтобы вовремя заметить выход)."""
    status = bat.read_battery_status()
    if status is None:
        return True, []  # нет батареи на этой машине (стационарный сервер) - режим неприменим

    percent, charging = status

    if state.battery_critical_mode:
        exited = charging or (
            state.battery_critical_entered_percent is not None
            and percent > state.battery_critical_entered_percent
        )
        if not exited:
            return False, []
        state.battery_critical_mode = False
        state.battery_critical_entered_percent = None
        return True, [Event(
            "battery_critical_exited",
            f"Заряд {percent}% (зарядка: {'да' if charging else 'нет'}) - "
            f"аварийный режим по батарее выключен, проверки возобновлены.",
        )]

    if percent <= bat.CRITICAL_PERCENT and not charging:
        state.battery_critical_mode = True
        state.battery_critical_entered_percent = percent
        return False, [Event(
            "battery_critical_entered",
            f"Заряд {percent}% (не на зарядке) - вхожу в аварийный режим по батарее, "
            f"все проверки выключены до восстановления заряда.",
        )]

    return True, []


UPDATE_CHECK_INTERVAL_SECONDS = 24 * 3600  # раз в сутки, не на каждом тике


def check_for_update_if_due(state: "WatchdogState") -> list[Event]:
    """Только уведомляет - никогда сама не скачивает и не обновляет
    (это делает отдельная команда apply-update, по явному решению
    пользователя). Не повторяет уведомление про ту же версию дважды."""
    now = time.time()
    if (state.update_last_checked_at is not None
            and now - state.update_last_checked_at < UPDATE_CHECK_INTERVAL_SECONDS):
        return []

    state.update_last_checked_at = now
    info = au.check_for_update()
    if info is None:
        return []
    if info["tag"] == state.update_notified_tag:
        return []  # уже уведомляли про эту версию

    state.update_notified_tag = info["tag"]
    return [Event(
        "update_available",
        f"Доступна новая версия {info['tag']}: {info['url']}. "
        f"Чтобы обновиться - команда apply-update (или пункт меню).",
    )]


# ------------------------------------------------------------------
# Основная функция одного цикла проверки
# ------------------------------------------------------------------
def tick(config_path: Path, state: WatchdogState) -> list[Event]:
    """Один цикл проверки. Возвращает список событий (для очереди
    уведомлений) и мутирует state на месте (вызывающий код должен
    сохранить его после вызова через save_state)."""
    events: list[Event] = []
    logger.debug("=== tick начат, режим=%s, конфиг=%s ===", state.mode, config_path)

    battery_ok, battery_events = check_battery(state)
    events.extend(battery_events)
    if not battery_ok:
        logger.debug("Аварийный режим по батарее активен - дальше не проверяю")
        return events

    events.extend(check_for_update_if_due(state))

    # Если ранее уже зафиксировали пропажу интернета - в первую очередь
    # проверяем, не появился ли он снова, НЕЗАВИСИМО от текущего
    # состояния пиров. Иначе есть риск, что пиры сами восстановятся
    # раньше, чем мы успеем это заметить в обычной ветке ниже (она
    # проверяется только когда пиры ВСЕ ещё лежат) - и флаг простоя
    # останется висеть навсегда, а уведомление "интернет вернулся"
    # так и не придёт.
    if state.internet_down_since is not None:
        logger.debug("Ранее зафиксирован простой интернета - проверяю восстановление")
        internet_ok, internet_event = check_internet(state)
        if internet_event:
            events.append(internet_event)
        if not internet_ok:
            logger.debug("Интернета всё ещё нет, дальше не проверяю")
            return events  # всё ещё нет интернета - дальше проверять нечего

    main_peers = read_main_peers(config_path)
    live = pd.get_live_peers()
    logger.debug("Основные пиры из конфига (%d): %s", len(main_peers), main_peers)
    logger.debug("Живой getPeers (%d записей): %s", len(live), live)

    def is_up(uri: str) -> bool:
        return live.get(uri, "").lower() == "up"

    # Level 2 - мониторинг устройств, ТОЛЬКО если основная связность
    # (главные/резервные для normal, региональные для regional) уже
    # подтверждена здоровой ДО всей остальной логики этого тика - не
    # дублирует то, что Level 1 и так проверяет.
    if state.mode == "regional":
        connectivity_healthy = any(is_up(uri) for uri in state.regional_peers)
    else:
        connectivity_healthy = (any(is_up(uri) for uri in main_peers)
                                 or any(is_up(uri) for uri in state.backup_peers))

    if connectivity_healthy and state.devices:
        for kind, message in dm.check_devices(
            state.devices, state.device_down_since, state.device_escalated_until,
        ):
            events.append(Event(kind, message))

    if state.mode == "regional":
        # Правило 4: основные не проверяем вообще, только региональные
        if not state.regional_peers:
            logger.warning("Режим 'regional' активен, но список региональных пиров пуст")
            return events

        if any(is_up(uri) for uri in state.regional_peers):
            return events  # хотя бы один региональный жив - всё в порядке

        # Все региональные легли - прежде чем искать замену, проверяем,
        # не пропал ли интернет вообще (искать на GitHub без сети бессмысленно)
        internet_ok, internet_event = check_internet(state)
        if internet_event:
            events.append(internet_event)
        if not internet_ok:
            return events

        exclude = {h for uri in (main_peers + state.backup_peers + state.regional_peers)
                   if (h := _host_from_uri(uri))}

        for spec in state.regional_countries:
            country, city = _parse_region_spec(spec)
            candidates = _find_region_candidates(country, city, exclude, count=1)
            for peer in candidates:
                pd.add_peer(peer.uri)
                state.regional_peers.append(peer.uri)
                events.append(Event(
                    "regional_replaced",
                    f"Все региональные пиры ({', '.join(state.regional_countries)}) легли. "
                    f"Добавлен новый: {peer.uri} ({peer.city})",
                ))
                return events  # одного найденного достаточно за один тик

        events.append(Event(
            "regional_all_down",
            f"Все региональные пиры ({', '.join(state.regional_countries)}) легли, "
            f"замену найти не удалось.",
        ))
        return events

    # mode == "normal"
    if any(is_up(uri) for uri in main_peers):
        if state.backup_peers:
            # Правило 2: хотя бы один основной поднялся - убрать все резервные
            removed = list(state.backup_peers)
            for uri in removed:
                pd.remove_peer(uri)
            state.backup_peers = []
            state.backup_region = None
            events.append(Event(
                "backup_removed",
                f"Основной пир снова в строю. Удалены резервные: {', '.join(removed)}",
            ))
        return events

    if any(is_up(uri) for uri in state.backup_peers):
        return events  # резервный держит связь, основные ещё лежат - ждём

    # Все основные и все резервные лежат - правило 1, но сначала
    # проверяем, не пропал ли интернет вообще
    internet_ok, internet_event = check_internet(state)
    if internet_event:
        events.append(internet_event)
    if not internet_ok:
        return events

    region = get_backup_search_region(state)
    if region is None:
        _handle_region_fetch_failure(state)
        events.append(Event(
            "main_down_no_region",
            "Все основные пиры лежат, не удалось определить регион для поиска замены "
            "(egress-регион не определился, а домашний регион не задан - "
            "задай его командой set-home-region).",
        ))
        return events

    state.consecutive_region_fetch_failures = 0

    exclude = {h for uri in (main_peers + state.backup_peers) if (h := _host_from_uri(uri))}
    candidates = pd.pick_replacement_peers(region, exclude_hosts=exclude, count=1)

    if candidates:
        peer = candidates[0]
        pd.add_peer(peer.uri)
        state.backup_peers.append(peer.uri)
        state.backup_region = region
        events.append(Event(
            "backup_added",
            f"Все основные пиры ({', '.join(main_peers)}) легли. "
            f"Добавлен резервный ({region}): {peer.uri} ({peer.city})",
        ))
    else:
        events.append(Event(
            "main_down_no_replacement",
            f"Все основные пиры легли, замену в регионе '{region}' найти не удалось. "
            f"Пробую аварийный перебор всех известных пиров.",
        ))
        events.extend(sos_scan_step(state, exclude))

    return events


# ------------------------------------------------------------------
# Ручные команды (региональный режим)
# ------------------------------------------------------------------
def _parse_region_spec(spec: str) -> tuple[str, Optional[str]]:
    """'russia' -> ('russia', None); 'russia/Vladivostok' -> ('russia', 'Vladivostok').
    Раздельный город нужен для больших стран, где пиры в разных городах
    физически далеко друг от друга (Россия, США, Канада и т.п.)."""
    if "/" in spec:
        country, city = spec.split("/", 1)
        return country.strip(), city.strip()
    return spec.strip(), None


def _find_region_candidates(country: str, city: Optional[str], exclude_hosts: set, count: int = 1) -> list:
    """Если для страны есть ISO-код и указанный город реально нашёлся в
    базе координат - ищем БЛИЖАЙШЕГО пира по расстоянию (настоящий
    подбор). Если города нет или он не найден в базе - откатываемся на
    старый способ (просто пиры страны, без учёта близости)."""
    if city:
        iso = pd.slug_to_iso(country)
        if iso:
            db = pd.load_city_database()
            city_obj = pd.find_city(db, iso, city)
            if city_obj:
                return pd.find_nearest_working_peer(country, city_obj, exclude_hosts, count=count)
    return pd.pick_replacement_peers(country, exclude_hosts=exclude_hosts, count=count, city=city)


def cmd_add_region(state: WatchdogState, countries: list[str]) -> list[Event]:
    """Переключает в региональный режим и делает первичный поиск пиров.
    Каждый элемент countries - это 'страна' или 'страна/город'
    (например 'russia' или 'russia/Vladivostok'). Не трогает основные
    и резервные - они просто перестают проверяться, пока региональный
    режим активен (правило 4).

    Накопительно: если региональный режим уже активен и в нём уже есть
    какие-то регионы, новые добавляются к ним, а не заменяют список -
    так можно вызывать эту команду несколько раз подряд (например, для
    интерактивного добавления регионов по одному через меню), не теряя
    уже настроенное."""
    events: list[Event] = []
    state.mode = "regional"
    for spec in countries:
        if spec not in state.regional_countries:
            state.regional_countries.append(spec)

    exclude = {h for uri in state.regional_peers if (h := _host_from_uri(uri))}
    for spec in countries:
        country, city = _parse_region_spec(spec)
        candidates = _find_region_candidates(country, city, exclude, count=1)
        for peer in candidates:
            pd.add_peer(peer.uri)
            state.regional_peers.append(peer.uri)
            events.append(Event(
                "regional_added",
                f"Добавлен региональный пир ({spec}): {peer.uri} ({peer.city})",
            ))

    if not any(e.kind == "regional_added" for e in events):
        events.append(Event(
            "regional_none_found",
            f"Не удалось найти ни одного рабочего пира для: {', '.join(countries)}",
        ))

    return events


def cmd_remove_region(state: WatchdogState) -> list[Event]:
    """Явная команда снятия региональных пиров - только так они и
    убираются (правило 5), никогда автоматически."""
    events: list[Event] = []
    removed = list(state.regional_peers)
    for uri in removed:
        pd.remove_peer(uri)

    events.append(Event(
        "regional_removed",
        f"Региональные пиры сняты вручную: {', '.join(removed) if removed else '(список был пуст)'}",
    ))

    state.mode = "normal"
    state.regional_peers = []
    state.regional_countries = []
    return events


def cmd_set_home_region(state: WatchdogState, country_slug: Optional[str]) -> list[Event]:
    """Задаёт домашний регион (slug из репозитория public-peers, например
    'russia') - запасной вариант для поиска резервных пиров, когда
    egress-регион через GeoIP определить не удалось (все провайдеры
    недоступны, или это вообще первый запуск программы). country_slug=None
    снимает домашний регион (явная команда, как и для прочих ручных
    настроек в этом модуле)."""
    if country_slug is None:
        old = state.home_region
        state.home_region = None
        return [Event("home_region_cleared", f"Домашний регион снят (был: {old or '(не задан)'})")]

    slug = country_slug.strip().lower()
    if pd.slug_to_iso(slug) is None:
        return [Event(
            "home_region_invalid",
            f"'{country_slug}' не похож на поддерживаемый slug страны репозитория "
            f"public-peers (посмотри list-countries).",
        )]

    state.home_region = slug
    return [Event("home_region_set", f"Домашний регион установлен: {slug}")]


# ------------------------------------------------------------------
# Ручное управление списком мониторимых устройств (Level 2)
# ------------------------------------------------------------------
def cmd_add_device(state: WatchdogState, name: str, address: str) -> list[Event]:
    state.devices[name] = address
    state.device_down_since.pop(name, None)
    state.device_escalated_until.pop(name, None)
    return [Event("device_added", f"Устройство '{name}' ({address}) добавлено в мониторинг.")]


def cmd_remove_device(state: WatchdogState, name: str) -> list[Event]:
    if name not in state.devices:
        return [Event("device_not_found", f"Устройство '{name}' не найдено в списке.")]
    address = state.devices.pop(name)
    state.device_down_since.pop(name, None)
    state.device_escalated_until.pop(name, None)
    return [Event("device_removed", f"Устройство '{name}' ({address}) убрано из мониторинга.")]

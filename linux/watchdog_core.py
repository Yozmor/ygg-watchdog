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

import peer_database as pd

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

    def to_dict(self) -> dict:
        return {
            "mode": self.mode,
            "backup_peers": self.backup_peers,
            "backup_region": self.backup_region,
            "regional_peers": self.regional_peers,
            "regional_countries": self.regional_countries,
            "internet_down_since": self.internet_down_since,
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
# Определение региона основных пиров (для поиска резервных того же региона)
# ------------------------------------------------------------------
def derive_backup_region(main_peer_uris: list[str]) -> Optional[str]:
    for uri in main_peer_uris:
        host = _host_from_uri(uri)
        if not host:
            continue
        region = pd.find_country_of_host(host)
        if region:
            return region
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
# Основная функция одного цикла проверки
# ------------------------------------------------------------------
def tick(config_path: Path, state: WatchdogState) -> list[Event]:
    """Один цикл проверки. Возвращает список событий (для очереди
    уведомлений) и мутирует state на месте (вызывающий код должен
    сохранить его после вызова через save_state)."""
    events: list[Event] = []
    logger.debug("=== tick начат, режим=%s, конфиг=%s ===", state.mode, config_path)

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

    region = state.backup_region or derive_backup_region(main_peers)
    if region is None:
        events.append(Event(
            "main_down_no_region",
            "Все основные пиры лежат, не удалось определить регион для поиска замены.",
        ))
        return events

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
            f"Все основные пиры легли, замену в регионе '{region}' найти не удалось.",
        ))

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

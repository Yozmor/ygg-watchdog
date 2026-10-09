"""
peer_database.py

Модуль работы с базой публичных пиров Yggdrasil.

Источник данных - официальный GitHub-репозиторий yggdrasil-network/public-peers
(структура: континент/страна.md, markdown-заголовки городов, пиры в виде
`протокол://хост:порт` в обратных кавычках). Список стран внутри континента
получаем через GitHub Contents API динамически, а не хардкодим - переживёт
добавление/удаление стран и городов без правок кода.

Никакого дискового кэша базы пиров нет - каждый раз, когда нужен кандидат
на замену, список скачивается заново с GitHub. Это сознательный компромисс
за простоту; единственная цена - если в момент реального сбоя окажется
недоступен ещё и сам GitHub, резервных данных не будет вообще.

Живость кандидата проверяется НАСТОЯЩИМ Yggdrasil-рукопожатием: пир
добавляется через runtime-команду `yggdrasilctl addPeer` (не требует
редактирования конфига и перезапуска демона - остальные уже работающие
соединения не затрагиваются), затем опрашивается `getPeers`, пока
состояние не станет Up либо не истечёт таймаут. Простой TCP-коннект
не используется - открытый порт ещё не значит, что Yggdrasil-протокол
там реально проходит (DPI может резать именно рукопожатие).
"""

import json
import logging
import re
import subprocess
import sys
import time
import urllib.request
import urllib.error
from dataclasses import dataclass
from math import radians, sin, cos, atan2, sqrt
from pathlib import Path
from typing import Optional

logger = logging.getLogger("ygg_watchdog.peer_database")

GITHUB_API_BASE = "https://api.github.com/repos/yggdrasil-network/public-peers"
GITHUB_RAW_BASE = "https://raw.githubusercontent.com/yggdrasil-network/public-peers/master"

CONTINENTS = [
    "africa", "asia", "europe", "mena",
    "north-america", "oceania", "other", "south-america",
]

HTTP_TIMEOUT_SECONDS = 10

# Путь к yggdrasilctl - по умолчанию полагаемся на PATH (так и на Linux,
# и через systemd, если PATH настроен явно в unit-файле службы watchdog).
# Путь к yggdrasilctl и дополнительные аргументы для него - разные на
# Windows и Linux. На Windows дефолтный порт админ-сокета (9001) может
# конфликтовать со слушателем входящих Yggdrasil-пиров при резолвинге
# "localhost" в IPv6 (подробности - см. историю фикса AdminListen в
# yggdrasil.conf); поэтому там используется отдельный порт 9002 через
# явный -endpoint. Поправь константы ниже, если у тебя порт другой
# или yggdrasilctl.exe лежит не в стандартном месте.
if sys.platform == "win32":
    YGGCTL_PATH = r"C:\Program Files\Yggdrasil\yggdrasilctl.exe"
    YGGCTL_EXTRA_ARGS = ["-endpoint=tcp://127.0.0.1:9002"]
else:
    YGGCTL_PATH = "yggdrasilctl"
    YGGCTL_EXTRA_ARGS = []

HANDSHAKE_TIMEOUT_SECONDS = 15    # сколько ждём, пока пир станет Up
HANDSHAKE_POLL_INTERVAL_SECONDS = 1.5


class PeerDatabaseError(Exception):
    pass


@dataclass
class Peer:
    uri: str
    country_slug: str  # имя файла без .md, например "russia"
    city: str
    protocol: str
    host: str
    port: int


# ------------------------------------------------------------------
# Разбор URI пира
# ------------------------------------------------------------------
_URI_RE = re.compile(
    r"^(?P<protocol>tcp|tls|quic|ws|wss|socks|sockstls)://"
    r"(?:\[(?P<host6>[^\]]+)\]|(?P<host4>[^:/?]+))"
    r":(?P<port>\d+)"
)


def parse_uri(uri: str) -> Optional[tuple[str, str, int]]:
    m = _URI_RE.match(uri.strip())
    if not m:
        return None
    host = m.group("host6") or m.group("host4")
    return m.group("protocol"), host, int(m.group("port"))


# ------------------------------------------------------------------
# Разбор markdown-файла страны
# ------------------------------------------------------------------
_HEADING_RE = re.compile(r"^\s*#{1,6}\s+(.+?)\s*$")
_URI_LINE_RE = re.compile(r"`((?:tcp|tls|quic|ws|wss|socks|sockstls)://[^`]+)`")


def parse_country_markdown(text: str, country_slug: str) -> list[Peer]:
    """Структурный разбор: заголовок любого уровня - название города,
    строки с URI в обратных кавычках после него - пиры этого города.
    Не привязан к конкретным названиям - переживёт изменения контента."""
    peers: list[Peer] = []
    current_city = "Unknown"

    for line in text.splitlines():
        heading_match = _HEADING_RE.match(line)
        if heading_match:
            current_city = heading_match.group(1)
            continue

        for uri_match in _URI_LINE_RE.finditer(line):
            uri = uri_match.group(1)
            parsed = parse_uri(uri)
            if parsed is None:
                logger.debug("Пропускаю нераспознанный URI: %s", uri)
                continue
            protocol, host, port = parsed
            peers.append(Peer(
                uri=uri, country_slug=country_slug, city=current_city,
                protocol=protocol, host=host, port=port,
            ))

    return peers


# ------------------------------------------------------------------
# Сеть: GitHub
# ------------------------------------------------------------------
def _http_get(url: str, as_json: bool = True):
    req = urllib.request.Request(url, headers={
        "User-Agent": "ygg-watchdog/1.0",
        "Accept": "application/vnd.github+json" if as_json else "*/*",
    })
    try:
        with urllib.request.urlopen(req, timeout=HTTP_TIMEOUT_SECONDS) as resp:
            raw = resp.read()
    except (urllib.error.URLError, TimeoutError, OSError) as e:
        raise PeerDatabaseError(f"Не удалось обратиться к {url}: {e}") from e

    if not as_json:
        return raw.decode("utf-8", errors="replace")
    try:
        return json.loads(raw.decode("utf-8"))
    except json.JSONDecodeError as e:
        raise PeerDatabaseError(f"Некорректный JSON от {url}: {e}") from e


def list_country_files() -> dict[str, str]:
    """{country_slug: raw_url} для всех .md файлов во всех континентах.

    Один запрос к git/trees (recursive=1) вместо восьми отдельных к
    Contents API (по одному на континент) - у GitHub жёсткий лимит на
    анонимные запросы (~60/час на IP), и восемь запросов за один вызов
    этой функции быстро съедали бы его при частых проверках watchdog
    во время реального затяжного сбоя пиров - то есть именно тогда,
    когда база пиров нужнее всего."""
    url = f"{GITHUB_API_BASE}/git/trees/master?recursive=1"
    try:
        data = _http_get(url)
    except PeerDatabaseError as e:
        logger.warning("Не удалось получить дерево репозитория: %s", e)
        return {}

    result: dict[str, str] = {}
    for item in data.get("tree", []):
        path = item.get("path", "")
        if item.get("type") != "blob" or not path.endswith(".md"):
            continue
        parts = path.split("/")
        if len(parts) != 2 or parts[0] not in CONTINENTS:
            continue
        continent, filename = parts
        slug = filename[:-3].lower()
        result[slug] = f"{GITHUB_RAW_BASE}/{path}"

    return result


def fetch_country_peers(country_slug: str) -> list[Peer]:
    """Скачивает и разбирает markdown-файл ОДНОЙ конкретной страны -
    без построения индекса по всем странам сразу (раз дискового кэша
    больше нет, незачем качать 190+ файлов ради одной страны)."""
    country_files = list_country_files()
    raw_url = country_files.get(country_slug.lower())
    if raw_url is None:
        raise PeerDatabaseError(f"Страна '{country_slug}' не найдена в репозитории")

    text = _http_get(raw_url, as_json=False)
    return parse_country_markdown(text, country_slug.lower())


def list_available_countries() -> list[str]:
    return sorted(list_country_files().keys())


def find_country_of_host(host: str) -> Optional[str]:
    """Ищет страну уже настроенного пира, перебирая country-файлы одного
    континента за раз, пока не найдёт совпадение по хосту. Дороже, чем
    было бы с кэшем, но кэша больше нет - это осознанный компромисс."""
    host_lower = host.lower()
    country_files = list_country_files()
    for slug, raw_url in country_files.items():
        try:
            text = _http_get(raw_url, as_json=False)
        except PeerDatabaseError:
            continue
        for peer in parse_country_markdown(text, slug):
            if peer.host.lower() == host_lower:
                return slug
    return None


# ------------------------------------------------------------------
# Взаимодействие с yggdrasilctl - настоящая проверка рукопожатия
# ------------------------------------------------------------------
def run_yggctl(args: list[str]) -> str:
    try:
        result = subprocess.run(
            [YGGCTL_PATH] + YGGCTL_EXTRA_ARGS + args,
            capture_output=True, text=True, timeout=10,
        )
    except (FileNotFoundError, subprocess.TimeoutExpired) as e:
        raise PeerDatabaseError(f"Не удалось выполнить yggdrasilctl {args}: {e}") from e
    return (result.stdout or "") + (result.stderr or "")


def add_peer(uri: str) -> None:
    run_yggctl(["addPeer", f"uri={uri}"])


def remove_peer(uri: str) -> None:
    run_yggctl(["removePeer", f"uri={uri}"])


_LIVE_URI_SEARCH_RE = re.compile(
    r"(?:tcp|tls|quic|ws|wss|socks|sockstls)://(?:\[[^\]]+\]|[^\s│:/?]+):\d+"
)


def get_peer_state(uri: str) -> Optional[str]:
    """Возвращает состояние (Up/Down/...) для конкретного URI из getPeers,
    или None, если такой пир вообще не найден в выводе."""
    output = run_yggctl(["getPeers"])

    for raw_line in output.splitlines():
        line = raw_line.strip()
        if uri not in line:
            continue
        remainder = line.split(uri, 1)[1]
        tokens = [t for t in re.split(r"[│\s]+", remainder) if t]
        if tokens:
            return tokens[0]
    return None


def get_live_peers() -> dict[str, str]:
    """Возвращает {uri: state} для ВСЕХ пиров, которые сейчас числятся
    в живом getPeers (и основные из конфига, и любые runtime-добавленные -
    getPeers их не различает, различение - забота вызывающего кода)."""
    output = run_yggctl(["getPeers"])
    result: dict[str, str] = {}

    for raw_line in output.splitlines():
        line = raw_line.strip()
        match = _LIVE_URI_SEARCH_RE.search(line)
        if not match:
            continue
        uri = match.group(0)
        remainder = line[match.end():]
        tokens = [t for t in re.split(r"[│\s]+", remainder) if t]
        if tokens:
            result[uri] = tokens[0]

    return result


def verify_peer_handshake(
    uri: str,
    timeout: float = HANDSHAKE_TIMEOUT_SECONDS,
    poll_interval: float = HANDSHAKE_POLL_INTERVAL_SECONDS,
) -> bool:
    """Настоящая проверка: добавляет пира через runtime-команду addPeer,
    ждёт, пока getPeers не покажет состояние Up, затем убирает пира
    обратно (removePeer выполняется всегда, даже при ошибке - в finally),
    поскольку это проверочное добавление, а не постоянное решение."""
    add_peer(uri)
    try:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            state = get_peer_state(uri)
            if state and state.lower() == "up":
                return True
            time.sleep(poll_interval)
        return False
    finally:
        remove_peer(uri)


# ------------------------------------------------------------------
# Основная точка входа
# ------------------------------------------------------------------
def list_cities(country_slug: str) -> list[str]:
    """Список уникальных городов/регионов внутри одной страны, в том
    порядке, в котором они встречаются в файле (обычно от крупных
    к менее крупным). Полезно для больших стран (Россия, США, Канада),
    где пиры в разных городах физически далеко друг от друга."""
    peers = fetch_country_peers(country_slug)
    seen: list[str] = []
    for p in peers:
        if p.city not in seen:
            seen.append(p.city)
    return seen


def pick_replacement_peers(
    country_slug: str,
    exclude_hosts: set[str],
    count: int = 2,
    verify_handshake: bool = True,
    city: Optional[str] = None,
) -> list[Peer]:
    """Возвращает до `count` пиров указанной страны, исключая уже
    настроенные хосты. Если verify_handshake=True (по умолчанию) -
    реально проверяет каждого кандидата через живое Yggdrasil-рукопожатие
    (addPeer -> ждём Up -> removePeer), а не просто TCP-коннектом.

    Если задан city - сначала пробует пиров именно этого города/региона
    (сравнение без учёта регистра), и только если таких не нашлось совсем -
    откатывается на всю страну целиком (лучше более дальний, но живой
    пир, чем полное отсутствие замены)."""
    all_peers = fetch_country_peers(country_slug)
    candidates = [p for p in all_peers if p.host.lower() not in exclude_hosts]

    if city:
        city_lower = city.lower()
        city_candidates = [p for p in candidates if p.city.lower() == city_lower]
        if city_candidates:
            candidates = city_candidates
        else:
            logger.warning(
                "Город '%s' не найден среди пиров '%s' (или все уже исключены) - "
                "ищу по всей стране", city, country_slug,
            )

    if not verify_handshake:
        return candidates[:count]

    result: list[Peer] = []
    for peer in candidates:
        if len(result) >= count:
            break
        logger.info("Проверяю рукопожатие: %s", peer.uri)
        if verify_peer_handshake(peer.uri):
            result.append(peer)
            logger.info("  Up - подходит")
        else:
            logger.info("  не поднялся за отведённое время, пропускаю")

    return result


# ------------------------------------------------------------------
# База городов (GeoNames cities15000, обработанная в компактный формат:
# country_code\tname\tlat\tlon\tpopulation\tname_ru). Файл cities.dat
# должен лежать рядом с этим модулем. Русские названия проверены вручную
# для ~50 крупных городов - для остальных используется английское имя
# (автоматическая эвристика перевода давала слишком много ошибок на
# масштабе тысяч городов, честнее показывать английское, чем неверное).
# ------------------------------------------------------------------
@dataclass
class City:
    country_code: str
    name: str
    lat: float
    lon: float
    population: int
    name_ru: str = ""

    def display_name(self) -> str:
        return self.name_ru or self.name

    def display_label(self) -> str:
        return f"{self.name} ({self.name_ru})" if self.name_ru else self.name


_cities_cache: Optional[list[City]] = None


def load_city_database() -> list[City]:
    global _cities_cache
    if _cities_cache is not None:
        return _cities_cache

    path = Path(__file__).resolve().parent / "cities.dat"
    if not path.exists():
        logger.error("Не найден cities.dat рядом с модулем - региональный режим не сможет искать по координатам")
        _cities_cache = []
        return _cities_cache

    cities = []
    for line in path.read_text(encoding="utf-8").splitlines():
        parts = line.split("\t")
        if len(parts) < 5:
            continue
        try:
            lat = float(parts[2])
            lon = float(parts[3])
            pop = int(parts[4])
        except ValueError:
            continue
        name_ru = parts[5] if len(parts) >= 6 else ""
        cities.append(City(country_code=parts[0], name=parts[1], lat=lat, lon=lon, population=pop, name_ru=name_ru))

    _cities_cache = cities
    logger.debug("Загружено городов из cities.dat: %d", len(cities))
    return cities


def haversine_distance_km(lat1: float, lon1: float, lat2: float, lon2: float) -> float:
    r = 6371.0
    d_lat = radians(lat2 - lat1)
    d_lon = radians(lon2 - lon1)
    a = sin(d_lat / 2) ** 2 + cos(radians(lat1)) * cos(radians(lat2)) * sin(d_lon / 2) ** 2
    c = 2 * atan2(sqrt(a), sqrt(1 - a))
    return r * c


def find_city(db: list[City], country_code: str, city_name: str) -> Optional[City]:
    for c in db:
        if c.country_code.lower() == country_code.lower() and c.name.lower() == city_name.lower():
            return c
    return None


def get_cities_for_country(db: list[City], iso: str) -> list[City]:
    """Всегда сортируем по английскому названию (единый ключ) - большинство
    городов не имеют проверенного русского названия, сортировка по
    отображаемому имени перемешивала бы алфавиты."""
    filtered = [c for c in db if c.country_code.lower() == iso.lower()]
    filtered.sort(key=lambda c: c.name)
    return filtered


# Соответствие slug'ов из репозитория GitHub кодам стран ISO 3166-1
# alpha-2. Только те 40 стран, что реально есть в репозитории на момент
# написания - i2p/lokinet/tor не страны, а отдельные оверлей-сети.
COUNTRY_SLUG_TO_ISO = {
    "armenia": "AM", "australia": "AU", "austria": "AT",
    "brazil": "BR", "canada": "CA", "chile": "CL",
    "czechia": "CZ", "finland": "FI", "france": "FR",
    "germany": "DE", "hong-kong": "HK", "hungary": "HU",
    "india": "IN", "indonesia": "ID", "japan": "JP",
    "latvia": "LV", "luxembourg": "LU", "moldova": "MD",
    "netherlands": "NL", "new-zealand": "NZ", "norway": "NO",
    "philippines": "PH", "poland": "PL", "romania": "RO",
    "russia": "RU", "saudi-arabia": "SA", "singapore": "SG",
    "slovakia": "SK", "south-africa": "ZA", "spain": "ES",
    "sweden": "SE", "switzerland": "CH", "taiwan": "TW",
    "turkey": "TR", "ukraine": "UA", "united-kingdom": "GB",
    "united-states": "US",
}


def slug_to_iso(slug: str) -> Optional[str]:
    return COUNTRY_SLUG_TO_ISO.get(slug.lower())


# Русские названия для тех же стран - проверено вручную (названия стран
# однозначны, не как города).
COUNTRY_SLUG_TO_RU = {
    "armenia": "Армения", "australia": "Австралия", "austria": "Австрия",
    "brazil": "Бразилия", "canada": "Канада", "chile": "Чили",
    "czechia": "Чехия", "finland": "Финляндия", "france": "Франция",
    "germany": "Германия", "hong-kong": "Гонконг", "hungary": "Венгрия",
    "india": "Индия", "indonesia": "Индонезия", "japan": "Япония",
    "latvia": "Латвия", "luxembourg": "Люксембург", "moldova": "Молдова",
    "netherlands": "Нидерланды", "new-zealand": "Новая Зеландия", "norway": "Норвегия",
    "philippines": "Филиппины", "poland": "Польша", "romania": "Румыния",
    "russia": "Россия", "saudi-arabia": "Саудовская Аравия", "singapore": "Сингапур",
    "slovakia": "Словакия", "south-africa": "ЮАР", "spain": "Испания",
    "sweden": "Швеция", "switzerland": "Швейцария", "taiwan": "Тайвань",
    "turkey": "Турция", "ukraine": "Украина", "united-kingdom": "Великобритания",
    "united-states": "США",
}


def slug_to_russian_name(slug: str) -> str:
    return COUNTRY_SLUG_TO_RU.get(slug.lower(), slug)


@dataclass
class CountryEntry:
    slug: str
    ru_name: str
    supported: bool


def get_sorted_country_list() -> list[CountryEntry]:
    """Живой список стран с GitHub, отсортированный по русскому названию.
    И показ списка, и разрешение введённого номера должны использовать
    ИМЕННО эту функцию - чтобы порядок гарантированно совпадал между
    отдельными вызовами (список показывается один раз, а номер
    разрешается в отдельном процессе позже)."""
    files = list_country_files()
    entries = [
        CountryEntry(slug=slug, ru_name=slug_to_russian_name(slug), supported=slug_to_iso(slug) is not None)
        for slug in files.keys()
    ]
    entries.sort(key=lambda e: e.ru_name)
    return entries


def find_nearest_working_peer(
    country_slug: str,
    dest_city: "City",
    exclude_hosts: set[str],
    count: int = 1,
) -> list[Peer]:
    """Ищет пиров страны, сортирует по расстоянию до dest_city (по
    координатам), проверяет ближайших первыми настоящим рукопожатием.
    Пиры, чей город не нашёлся в базе координат, уходят в конец очереди
    (не выбрасываются совсем - лучше дальний, но живой, чем никакого)."""
    candidates = fetch_country_peers(country_slug)
    candidates = [p for p in candidates if p.host.lower() not in exclude_hosts]

    db = load_city_database()
    iso = slug_to_iso(country_slug)

    scored = []
    for p in candidates:
        peer_city = find_city(db, iso, p.city) if iso else None
        if peer_city:
            dist = haversine_distance_km(dest_city.lat, dest_city.lon, peer_city.lat, peer_city.lon)
        else:
            dist = 1e9
        scored.append((dist, p))
    scored.sort(key=lambda x: x[0])

    result: list[Peer] = []
    for dist, p in scored:
        if len(result) >= count:
            break
        dist_text = "город не найден в базе" if dist >= 1e8 else f"{dist:.0f} км"
        logger.info("Проверяю рукопожатие: %s (%s, %s)", p.uri, p.city, dist_text)
        if verify_peer_handshake(p.uri):
            result.append(p)
            logger.info("  Up - подходит")
        else:
            logger.info("  не поднялся за отведённое время, пробую следующего")

    return result

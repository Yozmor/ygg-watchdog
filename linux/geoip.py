"""
geoip.py

Определение страны текущей точки выхода трафика (egress-региона) -
нужно, чтобы подбирать резервных пиров из того региона, откуда СЕЙЧАС
реально уходит трафик (после VPN, если он включён и держит дефолтный
маршрут), а не из региона основных пиров. Прежняя логика
(derive_backup_region в watchdog_core.py, по географии главных пиров)
была ошибочной именно в этом: если основные пиры недоступны из-за
VPN - искать замену по ИХ региону бессмысленно, у egress сейчас другая
точка выхода.

Цепочка резервов:
1. Несколько онлайн GeoIP-провайдеров по очереди (первый ответивший
   побеждает) - ip-api.com, ipinfo.io, ipapi.co.
2. Если все недоступны - возвращаем None, вызывающий код (watchdog_core.py)
   откатывается на домашний регион, заданный пользователем руками.

Офлайн-база (MaxMind GeoLite2 и т.п.) сознательно НЕ реализована - при
частоте обращений (раз в час + при смене VPN) это лишняя сложность,
не оправданная выгодой. Если практика покажет, что все три онлайн-
провайдера регулярно недоступны одновременно - добавить офлайн-базу
будет несложно, не меняя остальной код (просто ещё один шаг в цепочке
_PROVIDERS с другой сигнатурой получения).
"""

import json
import logging
import urllib.error
import urllib.request
from typing import Optional

logger = logging.getLogger("ygg_watchdog.geoip")

HTTP_TIMEOUT_SECONDS = 6

# Каждый провайдер: (url, функция извлечения ISO 3166-1 alpha-2 кода страны
# из распарсенного JSON-ответа). Порядок - по убыванию приоритета.
_PROVIDERS: list[tuple[str, "callable"]] = [
    ("http://ip-api.com/json/?fields=countryCode", lambda d: d.get("countryCode")),
    ("https://ipinfo.io/json", lambda d: d.get("country")),
    ("https://ipapi.co/json/", lambda d: d.get("country_code")),
]


def _http_get_json(url: str) -> Optional[dict]:
    req = urllib.request.Request(url, headers={"User-Agent": "ygg-watchdog/1.0"})
    try:
        with urllib.request.urlopen(req, timeout=HTTP_TIMEOUT_SECONDS) as resp:
            return json.loads(resp.read().decode("utf-8"))
    except (urllib.error.URLError, TimeoutError, OSError, json.JSONDecodeError, ValueError) as e:
        logger.debug("GeoIP-провайдер %s недоступен: %s", url, e)
        return None


def detect_egress_country_iso() -> Optional[str]:
    """Возвращает ISO 3166-1 alpha-2 код страны текущей точки выхода
    трафика (например "RU", "DE"), или None, если ни один из провайдеров
    не ответил - в этом случае вызывающий код должен откатиться на
    домашний регион."""
    for url, extract in _PROVIDERS:
        data = _http_get_json(url)
        if data is None:
            continue
        try:
            iso = extract(data)
        except Exception:
            iso = None
        if iso and isinstance(iso, str) and len(iso) == 2:
            iso = iso.upper()
            logger.debug("Egress-регион определён через %s: %s", url, iso)
            return iso
        logger.debug("Провайдер %s ответил, но без распознаваемого кода страны: %r", url, data)

    logger.warning("Все GeoIP-провайдеры недоступны - egress-регион не определён")
    return None

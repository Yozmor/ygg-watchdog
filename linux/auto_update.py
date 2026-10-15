"""
auto_update.py

Проверка новой версии на GitHub + обновление ТОЛЬКО по явной команде
пользователя (apply-update) - сама проверка (check-update) никогда
ничего не скачивает и не меняет, только уведомляет. Явный запуск
команды руками - это и есть согласие, отдельный пароль/код здесь не
нужен (в отличие от сценариев удалённого автономного подтверждения).

Целостность: скачивание идёт по официальному tarball_url из самого
GitHub Releases API (не с произвольного стороннего сервера). После
копирования новых файлов - быстрая самопроверка (компиляция главных
модулей); если новая версия не проходит даже это - автоматический
откат на сохранённую резервную копию.
"""

import json
import logging
import py_compile
import shutil
import tarfile
import tempfile
import urllib.error
import urllib.request
from pathlib import Path
from typing import Optional

logger = logging.getLogger("ygg_watchdog.auto_update")

RELEASES_API_URL = "https://api.github.com/repos/Yozmor/ygg-watchdog/releases/latest"
CURRENT_VERSION = "1.1.0"  # поднимать вручную при выпуске новой версии

INSTALL_FILES = [
    "peer_database.py", "watchdog_core.py", "watchdog_daemon.py",
    "geoip.py", "device_monitor.py", "battery.py", "vpn_hook.py", "auto_update.py",
    "yggdrasil_menu.py", "selftest.py", "run_watchdog_menu.sh", "launch_in_terminal.sh",
    "cities.dat", "README_LINUX.md",
]
_PY_FILES_TO_COMPILE_CHECK = [
    "peer_database.py", "watchdog_core.py", "watchdog_daemon.py",
    "geoip.py", "device_monitor.py", "battery.py", "vpn_hook.py", "auto_update.py",
]


def _version_tuple(v: str) -> tuple:
    v = v.lstrip("vV")
    parts = []
    for p in v.split("."):
        digits = "".join(c for c in p if c.isdigit())
        parts.append(int(digits) if digits else 0)
    return tuple(parts)


def _http_get_json(url: str) -> Optional[dict]:
    req = urllib.request.Request(url, headers={
        "User-Agent": "ygg-watchdog/1.0",
        "Accept": "application/vnd.github+json",
    })
    try:
        with urllib.request.urlopen(req, timeout=10) as resp:
            return json.loads(resp.read().decode("utf-8"))
    except (urllib.error.URLError, TimeoutError, OSError, json.JSONDecodeError) as e:
        logger.debug("Не удалось проверить обновления: %s", e)
        return None


def check_for_update() -> Optional[dict]:
    """None, если новой версии нет (включая случай, когда GitHub прямо
    сейчас недоступен - тихо пропускаем, следующая проверка - в
    следующий раз, никакого отдельного резервного механизма здесь не
    нужно, в отличие от GeoIP - последствие неудачи тут не критично)."""
    data = _http_get_json(RELEASES_API_URL)
    if data is None:
        return None
    tag = data.get("tag_name", "")
    if not tag or _version_tuple(tag) <= _version_tuple(CURRENT_VERSION):
        return None
    return {
        "tag": tag,
        "url": data.get("html_url", ""),
        "notes": (data.get("body") or "").strip()[:500],
        "tarball_url": data.get("tarball_url", ""),
    }


def _safe_extract(tar: tarfile.TarFile, dest: Path) -> None:
    dest_resolved = dest.resolve()
    for member in tar.getmembers():
        member_path = (dest / member.name).resolve()
        if not str(member_path).startswith(str(dest_resolved)):
            raise tarfile.TarError(f"Небезопасный путь в архиве: {member.name}")
    tar.extractall(dest)


def apply_update(tarball_url: str, dest_dir: Path) -> bool:
    """Скачивает архив релиза, копирует ТОЛЬКО известные файлы программы
    (не архив как есть) в dest_dir, затем компиляционная самопроверка -
    при провале автоматический откат на резервную копию. Запускать
    ТОЛЬКО по явной команде пользователя."""
    with tempfile.TemporaryDirectory() as tmp:
        tmp_path = Path(tmp)
        archive_path = tmp_path / "release.tar.gz"
        try:
            req = urllib.request.Request(tarball_url, headers={"User-Agent": "ygg-watchdog/1.0"})
            with urllib.request.urlopen(req, timeout=30) as resp, open(archive_path, "wb") as f:
                shutil.copyfileobj(resp, f)
        except (urllib.error.URLError, OSError) as e:
            logger.error("Не удалось скачать обновление: %s", e)
            return False

        extract_dir = tmp_path / "extracted"
        extract_dir.mkdir()
        try:
            with tarfile.open(archive_path) as tar:
                _safe_extract(tar, extract_dir)
        except (tarfile.TarError, OSError) as e:
            logger.error("Не удалось распаковать обновление: %s", e)
            return False

        roots = list(extract_dir.iterdir())
        if len(roots) != 1:
            logger.error("Неожиданная структура архива обновления")
            return False
        linux_src = roots[0] / "linux"
        if not linux_src.exists():
            logger.error("В архиве обновления не найдена папка linux/")
            return False

        backup_dir = dest_dir.parent / (dest_dir.name + ".backup")
        if backup_dir.exists():
            shutil.rmtree(backup_dir)
        shutil.copytree(dest_dir, backup_dir)

        def _rollback(reason: str) -> None:
            logger.error("%s - откатываюсь на предыдущую версию из резервной копии", reason)
            shutil.rmtree(dest_dir)
            shutil.copytree(backup_dir, dest_dir)

        try:
            for name in INSTALL_FILES:
                src_file = linux_src / name
                if src_file.exists():
                    shutil.copy2(src_file, dest_dir / name)
        except OSError as e:
            _rollback(f"Ошибка при копировании новых файлов: {e}")
            return False

        for name in _PY_FILES_TO_COMPILE_CHECK:
            target = dest_dir / name
            if not target.exists():
                continue
            try:
                py_compile.compile(str(target), doraise=True)
            except py_compile.PyCompileError as e:
                _rollback(f"Новая версия '{name}' не прошла самопроверку (ошибка компиляции): {e}")
                return False

    logger.info("Обновление применено успешно. Резервная копия предыдущей версии: %s", backup_dir)
    return True

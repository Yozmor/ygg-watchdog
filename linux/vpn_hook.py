"""
vpn_hook.py

"Продвинутый" уровень доступа к VPN - пользователь сам прописывает путь
к своей команде управления VPN (например, свой vpn-on/vpn-off на основе
Xray), и watchdog дёргает именно её вместо того, чтобы угадывать имена
общеизвестных служб (xray/v2ray/sing-box - см. watchdog_core._try_restart_local_vpn,
это и есть "базовый" уровень, он никуда не делся и работает, если
продвинутый хук не настроен).

Защита (обсуждали отдельно): путь к скрипту одобряется ОДНИМ разовым
действием (approve_vpn_command, запускается руками, с правами root -
сам факт root-доступа на этой машине и есть подтверждение, отдельный
Telegram-пароль здесь не нужен, в отличие от сценариев удалённого
подтверждения). При одобрении считается SHA-256 содержимого файла и
сохраняется рядом в отдельном файле с правами только для root (0600) -
НЕ в основном notify.conf/state.json. Перед КАЖДЫМ запуском хэш
пересчитывается и сравнивается: если скрипт изменился с момента
одобрения (кем угодно - атакующим или просто забытой правкой) -
запуск отказывается, нужно одобрить заново.
"""

import hashlib
import json
import logging
import os
import subprocess
from pathlib import Path
from typing import Optional

logger = logging.getLogger("ygg_watchdog.vpn_hook")

_APPROVAL_PATH = Path(__file__).resolve().parent / "vpn_hook_approval.json"


def _sha256_of_file(path: Path) -> Optional[str]:
    try:
        return hashlib.sha256(path.read_bytes()).hexdigest()
    except OSError:
        return None


def approve_vpn_command(command_path: str) -> bool:
    """Одноразовое одобрение - запускать руками, с правами root, после
    того как сам убедился, что путь и содержимое скрипта - то, что
    нужно. Перезатирает предыдущее одобрение, если было."""
    path = Path(command_path).resolve()
    if not path.exists():
        logger.error("Файл '%s' не существует - нечего одобрять", path)
        return False

    digest = _sha256_of_file(path)
    if digest is None:
        logger.error("Не удалось прочитать '%s' для расчёта контрольной суммы", path)
        return False

    _APPROVAL_PATH.write_text(
        json.dumps({"command": str(path), "sha256": digest}, ensure_ascii=False, indent=2),
        encoding="utf-8",
    )
    try:
        os.chmod(_APPROVAL_PATH, 0o600)
    except OSError:
        pass

    logger.info("VPN-команда одобрена: %s (sha256=%s)", path, digest[:12])
    return True


def clear_approval() -> None:
    if _APPROVAL_PATH.exists():
        _APPROVAL_PATH.unlink()
        logger.info("Одобрение VPN-команды снято.")


def get_approved_command() -> Optional[str]:
    """Возвращает путь к одобренной команде, ТОЛЬКО если её текущее
    содержимое на диске побайтово совпадает с тем, что было одобрено.
    Любое расхождение (подмена, случайная правка) - отказ, не запуск."""
    if not _APPROVAL_PATH.exists():
        return None

    try:
        data = json.loads(_APPROVAL_PATH.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as e:
        logger.warning("Не удалось прочитать одобрение VPN-команды: %s", e)
        return None

    path = Path(data.get("command", ""))
    expected_digest = data.get("sha256")
    if not path.exists():
        logger.warning("Одобренная VPN-команда '%s' больше не существует на диске", path)
        return None

    actual_digest = _sha256_of_file(path)
    if actual_digest != expected_digest:
        logger.error(
            "ВНИМАНИЕ: файл одобренной VPN-команды '%s' изменился с момента одобрения "
            "(ожидался sha256=%s, сейчас %s) - отказываюсь запускать, одобри заново (approve-vpn-command).",
            path, (expected_digest or "")[:12], (actual_digest or "")[:12],
        )
        return None

    return str(path)


def run_approved_vpn_command(args: list[str], timeout: float = 20.0) -> bool:
    """Запускает одобренную команду (см. get_approved_command) с
    указанными аргументами. Возвращает False, если команда не одобрена/
    не прошла проверку целостности, или если сам запуск завершился
    с ошибкой/таймаутом."""
    command = get_approved_command()
    if command is None:
        return False

    try:
        result = subprocess.run([command] + args, capture_output=True, timeout=timeout)
        if result.returncode == 0:
            logger.info("VPN-хук '%s %s' выполнен успешно", command, " ".join(args))
            return True
        logger.warning(
            "VPN-хук '%s %s' завершился с кодом %d: %s",
            command, " ".join(args), result.returncode,
            result.stderr.decode(errors="replace").strip(),
        )
        return False
    except (FileNotFoundError, subprocess.TimeoutExpired) as e:
        logger.warning("Не удалось выполнить VPN-хук '%s': %s", command, e)
        return False

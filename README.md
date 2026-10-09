<p align="center"><img src="assets/ygg-watchdog.png" width="160" alt="ygg-watchdog"></p>

<h1 align="center">ygg-watchdog</h1>

Программа для [Yggdrasil](https://yggdrasil-network.github.io/), которая следит за пирами и сама восстанавливает связь, когда они отваливаются, а о сбоях сообщает в Telegram. Есть версии для Linux (Python + systemd) и Windows (C++ + Планировщик заданий).

## Быстрый старт

- **Windows:** скачай из папки [`windows`](windows) три файла — `ygg_watchdog.exe`, `cities.dat`, `notify.conf.example` — положи их в одну папку, дважды кликни `ygg_watchdog.exe` и в меню нажми **2**.
- **Linux:** `git clone https://github.com/Yozmor/ygg-watchdog.git && cd ygg-watchdog/linux && sudo ./install.sh`

## Что делает

Каждые 3 минуты программа проверяет пиры Yggdrasil и действует сама:

- **Все основные пиры легли** — ищет замену из того же региона в публичном списке пиров, проверяет кандидатов настоящим рукопожатием и добавляет рабочего как резервного.
- **Основной пир снова жив** — резервные убираются.
- **Пропал интернет целиком** — не тратит время на бесполезный поиск пиров, ждёт восстановления и записывает в лог, когда связь пропала и когда вернулась.
- **Региональный режим** — можно выбрать страну и город, и программа будет держать пиры, ближайшие к этому городу (по реальным координатам из базы GeoNames).

Для ручного управления есть меню: статус, добавление и удаление основных пиров, региональный режим, перезапуск Yggdrasil (в том числе с задержкой, чтобы не оборвать свою же SSH-сессию, если подключён через Yggdrasil).

**Уведомления в Telegram** (необязательно): бот присылает сообщение, когда основные пиры легли и найдена замена, когда они вернулись и когда пропадал интернет. Рутинные проверки «всё в порядке» не присылаются. Если несколько устройств шлют уведомления одному боту, в каждом сообщении указано имя устройства.

## Установка

### Windows

**1. Скачай файлы.** Открой папку [`windows`](windows) в этом репозитории и скачай оттуда три файла. Чтобы скачать файл, кликни по нему, а потом на кнопку **Download raw file** (стрелка вниз справа над содержимым).

| Файл | Зачем |
|---|---|
| `ygg_watchdog.exe` | Сама программа |
| `cities.dat` | База городов, без неё не работает выбор региона |
| `notify.conf.example` | Пример настроек Telegram (нужен, только если хочешь уведомления) |

Остальные файлы в папке (`.cpp`, `.bat`, `.rc`, `.ico`) — исходники и сборка, для работы они не нужны.

**2. Положи их в одну постоянную папку**, например `C:\Tools\ygg-watchdog`. Не оставляй в «Загрузках»: задача планировщика запоминает путь к программе, и если потом переместить папку, проверки перестанут запускаться.

**3. Дважды кликни `ygg_watchdog.exe`.** Windows попросит права администратора, соглашайся. Откроется меню.

**4. Нажми `2` и Enter.** Это установит задачу в Планировщик Windows: программа будет проверять пиры каждые 3 минуты в фоне, даже когда никто не вошёл в систему. Больше ничего делать не нужно.

**Собрать exe самому** (если поменял код): скачай весь репозиторий (зелёная кнопка **Code → Download ZIP**), открой папку `windows` и запусти `run_ygg_watchdog.bat` — он соберёт `ygg_watchdog.exe` с иконкой. Нужен компилятор: [MinGW-w64](https://winlibs.com/) или Visual Studio с компонентом «Разработка классических приложений на C++».

Подробно про меню и настройки: [windows/README_WINDOWS.md](windows/README_WINDOWS.md)

### Linux

Нужны Python 3, systemd и установленный Yggdrasil.

```bash
git clone https://github.com/Yozmor/ygg-watchdog.git
cd ygg-watchdog/linux
sudo ./install.sh
```

Установщик скопирует программу в `/opt/ygg-watchdog`, включит systemd-таймер (проверка каждые 3 минуты) и добавит команду `ygg-watchdog` для меню. Повторный запуск обновляет программу, настройки и логи сохраняются. Удалить: `sudo ./install.sh --remove`.

```bash
ygg-watchdog                                  # меню
sudo python3 /opt/ygg-watchdog/selftest.py    # самопроверка
tail -f /opt/ygg-watchdog/watchdog.log        # лог
```

Подробно: [linux/README_LINUX.md](linux/README_LINUX.md)

## Уведомления в Telegram

1. Создай бота через `@BotFather` (`/newbot`) и получи токен.
2. Напиши своему боту любое сообщение.
3. Открой `https://api.telegram.org/bot<ТОКЕН>/getUpdates` и найди число в `"chat":{"id": ...}`.
4. Переименуй `notify.conf.example` в `notify.conf` и впиши:

```
BOT_TOKEN=123456:ABC-DEF...
CHAT_ID=987654321
```

На Windows, если Telegram у тебя открывается только через локальный прокси, впиши его в `PROXY=адрес:порт` в том же файле.

Без `notify.conf` уведомления просто выключены, всё остальное работает.

## Команды без меню (Linux)

```bash
cd /opt/ygg-watchdog
sudo python3 watchdog_daemon.py tick                          # одна проверка
sudo python3 watchdog_daemon.py status                        # статус
sudo python3 watchdog_daemon.py list-countries                # список стран
sudo python3 watchdog_daemon.py list-cities russia            # города страны
sudo python3 watchdog_daemon.py add-region russia Vladivostok # включить регион
sudo python3 watchdog_daemon.py remove-region                 # выключить регион
sudo python3 watchdog_daemon.py add-main-peer tls://host:port # добавить основной пир
sudo python3 watchdog_daemon.py restart-yggdrasil-delayed 15  # перезапуск через 15 с
```

## Состав репозитория

```
linux/      Python-версия: watchdog_daemon.py (меню и команды), watchdog_core.py (логика),
            peer_database.py (пиры и города), selftest.py, systemd-таймер, install.sh
windows/    C++-версия: ygg_watchdog.exe (готовая программа), ygg_watchdog.cpp (исходник),
            run_ygg_watchdog.bat (сборка), иконка
assets/     картинки для README
```

В обеих папках лежит `cities.dat`: база городов GeoNames с координатами, без неё региональный поиск не работает. Там же `notify.conf.example`, пример настроек Telegram.

@echo off
setlocal
chcp 65001 >nul 2>&1
rem Сборка и запуск тестов ygg-watchdog (tests.exe). Нужен MinGW g++ (как для run_ygg_watchdog.bat).
rem Тесты не ходят в сеть и не трогают Yggdrasil; state.txt и watchdog.log пишут в эту папку.
pushd "%~dp0"
where g++ >nul 2>&1
if errorlevel 1 (
    echo Не найден g++ - поставь MinGW-w64 ^(winlibs.com^) или собери через MSVC:
    echo   cl /nologo /EHsc /std:c++17 /utf-8 /I..\gui tests.cpp /Fe:tests.exe
    popd
    pause
    exit /b 1
)
echo Сборка tests.exe...
g++ -std=c++17 -O1 -municode -static -I..\gui -o tests.exe tests.cpp -lwinhttp -lws2_32 -lshell32 -ladvapi32 -lbcrypt -lole32 -loleaut32 -luuid -liphlpapi
if errorlevel 1 (
    echo Ошибка сборки, текст выше.
    popd
    pause
    exit /b 1
)
tests.exe
set "RC=%errorlevel%"
popd
echo.
if "%RC%"=="0" (echo Все тесты прошли.) else (echo Есть провалы - смотри строки ПРОВАЛ выше.)
pause
exit /b %RC%

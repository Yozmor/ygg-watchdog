@echo off
setlocal enabledelayedexpansion

net session >nul 2>&1
if %errorlevel% neq 0 (
    echo Требуются права администратора, перезапуск с повышением прав...
    powershell -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
    exit /b
)

set "SCRIPT_DIR=%~dp0"
set "SRC=%SCRIPT_DIR%ygg_watchdog.cpp"
set "EXE=%SCRIPT_DIR%ygg_watchdog.exe"

if not exist "%SRC%" (
    echo Не найден ygg_watchdog.cpp рядом с этим bat-файлом.
    echo Ожидаемый путь: %SRC%
    pause
    exit /b 1
)

set "NEED_BUILD=0"
if not exist "%EXE%" (
    set "NEED_BUILD=1"
) else (
    for /f %%a in ('powershell -NoProfile -Command "(Get-Item '%SRC%').LastWriteTime -gt (Get-Item '%EXE%').LastWriteTime"') do set "SRC_NEWER=%%a"
    if /i "!SRC_NEWER!"=="True" set "NEED_BUILD=1"
)

if "%NEED_BUILD%"=="0" goto RUNEXE

echo Сборка ygg_watchdog.exe из исходника...
echo.

where g++ >nul 2>&1
if not errorlevel 1 (
    echo Найден g++, собираю через MinGW...
    pushd "%SCRIPT_DIR%"
    set "RESOBJ="
    where windres >nul 2>&1
    if not errorlevel 1 if exist app.rc (
        windres app.rc -O coff -o app.res
        if not errorlevel 1 set "RESOBJ=app.res"
    )
    g++ -std=c++17 -O2 -municode -static -o ygg_watchdog.exe ygg_watchdog.cpp !RESOBJ! -lwinhttp -lws2_32 -lshell32 -ladvapi32
    set "GPPRESULT=!errorlevel!"
    if exist app.res del app.res
    popd
    if "!GPPRESULT!"=="0" goto BUILD_OK
    echo.
    echo Ошибка сборки через g++. Смотри текст ошибки выше.
    pause
    exit /b 1
)

where cl >nul 2>&1
if not errorlevel 1 (
    echo Найден cl, собираю через MSVC...
    pushd "%SCRIPT_DIR%"
    set "RESOBJ="
    if exist app.rc rc /nologo /fo app.res app.rc >nul && set "RESOBJ=app.res"
    cl /nologo /EHsc /std:c++17 /utf-8 "%SRC%" !RESOBJ! /Fe:"%EXE%" /link Winhttp.lib Ws2_32.lib Shell32.lib Advapi32.lib
    set "CLRESULT=!errorlevel!"
    if exist app.res del app.res
    if exist ygg_watchdog.obj del ygg_watchdog.obj
    popd
    if "!CLRESULT!"=="0" goto BUILD_OK
    echo.
    echo Ошибка сборки через cl. Смотри текст ошибки выше.
    pause
    exit /b 1
)

echo Не найден ни g++ ни cl напрямую в PATH.
echo Пытаюсь автоматически найти Visual Studio...
echo.

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"

if not exist "%VSWHERE%" goto NOCOMPILER

for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do (
    set "VSINSTALLPATH=%%i"
)

if not defined VSINSTALLPATH goto NOCOMPILER

set "VCVARS=%VSINSTALLPATH%\VC\Auxiliary\Build\vcvarsall.bat"
if not exist "%VCVARS%" goto NOCOMPILER

echo Найден Visual Studio в: %VSINSTALLPATH%
echo Подтягиваю окружение компилятора...
echo.
call "%VCVARS%" x64 >nul 2>&1

where cl >nul 2>&1
if errorlevel 1 goto NOCOMPILER

echo Собираю через автоматически найденный MSVC...
pushd "%SCRIPT_DIR%"
set "RESOBJ="
if exist app.rc rc /nologo /fo app.res app.rc >nul && set "RESOBJ=app.res"
cl /nologo /EHsc /std:c++17 /utf-8 "%SRC%" !RESOBJ! /Fe:"%EXE%" /link Winhttp.lib Ws2_32.lib Shell32.lib Advapi32.lib
set "CLRESULT=!errorlevel!"
if exist app.res del app.res
if exist ygg_watchdog.obj del ygg_watchdog.obj
popd

if "!CLRESULT!"=="0" goto BUILD_OK

echo.
echo Ошибка сборки через автоматически найденный MSVC.
pause
exit /b 1

:NOCOMPILER
echo.
echo Не найден ни один компилятор - ни g++, ни cl, ни установленная
echo Visual Studio с компонентом "Desktop development with C++".
echo.
echo Поставь один из вариантов:
echo   1. MinGW-w64 с winlibs.com - проще всего, добавь его bin в PATH
echo   2. Visual Studio Community с компонентом C++ - бесплатно,
echo      https://visualstudio.microsoft.com/
echo.
pause
exit /b 1

:BUILD_OK
echo.
echo Сборка успешна: %EXE%
echo.

:RUNEXE
if not exist "%EXE%" (
    echo Файл %EXE% не появился после сборки, что-то пошло не так.
    pause
    exit /b 1
)

if "%~1"=="" goto SHOWMENU

"%EXE%" %*
echo.
pause
exit /b

:SHOWMENU
cls
echo ================================================
echo         YGG WATCHDOG
echo ================================================
echo.
echo   1 - Проверить сейчас (tick)
echo   2 - Установить задачу планировщика (install-task)
echo   3 - Добавить регион (страна + город)
echo   4 - Убрать региональный режим
echo   5 - Показать основные пиры
echo   6 - Добавить основной пир
echo   7 - Удалить основной пир
echo   8 - Перезапустить Yggdrasil СЕЙЧАС (обрывает сессии по Yggdrasil!)
echo   9 - Перезапустить Yggdrasil С ЗАДЕРЖКОЙ (безопасно при RDP по Yggdrasil)
echo   0 - Выход
echo.
set /p menuchoice="Выбор: "

if "%menuchoice%"=="1" (
    "%EXE%" tick
) else if "%menuchoice%"=="2" (
    "%EXE%" install-task
) else if "%menuchoice%"=="3" (
    "%EXE%" list-countries
    echo.
    set /p region_country="Номер страны из списка выше: "
    echo.
    "%EXE%" list-cities "!region_country!"
    echo.
    set /p region_city="Номер города из списка выше (например 1): "
    "%EXE%" add-region "!region_country!" "!region_city!"
) else if "%menuchoice%"=="4" (
    "%EXE%" remove-region
) else if "%menuchoice%"=="5" (
    "%EXE%" list-main-peers
) else if "%menuchoice%"=="6" (
    set /p new_peer_uri="URI пира (например tls://host:port): "
    "%EXE%" add-main-peer "!new_peer_uri!"
    echo.
    echo Не забудь перезапустить службу - изменения вступят в силу только после этого.
) else if "%menuchoice%"=="7" (
    "%EXE%" list-main-peers
    echo.
    set /p remove_peer_num="Номер пира для удаления: "
    "%EXE%" remove-main-peer "!remove_peer_num!"
    echo.
    echo Не забудь перезапустить службу - изменения вступят в силу только после этого.
) else if "%menuchoice%"=="8" (
    "%EXE%" restart-yggdrasil
) else if "%menuchoice%"=="9" (
    set /p restart_delay="Задержка в секундах (по умолчанию 10): "
    if "!restart_delay!"=="" set "restart_delay=10"
    "%EXE%" restart-yggdrasil-delayed "!restart_delay!"
) else (
    exit /b
)

echo.
pause
goto SHOWMENU

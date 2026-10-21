@echo off
setlocal enabledelayedexpansion
chcp 65001 >nul 2>&1
rem Сборка ygg_watchdog.exe (сторож + окно в одном файле) и запуск.
rem   run_ygg_watchdog.bat          - собрать при изменении исходников и открыть окно
rem   run_ygg_watchdog.bat <команда> - собрать при необходимости и выполнить команду (tick, install-task, ...)
rem
rem Раскладка проекта:
rem   ygg_watchdog.cpp                     - исходник сторожа (в корне)
rem   app.rc, gui\bg_gray.png             - ресурсы
rem   gui\ygg_watchdog_gui.cpp             - окно
rem   gui\core.h, gui\util.h              - общие с панелью ygg-control (положить в gui\)
rem   gui\json_lite.h, gui\ui.h           - заголовки окна и парсера JSON (в gui\)
rem   ygg_guard.cpp                        - страж (ygg_guard.exe): его запускает задача Планировщика,
rem                                          он проверяет целостность ygg_watchdog.exe
rem
rem bat сохранён в UTF-8 без BOM (chcp 65001 в начале делает русский текст читаемым).

set "SCRIPT_DIR=%~dp0"
set "EXE=%SCRIPT_DIR%ygg_watchdog.exe"
set "GUARD=%SCRIPT_DIR%ygg_guard.exe"
pushd "%SCRIPT_DIR%"

if not exist "ygg_watchdog.cpp" (
    echo Не найден ygg_watchdog.cpp рядом с этим bat-файлом.
    popd
    pause
    exit /b 1
)

rem Пересобираем, если exe нет или любой исходник/заголовок новее него.
rem -ErrorAction SilentlyContinue нужен на случай, если core.h/util.h ещё не положены -
rem powershell молча пропустит отсутствующие, а не упадёт.
set "NEED_BUILD=0"
if not exist "%EXE%" set "NEED_BUILD=1"
if not exist "%GUARD%" if exist "ygg_guard.cpp" set "NEED_BUILD=1"
if "%NEED_BUILD%"=="0" (
    for /f %%a in ('powershell -NoProfile -Command "$e=(Get-Item 'ygg_watchdog.exe').LastWriteTime; [bool](Get-ChildItem 'ygg_watchdog.cpp','ygg_guard.cpp','gui\core.h','gui\util.h','gui\json_lite.h','gui\ui.h','app.rc','gui\*.cpp','gui\*.h','gui\bg_gray.png' -ErrorAction SilentlyContinue | Where-Object { $_.LastWriteTime -gt $e })"') do set "SRC_NEWER=%%a"
    if /i "!SRC_NEWER!"=="True" set "NEED_BUILD=1"
)
if "%NEED_BUILD%"=="0" goto RUNEXE

echo Сборка ygg_watchdog.exe...
echo.

rem iphlpapi (-liphlpapi / iphlpapi.lib) - для Icmp6CreateFile из util.h (ping IPv6).
rem Без него линковка упадёт на "undefined reference to Icmp6CreateFile".
set "LIBS_GCC=-lwinhttp -lws2_32 -lshell32 -ladvapi32 -lbcrypt -ld2d1 -ldwrite -ldwmapi -lole32 -loleaut32 -luser32 -lgdi32 -lwindowscodecs -luuid -lcomdlg32 -liphlpapi"
set "LIBS_CL=winhttp.lib ws2_32.lib shell32.lib advapi32.lib bcrypt.lib d2d1.lib dwrite.lib dwmapi.lib ole32.lib oleaut32.lib user32.lib gdi32.lib windowscodecs.lib comdlg32.lib iphlpapi.lib"

rem Перед сборкой закрываем запущенный ygg_watchdog.exe (значок в трее, окно): пока он работает, файл занят.
taskkill /f /im ygg_watchdog.exe >nul 2>&1
taskkill /f /im ygg_guard.exe >nul 2>&1

rem ---------- MinGW g++ ----------
:TRY_GCC
where g++ >nul 2>&1
if errorlevel 1 goto TRY_MSVC
echo Собираю через MinGW g++...
set "RESOBJ="
where windres >nul 2>&1
if not errorlevel 1 (
    windres app.rc -O coff -o app.res
    if not errorlevel 1 set "RESOBJ=app.res"
)
rem -Igui: страховка. #include "core.h" в ygg_watchdog.cpp (если ты оставил так, без "gui/")
rem ищется рядом с исходником (в корне), потом в -I путях. В gui\ - найдёт.
g++ -std=c++17 -O2 -municode -mwindows -static -Igui -o ygg_watchdog.exe ygg_watchdog.cpp gui\ygg_watchdog_gui.cpp !RESOBJ! %LIBS_GCC%
set "RES=!errorlevel!"
if "!RES!"=="0" if exist "ygg_guard.cpp" (
    echo Собираю стража ygg_guard.exe...
    g++ -std=c++17 -O2 -municode -mwindows -static -Igui -o ygg_guard.exe ygg_guard.cpp !RESOBJ! %LIBS_GCC%
    set "RES=!errorlevel!"
)
if exist app.res del app.res
if "!RES!"=="0" goto BUILD_OK
echo.
echo MinGW g++ не справился (текст ошибки выше).
echo   Если это "taskschd.h: No such file or directory" - в старом MinGW нет
echo   заголовка Планировщика заданий. Поставь более свежий MinGW-w64 (winlibs.com)
echo   или собери через MSVC.
echo.
echo Пробую MSVC...
echo.

rem ---------- MSVC cl ----------
:TRY_MSVC
where cl >nul 2>&1
if errorlevel 1 (
    set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
    if exist "!VSWHERE!" (
        for /f "usebackq tokens=*" %%i in (`"!VSWHERE!" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSINSTALLPATH=%%i"
        if defined VSINSTALLPATH call "!VSINSTALLPATH!\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul 2>&1
    )
)
where cl >nul 2>&1
if errorlevel 1 goto NOCOMPILER

echo Собираю через MSVC...
set "RESOBJ="
rc /nologo /fo app.res app.rc >nul && set "RESOBJ=app.res"
cl /nologo /EHsc /std:c++17 /utf-8 /O2 /Igui ygg_watchdog.cpp gui\ygg_watchdog_gui.cpp !RESOBJ! /Fe:ygg_watchdog.exe /link /SUBSYSTEM:WINDOWS /ENTRY:wWinMainCRTStartup %LIBS_CL%
set "RES=!errorlevel!"
if "!RES!"=="0" if exist "ygg_guard.cpp" (
    echo Собираю стража ygg_guard.exe...
    cl /nologo /EHsc /std:c++17 /utf-8 /O2 /Igui ygg_guard.cpp !RESOBJ! /Fe:ygg_guard.exe /link /SUBSYSTEM:WINDOWS /ENTRY:wWinMainCRTStartup %LIBS_CL%
    set "RES=!errorlevel!"
    if exist ygg_guard.obj del ygg_guard.obj
)
if exist app.res del app.res
if exist ygg_watchdog.obj del ygg_watchdog.obj
if exist ygg_watchdog_gui.obj del ygg_watchdog_gui.obj
if "!RES!"=="0" goto BUILD_OK
goto BUILD_FAIL

:NOCOMPILER
echo Не найден ни g++, ни cl, ни Visual Studio с компонентом C++.
echo   1. MinGW-w64 с winlibs.com - проще всего, добавь его bin в PATH
echo   2. Visual Studio Community с компонентом "Desktop development with C++"
popd
pause
exit /b 1

:BUILD_FAIL
echo.
echo Ошибка сборки, текст выше.
popd
pause
exit /b 1

:BUILD_OK
echo Сборка успешна: %EXE%
if exist "%GUARD%" echo                  %GUARD%
echo.

:RUNEXE
popd
if "%~1"=="" (
    rem окно: само попросит права администратора
    start "" "%EXE%"
    exit /b 0
)
rem команда: нужны права администратора (кроме status-json и справочников)
"%EXE%" %*
echo.
pause
exit /b
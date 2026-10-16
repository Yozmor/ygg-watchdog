@echo off
setlocal enabledelayedexpansion
rem Сборка ygg_watchdog.exe (сторож + окно в одном файле) и запуск.
rem   run_ygg_watchdog.bat          - собрать при изменении исходников и открыть окно
rem   run_ygg_watchdog.bat <команда> - собрать при необходимости и выполнить команду (tick, install-task, ...)

set "SCRIPT_DIR=%~dp0"
set "EXE=%SCRIPT_DIR%ygg_watchdog.exe"
pushd "%SCRIPT_DIR%"

if not exist "ygg_watchdog.cpp" (
    echo Не найден ygg_watchdog.cpp рядом с этим bat-файлом.
    popd
    pause
    exit /b 1
)

rem Пересобираем, если exe нет или любой исходник новее него
set "NEED_BUILD=0"
if not exist "%EXE%" (
    set "NEED_BUILD=1"
) else (
    for /f %%a in ('powershell -NoProfile -Command "$e=(Get-Item 'ygg_watchdog.exe').LastWriteTime; [bool](Get-ChildItem 'ygg_watchdog.cpp','app.rc','gui\*.cpp','gui\*.h','gui\bg_gray.png' | Where-Object { $_.LastWriteTime -gt $e })"') do set "SRC_NEWER=%%a"
    if /i "!SRC_NEWER!"=="True" set "NEED_BUILD=1"
)
if "%NEED_BUILD%"=="0" goto RUNEXE

echo Сборка ygg_watchdog.exe...
echo.

set "LIBS_GCC=-lwinhttp -lws2_32 -lshell32 -ladvapi32 -lbcrypt -ld2d1 -ldwrite -ldwmapi -lole32 -loleaut32 -luser32 -lgdi32 -lwindowscodecs -luuid -lcomdlg32"
set "LIBS_CL=winhttp.lib ws2_32.lib shell32.lib advapi32.lib bcrypt.lib d2d1.lib dwrite.lib dwmapi.lib ole32.lib oleaut32.lib user32.lib gdi32.lib windowscodecs.lib comdlg32.lib"

rem Перед сборкой закрываем запущенный ygg_watchdog.exe (значок в трее, окно): пока он работает, файл занят
taskkill /f /im ygg_watchdog.exe >nul 2>&1
where g++ >nul 2>&1
if not errorlevel 1 (
    echo Собираю через MinGW g++...
    set "RESOBJ="
    where windres >nul 2>&1
    if not errorlevel 1 (
        windres app.rc -O coff -o app.res
        if not errorlevel 1 set "RESOBJ=app.res"
    )
    g++ -std=c++17 -O2 -municode -mwindows -static -o ygg_watchdog.exe ygg_watchdog.cpp gui\ygg_watchdog_gui.cpp !RESOBJ! %LIBS_GCC%
    set "RES=!errorlevel!"
    if exist app.res del app.res
    if "!RES!"=="0" goto BUILD_OK
    goto BUILD_FAIL
)

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
cl /nologo /EHsc /std:c++17 /utf-8 /O2 ygg_watchdog.cpp gui\ygg_watchdog_gui.cpp !RESOBJ! /Fe:ygg_watchdog.exe /link /SUBSYSTEM:WINDOWS /ENTRY:wWinMainCRTStartup %LIBS_CL%
set "RES=!errorlevel!"
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

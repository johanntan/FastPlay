@echo off
REM Build script for FastPlay with modular source files

setlocal enabledelayedexpansion
cd /d "%~dp0"

REM Find Visual Studio using vswhere
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo Error: Cannot find vswhere.exe - Visual Studio 2017+ required
    exit /b 1
)

for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -property installationPath`) do (
    set "VSINSTALL=%%i"
)

if not defined VSINSTALL (
    echo Error: Cannot find Visual Studio with C++ tools
    exit /b 1
)

REM Set up the environment for x64
if exist "%VSINSTALL%\VC\Auxiliary\Build\vcvars64.bat" (
    call "%VSINSTALL%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
) else (
    echo Error: Cannot find vcvars64.bat
    exit /b 1
)

REM Build options (screen reader speech and Steam Audio are on by default). Both are
REM always passed, so an earlier no-speech / no-steamaudio build does not stick.
set "SPEECH=ON"
set "STEAMAUDIO=ON"

REM Parse arguments
:parse_args
if "%1"=="" goto :done_args
if "%1"=="no-speech" (
    set "SPEECH=OFF"
    echo Disabling screen reader support...
) else if "%1"=="no-steamaudio" (
    set "STEAMAUDIO=OFF"
    echo Disabling Steam Audio support...
)
shift
goto :parse_args
:done_args

REM Read version from version.h
set "APP_VERSION="
for /f "tokens=3 delims= " %%v in ('findstr /C:"#define APP_VERSION " include\fastplay\version.h') do set "APP_VERSION=%%~v"
echo Building FastPlay %APP_VERSION%...

REM Configure and build with CMake. The first run fetches and builds wxWidgets and
REM UniversalSpeech into build\, which takes a while; later runs reuse them.
REM FastPlay.exe is written to this folder. The commit hash for the update check
REM is picked up by CMakeLists.txt.
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DFASTPLAY_SPEECH=%SPEECH% -DFASTPLAY_STEAM_AUDIO=%STEAMAUDIO%
if errorlevel 1 goto :error
REM /nodeReuse:false: do not leave MSBuild worker processes running afterwards.
cmake --build build --config Release --parallel -- /nodeReuse:false
if errorlevel 1 goto :error

REM Build distribution zip
echo Building distribution...
call "%~dp0dist.bat"

REM Build installer if Inno Setup is available
set "ISCC="
for %%V in (7 6) do (
    if not defined ISCC if exist "%ProgramFiles(x86)%\Inno Setup %%V\ISCC.exe" set "ISCC=%ProgramFiles(x86)%\Inno Setup %%V\ISCC.exe"
    if not defined ISCC if exist "%ProgramFiles%\Inno Setup %%V\ISCC.exe" set "ISCC=%ProgramFiles%\Inno Setup %%V\ISCC.exe"
)

if defined ISCC (
    echo Building installer...
    REM Prepare dist_temp for installer
    if exist "dist_temp" rmdir /s /q "dist_temp"
    mkdir "dist_temp"
    copy /y "FastPlay.exe" "dist_temp\" >nul
    mkdir "dist_temp\docs" 2>nul
    xcopy /y /e "docs\*" "dist_temp\docs\" >nul 2>&1
    mkdir "dist_temp\lib" 2>nul
    for %%f in (lib\*.dll) do copy /y "%%f" "dist_temp\lib\" >nul 2>&1
    "%ISCC%" /DMyAppVersion=%APP_VERSION% /DSourceDir=dist_temp /DOutputDir=. installer.iss
    if errorlevel 1 (
        echo Installer build failed!
    ) else (
        echo Installer built successfully: FastPlayInstaller.exe
    )
    rmdir /s /q "dist_temp" 2>nul
) else (
    echo Inno Setup not found, skipping installer build.
)

echo.
echo Build successful! Run FastPlay.exe to start.
goto :end

:error
echo.
echo Build failed!
exit /b 1

:end

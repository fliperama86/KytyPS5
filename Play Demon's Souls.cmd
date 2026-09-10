@echo off
rem Launch Demon's Souls from the repository root with the current build.
rem
rem There is exactly one emulator binary: the one CMake writes to _Build\windows. Nothing is copied
rem or staged, so whatever was built last is what runs. Runtime data (saves, pipeline cache,
rem Kyty.ini) lives in _Runtime, which is the working directory; the emulator resolves its data
rem folders relative to it. The Qt DLLs live there too, so it goes on PATH.
rem
rem   Play Demon's Souls.cmd            normal run
rem   Play Demon's Souls.cmd profile    same binary with the Tracy profiler listening on 127.0.0.1:8086
rem                                     (on-demand: costs nothing until a capture client connects)
rem
rem Only the game dump lives outside the project; override it by setting DES_GAME before running.

setlocal
set "desRoot=%~dp0"
set "desRuntime=%desRoot%_Runtime"
set "desExe=%desRoot%_Build\windows\kyty_emulator.exe"

rem An absolute path, not a bare name: some environments set NoDefaultCurrentDirectoryInExePath,
rem which stops cmd resolving an executable from the working directory even when it is right there.
if not exist "%desExe%" (
	echo Missing "%desExe%".
	echo Build it with: powershell -ExecutionPolicy Bypass -File "%desRoot%_Build\build-demons-souls.ps1"
	pause
	exit /b 1
)

if not defined DES_GAME set "DES_GAME=E:/Emulation/PS5/Games/DemonsSouls-PPSA01342-dump"

rem The collision workaround is required for the game to reach gameplay; disabling the touch probes
rem stops it progressing past the intro. Full capture stays off because it prints every event.
set "KYTY_DEBUG_GUEST_FAULT=1"
set "KYTY_DEBUG_DES_TOUCH_LIST=1"
set "KYTY_DEBUG_DES_TOUCH_TRACE=1"
set "KYTY_DEBUG_DES_TOUCH_SERIALIZE=1"
set "KYTY_DEBUG_DES_TOUCH_FULL_CAPTURE=0"

set "desArgs="
if /i "%~1"=="profile" set "desArgs=--profiler-direction Network"

set "PATH=%desRuntime%;%PATH%"
cd /d "%desRuntime%"
echo Running "%desExe%"
echo Game: %DES_GAME%
"%desExe%" --game "%DES_GAME%" %desArgs%
set "desExit=%errorlevel%"

rem Exit cleanly through the game's own menu when possible: the Vulkan pipeline cache is only
rem written on a clean shutdown, and a forced kill discards the compiled shaders.
if not "%desExit%"=="0" (
	echo.
	echo Exited with code %desExit%.
	pause
)
exit /b %desExit%

@echo off
rem Launch Demon's Souls from the repository root.
rem
rem Paths are resolved relative to this file, so the repository can be moved without editing it.
rem Only the game dump lives outside the project; override it by setting DES_GAME before running.

setlocal
cd /d "%~dp0_Runtime"

if not defined DES_GAME set "DES_GAME=E:/Emulation/PS5/Games/DemonsSouls-PPSA01342-dump"

rem The collision workaround is required for the game to reach gameplay; disabling the touch probes
rem stops it progressing past the intro. Full capture stays off because it prints every event.
set "KYTY_DEBUG_GUEST_FAULT=1"
set "KYTY_DEBUG_DES_TOUCH_LIST=1"
set "KYTY_DEBUG_DES_TOUCH_TRACE=1"
set "KYTY_DEBUG_DES_TOUCH_SERIALIZE=1"
set "KYTY_DEBUG_DES_TOUCH_FULL_CAPTURE=0"

set "desExe=%~dp0_Runtime\kyty_emulator.exe"
if /i "%~1"=="profile" (
	rem Staged build with Tracy listening on 127.0.0.1:8086. On-demand, so it costs nothing until
	rem a capture client connects.
	set "desExe=%~dp0_Runtime\kyty_emulator-profile.exe"
	set "desArgs=--profiler-direction Network"
)

rem An absolute path, not a bare name: some environments set NoDefaultCurrentDirectoryInExePath,
rem which stops cmd resolving an executable from the working directory even when it is right there.
if not exist "%desExe%" (
	echo Missing "%desExe%".
	echo Build with _Build\build-demons-souls.ps1 and install, or stage a profile build first.
	pause
	exit /b 1
)

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

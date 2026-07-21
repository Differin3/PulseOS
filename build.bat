@echo off
setlocal EnableExtensions EnableDelayedExpansion
cd /d "%~dp0"

rem Usage: build.bat [clean|force]
rem   force — recompile network stack (tcp/nic/http) before make all
rem Uses default WSL distribution (Ubuntu marked with * in wsl -l -v)

set "TARGET=all"
set "PREMAKE="

if /i "%~1"=="clean" set "TARGET=clean"
if /i "%~1"=="force" (
    rem WSL on /mnt/c often skips recompile (clock skew). Drop stale network .o files.
    set "PREMAKE=rm -f kernel/drivers/network/protocols/tcp.o kernel/drivers/network/nic.o kernel/drivers/network/http_server.o kernel/drivers/network/http_protocol.o kernel/drivers/network/http_gzip.o kernel/drivers/network/socket.o kernel/drivers/network/network_config.o kernel/kernel.o &&"
    shift
)

set "WSL_CMD=wsl"
echo [build] Using default WSL distribution.

call :check_wsl
if !RC! neq 0 goto :failed

echo [build] Resolving project path...
set "WSL_DIR="
for /f "usebackq delims=" %%P in (`wsl wslpath -a "%CD%" 2^>nul`) do set "WSL_DIR=%%P"
if not defined WSL_DIR goto :failed

echo [build] make %TARGET% in !WSL_DIR!
if defined PREMAKE echo [build] force: network objects will be rebuilt
wsl -e sh -c "cd '!WSL_DIR!' && !PREMAKE! make %TARGET%"
set "RC=!errorlevel!"
if !RC! neq 0 goto :failed

if /i "%TARGET%"=="all" (
    if not exist "%~dp0iso\boot\kernel.bin" (
        echo [build] kernel.bin was not created.
        goto :failed
    )
    if not exist "%~dp0myos.iso" (
        echo [build] myos.iso was not created.
        goto :failed
    )
)

echo.
echo [build] Success.
exit /b 0

:check_wsl
wsl -e /bin/true
set "RC=!errorlevel!"
exit /b 0

:failed
echo.
echo [build] FAILED.
echo.
echo Try building manually inside Ubuntu:
echo   wsl
echo   cd "/mnt/c/Users/d.mordvinov/Documents/my os c++"
echo   make
echo.
echo List distros: wsl -l -v
echo.
exit /b 1

@echo off
setlocal EnableExtensions EnableDelayedExpansion

echo === Installed WSL distributions ===
wsl -l -v
echo.

rem Prefer default WSL (works when "wsl -d Ubuntu" fails due to name encoding)
set "WSL_CMD=wsl"
set "DISTRO_LABEL=default"

wsl -e /bin/true >nul 2>&1
set "RC=!errorlevel!"
if !RC! neq 0 (
    echo [setup] Default WSL is not available.
    echo Open Ubuntu from Start menu once, then retry.
    exit /b 1
)

echo [setup] Using WSL: %DISTRO_LABEL%
echo.

echo [setup] Updating package lists and installing build tools...
echo       Enter your Ubuntu password when asked (characters are hidden).
echo.

wsl -e bash -lc "sudo apt update && sudo apt install -y nasm gcc-multilib g++-multilib binutils grub-pc-bin xorriso make"
set "RC=!errorlevel!"
if !RC! neq 0 (
    echo.
    echo [setup] FAILED. Run manually inside Ubuntu:
    echo   wsl
    echo   sudo apt update
    echo   sudo apt install -y nasm gcc-multilib g++-multilib binutils grub-pc-bin xorriso make
    exit /b 1
)

echo.
echo [setup] Done. Build the kernel from cmd:
echo   cd "%~dp0"
echo   build.bat
echo.
echo Or inside Ubuntu:
echo   cd "/mnt/c/Users/d.mordvinov/Documents/my os c++"
echo   make
echo.
exit /b 0

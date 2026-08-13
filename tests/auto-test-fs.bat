@echo off
setlocal EnableExtensions EnableDelayedExpansion
cd /d "%~dp0\.."

if not exist "logs" mkdir "logs"

echo ============================================================
echo   KnitOS — automated filesystem test (CMD -^> WSL -^> QEMU)
echo ============================================================
echo.
echo   From CMD in project folder:
echo     tests\auto-test-fs.bat
echo     tests\auto-test-fs.bat --skip-build
echo     tests\auto-test-fs.bat --rebuild
echo.
echo   Guest command ^(manual^):
echo     autotest fs
echo.
echo   Also runs inside: tests\auto-test.bat  ^(before network/HTTP^)
echo.
echo   Logs:  logs\auto-fs-test.log   logs\qemu-serial.log
echo.

if not defined MYOS_VERBOSE set "MYOS_VERBOSE=1"
if not defined MYOS_HEADLESS set "MYOS_HEADLESS=1"
if not defined MYOS_NIC set "MYOS_NIC=virtio"

set "WSL_DIR="
for /f "usebackq delims=" %%P in (`wsl wslpath -a "%CD%" 2^>nul`) do set "WSL_DIR=%%P"
if not defined WSL_DIR (
    echo [FAIL] WSL not available. Run setup-wsl.bat or install Ubuntu WSL.
    exit /b 1
)

echo [auto-test-fs] WSL: !WSL_DIR!
echo.

wsl sed -i "s/\r$//" "!WSL_DIR!/tests/fs/auto-fs-test.sh" 2>nul

wsl /usr/bin/env MYOS_NIC=!MYOS_NIC! MYOS_VERBOSE=!MYOS_VERBOSE! MYOS_HEADLESS=!MYOS_HEADLESS! /usr/bin/bash "!WSL_DIR!/tests/fs/auto-fs-test.sh" %*
set "RC=!errorlevel!"

echo.
if !RC! equ 0 (
    echo [auto-test-fs] PASSED — see logs\auto-fs-test.log
) else (
    echo [auto-test-fs] FAILED ^(!RC!^) — see logs\auto-fs-test.log and logs\qemu-serial.log
)
exit /b !RC!

@echo off
setlocal EnableExtensions EnableDelayedExpansion
cd /d "%~dp0"

if not exist "myos.iso" (
    echo [run-qemu] myos.iso not found. Run build.bat first.
    exit /b 1
)

if not exist "logs" mkdir "logs"

set "LOG_WIN=%~dp0logs\qemu-serial.log"
echo [run-qemu] Serial log: %LOG_WIN%

set "WSL_ISO="
set "WSL_LOG="
for /f "usebackq delims=" %%P in (`wsl wslpath -a "%CD%\myos.iso" 2^>nul`) do set "WSL_ISO=%%P"
for /f "usebackq delims=" %%P in (`wsl wslpath -a "%LOG_WIN%" 2^>nul`) do set "WSL_LOG=%%P"

if not defined WSL_ISO (
    echo [run-qemu] Failed to resolve paths via WSL.
    exit /b 1
)

echo [run-qemu] Starting QEMU (RTL8139 + user networking)...
echo [run-qemu] NOTE: ISO only - no writable disk. For FS tests use run-qemu-ahci.bat
echo [run-qemu] Close QEMU window to stop. Open logs\qemu-serial.log to read debug output.

wsl qemu-system-i386 -cdrom "!WSL_ISO!" -netdev user,id=net0 -device rtl8139,netdev=net0 -serial file:"!WSL_LOG!"
set "RC=!errorlevel!"

if !RC! neq 0 (
    echo [run-qemu] QEMU exited with code !RC!
    exit /b !RC!
)

echo [run-qemu] Done. Log saved to logs\qemu-serial.log
exit /b 0

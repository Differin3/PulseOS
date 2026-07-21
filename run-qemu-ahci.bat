@echo off
setlocal EnableExtensions EnableDelayedExpansion
cd /d "%~dp0"

if not exist "myos.iso" (
    echo [run-qemu-ahci] myos.iso not found. Run build.bat first.
    exit /b 1
)

if not exist "myos.img" (
    echo [run-qemu-ahci] Creating empty 64MB disk image myos.img ...
    wsl dd if=/dev/zero of=myos.img bs=1M count=64 2>nul
)

if not exist "logs" mkdir "logs"

set "LOG_WIN=%~dp0logs\qemu-serial.log"
echo [run-qemu-ahci] Serial log: %LOG_WIN%

set "WSL_ISO="
set "WSL_IMG="
set "WSL_LOG="
for /f "usebackq delims=" %%P in (`wsl wslpath -a "%CD%\myos.iso" 2^>nul`) do set "WSL_ISO=%%P"
for /f "usebackq delims=" %%P in (`wsl wslpath -a "%CD%\myos.img" 2^>nul`) do set "WSL_IMG=%%P"
for /f "usebackq delims=" %%P in (`wsl wslpath -a "%LOG_WIN%" 2^>nul`) do set "WSL_LOG=%%P"

if not defined WSL_ISO (
    echo [run-qemu-ahci] Failed to resolve paths via WSL.
    exit /b 1
)

echo [run-qemu-ahci] QEMU with ich9-ahci + raw disk (myos.img) ...
wsl qemu-system-i386 -cdrom "!WSL_ISO!" -drive file="!WSL_IMG!",if=none,format=raw,id=disk0 -device ich9-ahci,id=ahci0 -device ide-hd,drive=disk0,bus=ahci0.0 -netdev user,id=net0 -device rtl8139,netdev=net0 -serial file:"!WSL_LOG!"
set "RC=!errorlevel!"
if !RC! neq 0 exit /b !RC!
echo [run-qemu-ahci] Done.
exit /b 0

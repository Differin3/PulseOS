@echo off
setlocal EnableExtensions EnableDelayedExpansion
cd /d "%~dp0"

if not exist "myos.iso" (
    echo [run-qemu-test] myos.iso not found. Run build.bat first.
    exit /b 1
)

if not exist "myos.img" (
    echo [run-qemu-test] Creating empty 64MB disk image myos.img ...
    wsl dd if=/dev/zero of=myos.img bs=1M count=64 2>nul
)

if not exist "logs" mkdir "logs"

set "SERIAL_PORT=4444"
if not "%~1"=="" set "SERIAL_PORT=%~1"

set "LOG_WIN=%~dp0logs\qemu-serial.log"
echo [run-qemu-test] Serial TCP: 127.0.0.1:%SERIAL_PORT% ^(bidirectional COM1^)
echo [run-qemu-test] HTTP hostfwd: 127.0.0.1:8080 -^> guest:8080
echo [run-qemu-test] Log: %LOG_WIN%
echo.
echo   In QEMU guest:  autotest network 8080 16
echo   In 2nd cmd:     tests\network\host-tests.bat 127.0.0.1 8080
echo   Full auto:      test.bat  ^(separate cmd, stops this QEMU first^)
echo.

set "WSL_ISO="
set "WSL_IMG="
for /f "usebackq delims=" %%P in (`wsl wslpath -a "%CD%\myos.iso" 2^>nul`) do set "WSL_ISO=%%P"
for /f "usebackq delims=" %%P in (`wsl wslpath -a "%CD%\myos.img" 2^>nul`) do set "WSL_IMG=%%P"

if not defined WSL_ISO (
    echo [run-qemu-test] Failed to resolve paths via WSL.
    exit /b 1
)

echo [run-qemu-test] QEMU test mode: headless + serial TCP ...
wsl qemu-system-i386 -display gtk -cdrom "!WSL_ISO!" -drive file="!WSL_IMG!",if=none,format=raw,id=disk0 -device ich9-ahci,id=ahci0 -device ide-hd,drive=disk0,bus=ahci0.0 -netdev user,id=net0,hostfwd=tcp::8080-:8080 -device virtio-net-pci,disable-legacy=off,disable-modern=on,netdev=net0 -serial tcp:0.0.0.0:%SERIAL_PORT%,server,nowait
set "RC=!errorlevel!"
exit /b !RC!

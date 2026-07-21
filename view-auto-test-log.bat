@echo off
cd /d "%~dp0"
if not exist "logs\auto-test.log" (
    echo Log not found. Run test.bat or build-test.bat first.
    exit /b 1
)
start "" notepad "%~dp0logs\auto-test.log"
if exist "logs\qemu-serial.log" (
    start "" notepad "%~dp0logs\qemu-serial.log"
)

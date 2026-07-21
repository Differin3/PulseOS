@echo off
cd /d "%~dp0"
if not exist "logs\qemu-serial.log" (
    echo Log file not found. Run run-qemu.bat first.
    exit /b 1
)
echo Opening logs\qemu-serial.log ...
start "" notepad "%~dp0logs\qemu-serial.log"
